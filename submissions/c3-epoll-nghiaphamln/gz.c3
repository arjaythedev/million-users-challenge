// gzip, tuned for one JSON body per call: greedy LZ77 with a single-probe hash table (one lookup per
// position, no chains; matches compared eight bytes at a time), the fixed Huffman block (no tree to
// build, and the feed stays at two TCP segments either way), and ISA-L's CPU-dispatched gzip CRC.
module server;

extern fn uint crc32_gzip_refl(uint initial, char* p, ulong n);

tlocal char* zbuf;
tlocal ushort* gz_head;  // hash -> last position + 1 (0: none); input is capped at 64 KB so positions fit

const usz GZ_MAX_IN = 65535;
const uint GZ_HBITS = 13;

ushort[288] lit_code;    // fixed Huffman literal/length codes, bit-reversed for the LSB-first stream
char[288] lit_bits;
ushort[30] dist_code;    // fixed Huffman distance codes: 5 bits, bit-reversed
char[259] len_sym;       // match length -> length symbol index 0..28
char[257] dist_sym_lo;   // distance -> distance symbol: d <= 256 by d ...
char[257] dist_sym_hi;   // ... else by (d - 1) >> 7
uint[29] len_base = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
	115, 131, 163, 195, 227, 258 };
char[29] len_extra = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
uint[30] dist_base = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
	1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
char[30] dist_extra = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
	12, 13, 13 };

// Huffman codes go out MSB-first inside the LSB-first stream.
fn uint rev(uint v, uint n)
{
	uint r = 0;
	for (uint i = 0; i < n; i++)
	{
		r = r << 1 | (v & 1);
		v >>= 1;
	}
	return r;
}

fn void gz_init()
{
	// RFC 1951 3.2.6: 0-143: 8 bits from 0x30; 144-255: 9 bits from 0x190; 256-279: 7 bits from 0;
	// 280-287: 8 bits from 0xc0; distances: 5 bits
	for (uint s = 0; s < 288; s++)
	{
		uint code;
		uint bits;
		if (s < 144)
		{
			code = 0x30 + s;
			bits = 8;
		}
		else if (s < 256)
		{
			code = 0x190 + s - 144;
			bits = 9;
		}
		else if (s < 280)
		{
			code = s - 256;
			bits = 7;
		}
		else
		{
			code = 0xc0 + s - 280;
			bits = 8;
		}
		lit_code[s] = (ushort)rev(code, bits);
		lit_bits[s] = (char)bits;
	}
	for (uint d = 0; d < 30; d++) dist_code[d] = (ushort)rev(d, 5);
	for (uint c = 0; c < 29; c++)
	{
		for (uint l = len_base[c]; l < 259 && (c == 28 || l < len_base[c + 1]); l++) len_sym[l] = (char)c;
	}
	for (uint c = 0; c < 30; c++)
	{
		uint lo = dist_base[c];
		uint hi = c == 29 ? 32768 : dist_base[c + 1] - 1;
		for (uint d = lo; d <= hi; d++)
		{
			if (d <= 256)
			{
				dist_sym_lo[d] = (char)c;
			}
			else
			{
				dist_sym_hi[(d - 1) >> 7] = (char)c;
			}
		}
	}
}

struct Bits
{
	char* out;
	usz n;
	ulong acc;
	uint cnt;
}

// At most 32 bits per call.
fn void Bits.put(&self, uint code, uint len) @inline
{
	self.acc |= (ulong)code << self.cnt;
	self.cnt += len;
	if (self.cnt >= 32)
	{
		uint w = (uint)self.acc;
		memcpy(self.out + self.n, &w, 4);
		self.n += 4;
		self.acc >>= 32;
		self.cnt -= 32;
	}
}

fn void Bits.flush(&self)
{
	while (self.cnt > 0)
	{
		self.out[self.n++] = (char)self.acc;
		self.acc >>= 8;
		self.cnt = self.cnt > 8 ? self.cnt - 8 : 0;
	}
}

fn void Bits.sym(&self, uint s) @inline
{
	self.put(lit_code[s], lit_bits[s]);
}

fn void Bits.match(&self, uint len, uint dist) @inline
{
	uint ls = len_sym[len];
	self.sym(257 + ls);
	if (len_extra[ls] != 0) self.put(len - len_base[ls], len_extra[ls]);
	uint ds = dist <= 256 ? dist_sym_lo[dist] : dist_sym_hi[(dist - 1) >> 7];
	self.put(dist_code[ds], 5);
	if (dist_extra[ds] != 0) self.put(dist - dist_base[ds], dist_extra[ds]);
}

// Four bytes hashed: a 4-byte minimum match loses a few 3-byte matches and skips many false candidates.
fn uint gz_hash(char* p) @inline
{
	uint v;
	memcpy(&v, p, 4);
	return (v * 2654435761u) >> (32 - GZ_HBITS);
}

// Length of the common prefix of a and b, at most max (>= 4 is known), eight bytes at a time.
fn usz match_len(char* a, char* b, usz max) @inline
{
	usz len = 4;
	while (len + 8 <= max)
	{
		ulong x;
		ulong y;
		memcpy(&x, a + len, 8);
		memcpy(&y, b + len, 8);
		if (x != y) return len + (usz)((x ^ y).ctz() >> 3);  // little-endian: the first differing byte is the lowest set bit
		len += 8;
	}
	while (len < max && a[len] == b[len]) len++;
	return len;
}

// Compresses in[0..n) into out (cap bytes). Returns the gzip size, or 0 if it doesn't fit or n is too
// large.
fn usz gzip(char* in, usz n, char* out, usz cap)
{
	// the worst case (all literals, 9 bits each) is under n * 9/8 + 32
	if (n > GZ_MAX_IN || cap < n + n / 8 + 32) return 0;
	if (gz_head == null) gz_head = calloc(1u << GZ_HBITS, 2);
	memset(gz_head, 0, (1u << GZ_HBITS) * 2);
	char[10] gzhdr = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
	memcpy(out, &gzhdr, 10);
	Bits b = { out, 10, 0, 0 };
	b.put(1, 1);  // BFINAL
	b.put(1, 2);  // BTYPE=01 (fixed Huffman)
	usz i = 0;
	usz last = n >= 4 ? n - 4 : 0;  // last position with four bytes to hash
	while (i <= last)
	{
		uint h = gz_hash(in + i);
		usz cand = gz_head[h];
		gz_head[h] = (ushort)(i + 1);
		if (cand != 0)
		{
			cand--;
			uint x;
			uint y;
			memcpy(&x, in + cand, 4);
			memcpy(&y, in + i, 4);
			if (x == y && i - cand <= 32768)
			{
				usz len = match_len(in + cand, in + i, n - i < 258 ? n - i : 258);
				b.match((uint)len, (uint)(i - cand));
				i += len;
				continue;
			}
		}
		b.sym(in[i]);
		i++;
	}
	while (i < n)
	{
		b.sym(in[i]);
		i++;
	}
	b.sym(256);
	b.flush();
	uint crc = crc32_gzip_refl(0, in, n);
	uint isz = (uint)n;
	memcpy(out + b.n, &crc, 4);  // little-endian on every target we build for
	memcpy(out + b.n + 4, &isz, 4);
	return b.n + 8;
}
