package main

// A gzip encoder that knows what it is compressing. General compressors spend
// their time rediscovering structure and counting symbols; here both are known
// before the first byte:
//
//   - Every feed row repeats the same keys, so the encoder never searches for
//     them: the JSON writer records where each row's fields are, and each key
//     becomes a back-reference to the same key one row up.
//   - The Huffman tables never change. They are built once, at startup, from
//     the letter statistics of JSON-wrapped English, and that one DEFLATE block
//     header is copied in front of every response. Nothing is counted per
//     response and no tree is built; a symbol is one table lookup.
//   - Post bodies get a small LZ77 that only looks at word starts, which is
//     where text repeats.
//
// The output is ordinary gzip that any decoder reads.

import (
	"encoding/binary"
	"hash/crc32"
	"math/bits"
)

// rowMark records where one post object's variable fields sit in the JSON.
type rowMark struct {
	start     int32 // the row's opening brace
	bodyStart int32 // first byte inside the body's quotes
	bodyEnd   int32 // the body's closing quote
	nameStart int32
	nameEnd   int32 // the author's closing quote
	end       int32 // one past the row's closing brace
}

const (
	keyID      = len(`{"id":`)
	keyBody    = len(`,"body":"`)
	keyCreated = len(`","created_at":"`)
	timestamp  = len(`2025-12-31T23:59:19.409Z`)
	keyAuthor  = len(`Z","author":"`)
	keyLikes   = len(`","like_count":`)

	maxLitBits = 14 // four literals, plus 7 leftover bits, fit the 64-bit buffer
	lzTabBits  = 10
)

type symbol struct {
	code uint32 // bit-reversed, ready to shift into the stream
	n    uint32
}

var (
	litEnc   [256]uint32 // code<<8 | bits
	lenEnc   [259]uint32 // by match length: code+extra<<5 | bits (see init)
	distSyms [30]symbol
	eobEnc   symbol
	gzPrefix []byte // gzip header and the whole bytes of the block header
	gzTail   uint64 // the block header's last bits
	gzTailN  uint
)

// literalWeights models the literal stream left once keys are back-references:
// lowercase prose and usernames, and the digits of ids, timestamps and counts,
// which in API JSON are as common as letters.
func literalWeights() []uint32 {
	w := make([]uint32, 286)
	for c := 0x20; c < 0x7f; c++ {
		w[c] = 2 // every printable byte has a code
	}
	for c := 0x7f; c < 0x100; c++ {
		w[c] = 1 // UTF-8 text must encode
	}
	// Bytes below 0x20 keep no code: JSON escapes them.
	for i, f := range [...]uint32{ // a-z
		72, 13, 26, 33, 100, 19, 17, 38, 64, 1, 6, 37, 22, 62, 68, 16, 1, 56, 58, 78, 24, 9, 15, 2, 17, 1,
	} {
		w['a'+i] = f * 6
		w['A'+i] = f/8 + 2
	}
	for c := '0'; c <= '9'; c++ {
		w[c] = 350
	}
	for _, p := range []struct {
		c byte
		w uint32
	}{{' ', 700}, {'.', 90}, {',', 60}, {'_', 90}, {'?', 12}, {'!', 12}, {'-', 40}, {':', 50},
		{'"', 40}, {'{', 4}, {'}', 90}, {'[', 2}, {']', 3}, {'(', 15}, {')', 15}, {'/', 15}, {'\'', 15}} {
		w[p.c] = p.w
	}
	w[256] = 2 // end of block
	// Length symbols: words are 4-12 bytes; keys run 6, 9, 13 and 15-40.
	for s := 257; s < 286; s++ {
		switch {
		case s <= 258: // 3-4
			w[s] = 60
		case s <= 262: // 5-8
			w[s] = 500
		case s <= 266: // 9-12
			w[s] = 800
		case s <= 272: // 13-30
			w[s] = 500
		case s <= 276: // 31-66
			w[s] = 40
		default:
			w[s] = 1
		}
	}
	return w
}

func distWeights() []uint32 {
	w := make([]uint32, 30)
	for s := range w {
		switch {
		case s >= 8 && s <= 13: // 17-128: inside a row
			w[s] = 120
		case s >= 14 && s <= 17: // 129-512: the same key, one row up
			w[s] = 800
		case s >= 18 && s <= 22: // 513-4096: a word from further up
			w[s] = 200
		default:
			w[s] = 2
		}
	}
	return w
}

