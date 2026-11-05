package main

import (
	"encoding/binary"
	"hash/crc32"
	"math/bits"
	"sync"
)

// gzipAppend appends a gzip member holding body to dst.
//
// Responses are a few KiB of JSON. For inputs that small a general-purpose
// compressor spends most of its time building and choosing Huffman tables for
// each response, not finding matches. This encoder writes one DEFLATE block
// with the fixed Huffman codes of RFC 1951 (section 3.2.6), so there are no
// tables to build: it finds repeats with a single-probe hash table (greedy
// LZ77, as gzip -1 does) and emits each literal or match with a table lookup.
// The output is standard gzip that any decoder reads.
func gzipAppend(dst, body []byte) []byte {
	e := encoders.Get().(*fastEncoder)
	dst = append(dst, gzipHeader[:]...)
	dst = e.deflate(dst, body)
	dst = binary.LittleEndian.AppendUint32(dst, crc32.ChecksumIEEE(body))
	dst = binary.LittleEndian.AppendUint32(dst, uint32(len(body)))
	encoders.Put(e)
	return dst
}

// gzipHeader: magic, CM=8 (deflate), no flags, no mtime, XFL=4 (fastest), OS=255 (unknown).
var gzipHeader = [10]byte{0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 4, 255}

const (
	hashBits   = 13
	minMatch   = 4
	maxMatch   = 258
	windowSize = 1 << 15
)

var encoders = sync.Pool{New: func() any { return new(fastEncoder) }}

// fastEncoder holds the match table between calls. Entries store a position
// plus base; raising base by more than the input length on each call makes
// every older entry fall below it, so the table never needs clearing.
type fastEncoder struct {
	table [1 << hashBits]int32
	base  int32
	acc   uint64
	nacc  uint
	out   []byte
}

func hash4(u uint32) uint32 { return (u * 0x1e35a7bd) >> (32 - hashBits) }

func (e *fastEncoder) deflate(dst, src []byte) []byte {
	if len(src) > 1<<30 || e.base > 1<<30 {
		e.table = [1 << hashBits]int32{}
		e.base = 0
	}
	base := e.base + 1
	e.base += int32(len(src)) + 1
	e.out, e.acc, e.nacc = dst, 0, 0
	e.put(3, 3) // BFINAL=1, BTYPE=01 (fixed Huffman)

	n := len(src)
	literals := 0
	for i := 0; i+minMatch <= n; {
		current := binary.LittleEndian.Uint32(src[i:])
		h := hash4(current)
		candidate := int(e.table[h] - base)
		e.table[h] = int32(i) + base
		if candidate >= 0 && i-candidate <= windowSize && binary.LittleEndian.Uint32(src[candidate:]) == current {
			length := minMatch + matchLength(src[candidate+minMatch:], src[i+minMatch:], maxMatch-minMatch)
			e.literals(src[literals:i])
			e.match(length, i-candidate)
			i += length
			literals = i
			// Index the position just before the next one so a run of
			// repeats is found again at once.
			if i-1+minMatch <= n {
				e.table[hash4(binary.LittleEndian.Uint32(src[i-1:]))] = int32(i-1) + base
			}
			continue
		}
		// Skip ahead faster through data that has not matched for a while.
		i += 1 + (i-literals)>>5
	}
	e.literals(src[literals:])
	e.put(0, 7) // end of block (symbol 256)
	e.flush()
	dst = e.out
	e.out = nil
	return dst
}

// matchLength counts the leading bytes a and b share, up to limit.
func matchLength(a, b []byte, limit int) int {
	if len(b) < limit {
		limit = len(b)
	}
	n := 0
	for n+8 <= limit {
		if x := binary.LittleEndian.Uint64(a[n:]) ^ binary.LittleEndian.Uint64(b[n:]); x != 0 {
			return n + bits.TrailingZeros64(x)>>3
		}
		n += 8
	}
	for n < limit && a[n] == b[n] {
		n++
	}
	return n
}

func (e *fastEncoder) put(value uint64, n uint) {
	e.acc |= value << e.nacc
	e.nacc += n
	if e.nacc >= 32 {
		e.out = binary.LittleEndian.AppendUint32(e.out, uint32(e.acc))
		e.acc >>= 32
		e.nacc -= 32
	}
}

func (e *fastEncoder) flush() {
	for e.nacc > 0 {
		e.out = append(e.out, byte(e.acc))
		e.acc >>= 8
		if e.nacc >= 8 {
			e.nacc -= 8
		} else {
			e.nacc = 0
		}
	}
	e.acc = 0
}

func (e *fastEncoder) literals(s []byte) {
	for _, c := range s {
		code := literalCodes[c]
		e.put(uint64(code&0xffff), uint(code>>16))
	}
}

// match emits a length/distance pair: the length symbol and its extra bits,
// then the distance code and its extra bits.
func (e *fastEncoder) match(length, distance int) {
	code := lengthCodes[length-minMatch]
	e.put(uint64(code&0xffff), uint(code>>16))
	d := uint32(distance - 1)
	var symbol, extra, extraBits uint32
	if d < 4 {
		symbol = d
	} else {
		top := uint32(bits.Len32(d)) - 1
		extraBits = top - 1
		symbol = 2*top + (d>>extraBits)&1
		extra = d & (1<<extraBits - 1)
	}
	e.put(uint64(reverseBits(symbol, 5))|uint64(extra)<<5, uint(5+extraBits))
}

// literalCodes and lengthCodes pack each fixed code, bit-reversed for the
// LSB-first bit stream and followed by any extra bits, in the low 16 bits and
// its total bit count in the high bits.
var literalCodes, lengthCodes = fixedCodes()

func fixedCodes() (literal [256]uint32, length [maxMatch - minMatch + 1]uint32) {
	for c := 0; c < 256; c++ {
		if c < 144 {
			literal[c] = 8<<16 | reverseBits(uint32(0x30+c), 8)
		} else {
			literal[c] = 9<<16 | reverseBits(uint32(0x190+c-144), 9)
		}
	}
	// Length symbols 257-285: base lengths and extra bit counts (RFC 1951, 3.2.5).
	bases := [29]int{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258}
	extras := [29]uint32{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0}
	for l := minMatch; l <= maxMatch; l++ {
		s := 28
		for bases[s] > l {
			s--
		}
		symbol := uint32(257 + s)
		var code, n uint32
		if symbol < 280 {
			code, n = reverseBits(symbol-256, 7), 7
		} else {
			code, n = reverseBits(0xc0+symbol-280, 8), 8
		}
		length[l-minMatch] = (n+extras[s])<<16 | code | uint32(l-bases[s])<<n
	}
	return
}

func reverseBits(v uint32, n uint32) uint32 { return bits.Reverse32(v) >> (32 - n) }
