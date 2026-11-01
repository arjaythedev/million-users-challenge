package main

import (
	"encoding/binary"
	"hash/crc32"
	"math/bits"
)

// A small gzip encoder for response bodies: greedy LZ77 with one hash probe, coded with
// DEFLATE's fixed Huffman tables (RFC 1951, block type 01). Building dynamic tables
// cost most of the compression time of a 5 KB feed; fixed codes need no table work and
// no table header, and the compressed feed still fits the same two TCP segments.

const (
	lzHashBits = 13
	lzMinMatch = 4
	lzMaxMatch = 258
	lzWindow   = 32 << 10
)

type deflater struct {
	table [1 << lzHashBits]uint32 // position + base of the last 4-byte sequence per hash
	base  uint32
}

type huffCode struct {
	code uint16 // bit-reversed, ready to write LSB first
	len  uint8
}

var (
	fixedLit  [288]huffCode
	fixedDist [30]huffCode
	// For match lengths 3..258: the length symbol index (0..28) and its extra bits.
	lengthSym [259]uint8
)

var lengthBase = [29]uint16{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258}
var lengthExtra = [29]uint8{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0}
var distBase = [30]uint32{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577}
var distExtra = [30]uint8{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13}

func reverse(code uint16, n uint8) uint16 { return bits.Reverse16(code) >> (16 - n) }

func init() {
	for v := range 288 {
		var c uint16
		var n uint8
		switch {
		case v < 144:
			c, n = uint16(0x30+v), 8
		case v < 256:
			c, n = uint16(0x190+v-144), 9
		case v < 280:
			c, n = uint16(v-256), 7
		default:
			c, n = uint16(0xc0+v-280), 8
		}
		fixedLit[v] = huffCode{reverse(c, n), n}
	}
	for d := range 30 {
		fixedDist[d] = huffCode{reverse(uint16(d), 5), 5}
	}
	for s := range 29 {
		hi := lzMaxMatch
		if s < 28 {
			hi = int(lengthBase[s+1]) - 1
		}
		for l := int(lengthBase[s]); l <= hi; l++ {
			lengthSym[l] = uint8(s)
		}
	}
	lengthSym[lzMaxMatch] = 28
}

func distSym(d uint32) uint8 {
	// Codes 0..3 are distances 1..4; above that two codes per power of two.
	if d <= 4 {
		return uint8(d - 1)
	}
	b := uint8(bits.Len32(d-1)) - 1 // floor(log2(d-1))
	return 2*b + uint8(((d-1)>>(b-1))&1)
}

type bitWriter struct {
	out  []byte
	acc  uint64
	nacc uint
}

// write adds n <= 18 bits. Fewer than 40 bits stay buffered between calls, so the
// 64-bit accumulator never overflows.
func (w *bitWriter) write(v uint64, n uint) {
	w.acc |= v << w.nacc
	w.nacc += n
	if w.nacc >= 40 {
		w.out = binary.LittleEndian.AppendUint64(w.out, w.acc)[:len(w.out)+5]
		w.acc >>= 40
		w.nacc -= 40
	}
}

func (w *bitWriter) flush() {
	for w.nacc > 0 {
		w.out = append(w.out, byte(w.acc))
		w.acc >>= 8
		w.nacc = sub8(w.nacc)
	}
}

func sub8(n uint) uint {
	if n < 8 {
		return 0
	}
	return n - 8
}

func (w *bitWriter) literals(p []byte) {
	for len(p) >= 2 {
		a, b := fixedLit[p[0]], fixedLit[p[1]]
		w.write(uint64(a.code)|uint64(b.code)<<a.len, uint(a.len+b.len))
		p = p[2:]
	}
	if len(p) == 1 {
		a := fixedLit[p[0]]
		w.write(uint64(a.code), uint(a.len))
	}
}

// matchLen returns how many bytes a and b share at the start, at most max.
func matchLen(a, b []byte, max int) int {
	n := 0
	for n+8 <= max {
		x := binary.LittleEndian.Uint64(a[n:]) ^ binary.LittleEndian.Uint64(b[n:])
		if x != 0 {
			return n + bits.TrailingZeros64(x)/8
		}
		n += 8
	}
	for n < max && a[n] == b[n] {
		n++
	}
	return n
}

// appendGzip appends the gzip encoding of src to dst.
func (z *deflater) appendGzip(dst, src []byte) []byte {
	dst = append(dst, 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff)
	w := bitWriter{out: dst}
	w.write(3, 3) // BFINAL=1, BTYPE=01 (fixed Huffman)
	if uint64(z.base)+uint64(len(src))+lzWindow >= 1<<32 {
		z.table, z.base = [1 << lzHashBits]uint32{}, 0
	}
	base := z.base + lzWindow // every stored entry from an earlier call is below base
	n := len(src)
	lit, i := 0, 0
	for i+lzMinMatch <= n {
		cur := binary.LittleEndian.Uint32(src[i:])
		h := (cur * 2654435761) >> (32 - lzHashBits)
		cand := z.table[h]
		z.table[h] = uint32(i) + base
		if cand >= base {
			c := int(cand - base)
			if i-c <= lzWindow && binary.LittleEndian.Uint32(src[c:]) == cur {
				l := lzMinMatch + matchLen(src[c+lzMinMatch:], src[i+lzMinMatch:], min(n-i, lzMaxMatch)-lzMinMatch)
				w.literals(src[lit:i])
				s := lengthSym[l]
				lc := fixedLit[257+int(s)]
				w.write(uint64(lc.code), uint(lc.len))
				if e := lengthExtra[s]; e > 0 {
					w.write(uint64(l-int(lengthBase[s])), uint(e))
				}
				d := uint32(i - c)
				ds := distSym(d)
				dc := fixedDist[ds]
				w.write(uint64(dc.code), uint(dc.len))
				if e := distExtra[ds]; e > 0 {
					w.write(uint64(d-distBase[ds]), uint(e))
				}
				i += l
				lit = i
				continue
			}
		}
		i++
	}
	w.literals(src[lit:])
	eob := fixedLit[256]
	w.write(uint64(eob.code), uint(eob.len))
	w.flush()
	z.base = base + uint32(n)
	dst = binary.LittleEndian.AppendUint32(w.out, crc32.ChecksumIEEE(src))
	return binary.LittleEndian.AppendUint32(dst, uint32(n))
}