// huffLengths builds code lengths no longer than limit: Huffman's algorithm on
// the weights, flattened until the longest code fits. Startup only.
func huffLengths(freq []uint32, limit int) []uint8 {
	f := append([]uint32(nil), freq...)
	type node struct {
		w      uint64
		leaves []int
	}
	for {
		lengths := make([]uint8, len(f))
		var q []node
		for i, v := range f {
			if v > 0 {
				q = append(q, node{uint64(v), []int{i}})
			}
		}
		for len(q) > 1 {
			a, b := 0, 1
			if q[b].w < q[a].w {
				a, b = b, a
			}
			for i := 2; i < len(q); i++ {
				if q[i].w < q[a].w {
					a, b = i, a
				} else if q[i].w < q[b].w {
					b = i
				}
			}
			m := node{q[a].w + q[b].w, append(append([]int(nil), q[a].leaves...), q[b].leaves...)}
			for _, l := range m.leaves {
				lengths[l]++
			}
			if a < b {
				a, b = b, a
			}
			q[a] = q[len(q)-1]
			q = q[:len(q)-1]
			q[b] = q[len(q)-1]
			q = q[:len(q)-1]
			q = append(q, m)
		}
		longest := 0
		for _, l := range lengths {
			longest = max(longest, int(l))
		}
		if longest <= limit {
			return lengths
		}
		for i, v := range f {
			if v > 0 {
				f[i] = v/2 + 1
			}
		}
	}
}

// canonical assigns the RFC 1951 codes, bit-reversed for the LSB-first stream.
func canonical(lengths []uint8) []uint32 {
	var count [16]uint32
	for _, l := range lengths {
		count[l]++
	}
	count[0] = 0
	var next [16]uint32
	code := uint32(0)
	for b := 1; b < 16; b++ {
		code = (code + count[b-1]) << 1
		next[b] = code
	}
	codes := make([]uint32, len(lengths))
	for i, l := range lengths {
		if l > 0 {
			codes[i] = bits.Reverse32(next[l]) >> (32 - uint(l))
			next[l]++
		}
	}
	return codes
}

// bitWriter is the startup-time writer for the block header.
type bitWriter struct {
	b    []byte
	acc  uint64
	used uint
}

func (w *bitWriter) put(v uint32, n uint) {
	w.acc |= uint64(v) << w.used
	w.used += n
	for w.used >= 8 {
		w.b = append(w.b, byte(w.acc))
		w.acc >>= 8
		w.used -= 8
	}
}

var (
	lenBase  = [29]int{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258}
	lenExtra = [29]uint{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0}
	dstBase  = [30]int{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577}
	dstExtra = [30]uint{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13}
)

func init() {
	ll := huffLengths(literalWeights(), maxLitBits)
	dl := huffLengths(distWeights(), 15)
	lc := canonical(ll)
	dc := canonical(dl)
	for i := 0; i < 256; i++ {
		litEnc[i] = lc[i]<<8 | uint32(ll[i])
	}
	eobEnc = symbol{lc[256], uint32(ll[256])}
	for s := 28; s >= 0; s-- {
		n := uint32(ll[257+s])
		for l := lenBase[s]; l <= 258 && (s == 28 || l < lenBase[s+1]); l++ {
			// 14 code bits + 5 extra bits leave room for the 5-bit count.
			lenEnc[l] = (lc[257+s]|uint32(l-lenBase[s])<<n)<<5 | (n + uint32(lenExtra[s]))
		}
	}
	lenEnc[258] = lc[285]<<5 | uint32(ll[285])
	for s := range distSyms {
		distSyms[s] = symbol{dc[s], uint32(dl[s])}
	}
	buildBlockHeader(ll, dl)
}

// buildBlockHeader serializes the one dynamic-Huffman header (RFC 1951 3.2.7)
// behind the gzip header.
func buildBlockHeader(ll, dl []uint8) {
	seq := append(append([]uint8(nil), ll...), dl...)
	type rle struct {
		sym   uint8
		extra uint32
		n     uint
	}
	var toks []rle
	for i := 0; i < len(seq); {
		v := seq[i]
		run := 1
		for i+run < len(seq) && seq[i+run] == v {
			run++
		}
		i += run
		for run > 0 {
			switch {
			case v == 0 && run >= 11:
				n := min(run, 138)
				toks = append(toks, rle{18, uint32(n - 11), 7})
				run -= n
			case v == 0 && run >= 3:
				toks = append(toks, rle{17, uint32(run - 3), 3})
				run = 0
			case v != 0 && run >= 4:
				toks = append(toks, rle{v, 0, 0})
				run--
				for run >= 3 {
					n := min(run, 6)
					toks = append(toks, rle{16, uint32(n - 3), 2})
					run -= n
				}
			default:
				toks = append(toks, rle{v, 0, 0})
				run--
			}
		}
	}
	freq := make([]uint32, 19)
	for _, t := range toks {
		freq[t.sym]++
	}
	cl := huffLengths(freq, 7)
	cc := canonical(cl)
	order := []int{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15}
	hclen := len(order)
	for hclen > 4 && cl[order[hclen-1]] == 0 {
		hclen--
	}
	w := &bitWriter{b: []byte{0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff}}
	w.put(1, 1) // final block
	w.put(2, 2) // dynamic Huffman
	w.put(uint32(len(ll)-257), 5)
	w.put(uint32(len(dl)-1), 5)
	w.put(uint32(hclen-4), 4)
	for _, s := range order[:hclen] {
		w.put(uint32(cl[s]), 3)
	}
	for _, t := range toks {
		w.put(cc[t.sym], uint(cl[t.sym]))
		if t.n > 0 {
			w.put(t.extra, t.n)
		}
	}
	gzPrefix, gzTail, gzTailN = w.b, w.acc, w.used
}

// gzEncoder compresses into its own buffer, leaving hdrRoom in front.
type gzEncoder struct {
	out   []byte
	o     int
	acc   uint64
	nb    uint
	table [1 << lzTabBits]uint16 // word hash -> position + 1, this response only
}

func newGzEncoder() *gzEncoder { return &gzEncoder{out: make([]byte, 64<<10)} }

// lits emits literals, four per bit-buffer flush.
func (g *gzEncoder) lits(s []byte) {
	out, o, acc, nb := g.out, g.o, g.acc, g.nb
	for len(s) >= 4 {
		a, b, c, d := litEnc[s[0]], litEnc[s[1]], litEnc[s[2]], litEnc[s[3]]
		acc |= uint64(a>>8) << nb
		nb += uint(a & 0xff)
		acc |= uint64(b>>8) << nb
		nb += uint(b & 0xff)
		acc |= uint64(c>>8) << nb
		nb += uint(c & 0xff)
		acc |= uint64(d>>8) << nb
		nb += uint(d & 0xff)
		binary.LittleEndian.PutUint64(out[o:], acc)
		o += int(nb >> 3)
		acc >>= nb &^ 7
		nb &= 7
		s = s[4:]
	}
	for _, c := range s {
		e := litEnc[c]
		acc |= uint64(e>>8) << nb
		nb += uint(e & 0xff)
	}
	binary.LittleEndian.PutUint64(out[o:], acc)
	o += int(nb >> 3)
	acc >>= nb &^ 7
	nb &= 7
	g.o, g.acc, g.nb = o, acc, nb
}

// match emits a back-reference; length is 3-258 and dist at most 32768.
func (g *gzEncoder) match(length, dist int) {
	e := lenEnc[length]
	acc := g.acc | uint64(e>>5)<<g.nb
	nb := g.nb + uint(e&31)

	s := 2*bits.Len32(uint32(dist-1)) - 2
	if dist <= 2 {
		s = dist - 1
	} else if (dist-1)>>(uint(s)/2-1)&1 != 0 {
		s++
	}
	d := distSyms[s]
	acc |= uint64(d.code) << nb
	nb += uint(d.n)
	acc |= uint64(dist-dstBase[s]) << nb
	nb += dstExtra[s]

	binary.LittleEndian.PutUint64(g.out[g.o:], acc)
	g.o += int(nb >> 3)
	g.acc = acc >> (nb &^ 7)
	g.nb = nb & 7
}

// matchLen counts the common prefix of a and b, 8 bytes at a time.
func matchLen(a, b []byte) int {
	n := 0
	for len(b)-n >= 8 && len(a)-n >= 8 {
		if x := binary.LittleEndian.Uint64(a[n:]) ^ binary.LittleEndian.Uint64(b[n:]); x != 0 {
			return n + bits.TrailingZeros64(x)>>3
		}
		n += 8
	}
	for n < len(a) && n < len(b) && a[n] == b[n] {
		n++
	}
	return n
}

// text encodes b[i:end], trying a back-reference at each word start.
func (g *gzEncoder) text(b []byte, i, end int) {
	lit := i
	for end-i >= 4 {
		v := binary.LittleEndian.Uint32(b[i:])
		h := v * 2654435761 >> (32 - lzTabBits)
		c := int(g.table[h]) - 1
		g.table[h] = uint16(i + 1)
		if c >= 0 && i-c <= 32768 && binary.LittleEndian.Uint32(b[c:]) == v {
			n := 4 + matchLen(b[c+4:end], b[i+4:end])
			if n > 258 {
				n = 258
			}
			g.lits(b[lit:i])
			g.match(n, i-c)
			i += n
			lit = i
			continue
		}
		// On to the next word.
		for i < end && !wordBreak[b[i]] {
			i++
		}
		i++
	}
	g.lits(b[lit:end])
}

// wordBreak marks the bytes a new word can start after.
var wordBreak = [256]bool{' ': true, '_': true, '-': true, '(': true}

// key encodes a run of bytes known to equal the bytes dist back.
func (g *gzEncoder) key(b []byte, i, n, dist int) {
	if dist > 32768 {
		g.lits(b[i : i+n])
		return
	}
	g.match(n, dist)
}

// compress gzips body. With rows given, the rows' keys are referenced directly
// and the search only covers the text between them. The result has hdrRoom
// bytes of room in front.
func (g *gzEncoder) compress(body []byte, rows []rowMark) []byte {
	if need := hdrRoom + len(gzPrefix) + len(body)*2 + 64; len(g.out) < need {
		g.out = make([]byte, need*2)
	}
	copy(g.out[hdrRoom:], gzPrefix)
	g.o, g.acc, g.nb = hdrRoom+len(gzPrefix), gzTail, gzTailN
	clear(g.table[:])

	if len(body) > 60000 { // past the position table's reach
		g.lits(body)
	} else {
		pos := 0
		for r := range rows {
			m := &rows[r]
			start, bs, be, ns, ne := int(m.start), int(m.bodyStart), int(m.bodyEnd), int(m.nameStart), int(m.nameEnd)
			if r == 0 {
				g.lits(body[pos : start+keyID])
				g.lits(body[start+keyID : bs])
				g.text(body, bs, be)
				g.lits(body[be : be+keyCreated])
				g.text(body, be+keyCreated, ns-keyAuthor)
				g.lits(body[ns-keyAuthor : ns])
				g.text(body, ns, ne)
				g.lits(body[ne:m.end])
				pos = int(m.end)
				continue
			}
			p := &rows[r-1]
			g.lits(body[pos:start]) // the comma
			g.key(body, start, keyID, start-int(p.start))
			g.lits(body[start+keyID : bs-keyBody])
			g.key(body, bs-keyBody, keyBody, bs-int(p.bodyStart))
			g.text(body, bs, be)

			// The timestamp's date usually repeats, and an earlier word (a
			// time quoted in the body) can cover it whole; the row above is
			// the fallback.
			g.key(body, be, keyCreated, be-int(p.bodyEnd))
			ts := be + keyCreated
			g.text(body, ts, ts+timestamp-1)
			g.key(body, ns-keyAuthor, keyAuthor, ns-int(p.nameStart))
			g.text(body, ns, ne)
			g.key(body, ne, keyLikes, ne-int(p.nameEnd))
			g.lits(body[ne+keyLikes : m.end])
			pos = int(m.end)
		}
		g.text(body, pos, len(body))
	}

	// End of block, pad to a byte, then the gzip trailer.
	acc := g.acc | uint64(eobEnc.code)<<g.nb
	nb := g.nb + uint(eobEnc.n)
	binary.LittleEndian.PutUint64(g.out[g.o:], acc)
	g.o += int(nb+7) >> 3
	binary.LittleEndian.PutUint32(g.out[g.o:], crc32.ChecksumIEEE(body))
	binary.LittleEndian.PutUint32(g.out[g.o+4:], uint32(len(body)))
	return g.out[:g.o+8]
}
