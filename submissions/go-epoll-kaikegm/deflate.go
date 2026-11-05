package main

import (
	"encoding/binary"
	"hash/crc32"
	"math/bits"
	"slices"
)

// A small gzip encoder for response bodies: greedy LZ77 with one hash probe, then one
// DEFLATE block (RFC 1951) coded with Huffman tables built for that body, or with the
// fixed tables when those come out smaller. Dynamic tables make a feed about 13% smaller
// than fixed codes; the LZ77 pass, not the table work, is most of the time.

const (
	lzHashBits = 13
	lzMinMatch = 4
	lzMaxMatch = 258
	lzWindow   = 32 << 10
)

type deflater struct {
	table [1 << lzHashBits]uint32 // position + base of the last 4-byte sequence per hash
	base  uint32
	// One body's LZ77 matches: literals before it | length<<24 | distance<<33.
	toks     []uint64
	litFreq  [286]uint32
	distFreq [30]uint32
	lit      [286]huffCode
	dist     [30]huffCode
	litLen   [286]uint8
	distLen  [30]uint8
	sorted   []uint64
	depth    [286]int // buildLengths scratch
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

// write adds n <= 32 bits. Fewer than 32 bits stay buffered between calls, so the
// 64-bit accumulator never overflows.
func (w *bitWriter) write(v uint64, n uint) {
	w.acc |= v << w.nacc
	w.nacc += n
	if w.nacc >= 32 {
		w.out = binary.LittleEndian.AppendUint32(w.out, uint32(w.acc))
		w.acc >>= 32
		w.nacc -= 32
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

func (z *deflater) appendGzip(dst, src []byte, fastFixed bool) []byte {
	if fastFixed {
		return z.appendGzipFast(dst, src)
	}
	dst = append(dst, 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff)
	w := bitWriter{out: dst}
	z.tokenize(src)
	if z.dynamicTables(&w) {
		z.writeTokens(&w, src, &z.lit, &z.dist)
	} else {
		w.write(3, 3) // BFINAL=1, BTYPE=01 (fixed Huffman)
		z.writeTokens(&w, src, (*[286]huffCode)(fixedLit[:286]), &fixedDist)
	}
	w.flush()
	dst = binary.LittleEndian.AppendUint32(w.out, crc32.ChecksumIEEE(src))
	return binary.LittleEndian.AppendUint32(dst, uint32(len(src)))
}

// appendGzipFast emits RFC 1951 fixed Huffman directly in a single pass over src without intermediate tokens.
func (z *deflater) appendGzipFast(dst, src []byte) []byte {
	dst = append(dst, 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff)
	w := bitWriter{out: dst}
	w.write(3, 3) // BFINAL=1, BTYPE=01 (RFC 1951 Fixed Huffman)

	if uint64(z.base)+uint64(len(src))+lzWindow >= 1<<32 {
		z.table, z.base = [1 << lzHashBits]uint32{}, 0
	}
	base := z.base + lzWindow
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
				// Emite literais acumulados antes do match (2 por vez)
				p := src[lit:i]
				for len(p) >= 2 {
					a, b := fixedLit[p[0]], fixedLit[p[1]]
					w.write(uint64(a.code)|uint64(b.code)<<a.len, uint(a.len+b.len))
					p = p[2:]
				}
				if len(p) == 1 {
					a := fixedLit[p[0]]
					w.write(uint64(a.code), uint(a.len))
				}

				l := lzMinMatch + matchLen(src[c+lzMinMatch:], src[i+lzMinMatch:], min(n-i, lzMaxMatch)-lzMinMatch)
				d := uint32(i - c)

				s := lengthSym[l]
				lc := fixedLit[257+int(s)]
				w.write(uint64(lc.code)|uint64(l-int(lengthBase[s]))<<lc.len, uint(lc.len+lengthExtra[s]))

				ds := distSym(d)
				dc := fixedDist[ds]
				w.write(uint64(dc.code)|uint64(d-distBase[ds])<<dc.len, uint(dc.len+distExtra[ds]))

				i += l
				lit = i
				continue
			}
		}
		i++
	}
	z.base = base + uint32(n)

	// Emite literais finais restantes + EOB (256)
	p := src[lit:]
	for len(p) >= 2 {
		a, b := fixedLit[p[0]], fixedLit[p[1]]
		w.write(uint64(a.code)|uint64(b.code)<<a.len, uint(a.len+b.len))
		p = p[2:]
	}
	if len(p) == 1 {
		a := fixedLit[p[0]]
		w.write(uint64(a.code), uint(a.len))
	}
	eob := fixedLit[256]
	w.write(uint64(eob.code), uint(eob.len))
	w.flush()

	dst = binary.LittleEndian.AppendUint32(w.out, crc32.ChecksumIEEE(src))
	return binary.LittleEndian.AppendUint32(dst, uint32(len(src)))
}

// tokenize runs greedy LZ77 over src into z.toks and counts the symbols they need.
func (z *deflater) tokenize(src []byte) {
	z.toks = z.toks[:0]
	z.litFreq, z.distFreq = [286]uint32{}, [30]uint32{}
	z.litFreq[256] = 1 // end of block
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
				for _, b := range src[lit:i] {
					z.litFreq[b]++
				}
				d := uint32(i - c)
				z.toks = append(z.toks, uint64(i-lit)|uint64(l)<<24|uint64(d)<<33)
				z.litFreq[257+int(lengthSym[l])]++
				z.distFreq[distSym(d)]++
				i += l
				lit = i
				continue
			}
		}
		i++
	}
	for _, b := range src[lit:] {
		z.litFreq[b]++
	}
	z.base = base + uint32(n)
}

// dynamicTables builds Huffman tables for the counted symbols and, if a block coded with
// them (header included) is smaller than with the fixed tables, writes that block's header.
func (z *deflater) dynamicTables(w *bitWriter) bool {
	z.sorted = buildLengths(z.litFreq[:], z.litLen[:], 15, z.sorted, &z.depth)
	z.sorted = buildLengths(z.distFreq[:], z.distLen[:], 15, z.sorted, &z.depth)
	hlit, hdist := 286, 30
	for hlit > 257 && z.litLen[hlit-1] == 0 {
		hlit--
	}
	for hdist > 1 && z.distLen[hdist-1] == 0 {
		hdist--
	}
	// The code lengths of both tables, run-length coded with symbols 0..18.
	var all [286 + 30]uint8
	n := copy(all[:], z.litLen[:hlit])
	n += copy(all[n:], z.distLen[:hdist])
	var rle [286 + 30]uint16 // symbol | extra bits value << 5
	nr := 0
	var clFreq [19]uint32
	emit := func(sym, extra int) {
		rle[nr] = uint16(sym | extra<<5)
		nr++
		clFreq[sym]++
	}
	for i := 0; i < n; {
		l := all[i]
		run := 1
		for i+run < n && all[i+run] == l {
			run++
		}
		i += run
		if l == 0 {
			for ; run >= 11; run -= min(run, 138) {
				emit(18, min(run, 138)-11)
			}
			if run >= 3 {
				emit(17, run-3)
				run = 0
			}
		} else {
			emit(int(l), 0)
			run--
			for ; run >= 3; run -= min(run, 6) {
				emit(16, min(run, 6)-3)
			}
		}
		for ; run > 0; run-- {
			emit(int(l), 0)
		}
	}
	var clLen [19]uint8
	z.sorted = buildLengths(clFreq[:], clLen[:], 7, z.sorted, &z.depth)
	hclen := 19
	for hclen > 4 && clLen[clOrder[hclen-1]] == 0 {
		hclen--
	}

	// Compare sizes in bits; extra bits of lengths and distances are the same either way.
	dyn := 3 + 5 + 5 + 4 + 3*hclen + 2*int(clFreq[16]) + 3*int(clFreq[17]) + 7*int(clFreq[18])
	fixed := 3
	for s, f := range clFreq {
		dyn += int(f) * int(clLen[s])
	}
	for s, f := range z.litFreq {
		dyn += int(f) * int(z.litLen[s])
		fixed += int(f) * int(fixedLit[s].len)
	}
	for s, f := range z.distFreq {
		dyn += int(f) * int(z.distLen[s])
		fixed += int(f) * 5
	}
	if dyn >= fixed {
		return false
	}

	var cl [19]huffCode
	buildCodes(clLen[:], cl[:])
	// Symbols past hlit and hdist are unused (frequency 0), so their codes are never read.
	buildCodes(z.litLen[:hlit], z.lit[:hlit])
	buildCodes(z.distLen[:hdist], z.dist[:hdist])
	w.write(5, 3) // BFINAL=1, BTYPE=10 (dynamic Huffman)
	w.write(uint64(hlit-257)|uint64(hdist-1)<<5|uint64(hclen-4)<<10, 14)
	for _, s := range clOrder[:hclen] {
		w.write(uint64(clLen[s]), 3)
	}
	for _, r := range rle[:nr] {
		sym, extra := r&31, uint64(r>>5)
		w.write(uint64(cl[sym].code), uint(cl[sym].len))
		switch sym {
		case 16:
			w.write(extra, 2)
		case 17:
			w.write(extra, 3)
		case 18:
			w.write(extra, 7)
		}
	}
	return true
}

// writeTokens codes src as z.toks describes it, then the end of block, with the given tables.
func (z *deflater) writeTokens(w *bitWriter, src []byte, lit *[286]huffCode, dist *[30]huffCode) {
	pos := 0
	for _, t := range z.toks {
		pos = writeLiterals(w, src[pos:pos+int(t&(1<<24-1))], lit, pos)
		l, d := int(t>>24&(1<<9-1)), uint32(t>>33)
		s := lengthSym[l]
		lc := lit[257+int(s)]
		w.write(uint64(lc.code)|uint64(l-int(lengthBase[s]))<<lc.len, uint(lc.len+lengthExtra[s]))
		ds := distSym(d)
		dc := dist[ds]
		w.write(uint64(dc.code)|uint64(d-distBase[ds])<<dc.len, uint(dc.len+distExtra[ds]))
		pos += l
	}
	writeLiterals(w, src[pos:], lit, pos)
	eob := lit[256]
	w.write(uint64(eob.code), uint(eob.len))
}

// writeLiterals codes p two bytes per write (codes are at most 15 bits) and returns
// pos advanced past p.
func writeLiterals(w *bitWriter, p []byte, lit *[286]huffCode, pos int) int {
	pos += len(p)
	for len(p) >= 2 {
		a, b := lit[p[0]], lit[p[1]]
		w.write(uint64(a.code)|uint64(b.code)<<a.len, uint(a.len+b.len))
		p = p[2:]
	}
	if len(p) == 1 {
		a := lit[p[0]]
		w.write(uint64(a.code), uint(a.len))
	}
	return pos
}

// clOrder is the order code-length code lengths are sent in.
var clOrder = [19]uint8{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15}

// buildLengths sets lens to the code lengths (at most maxLen) of a Huffman code for freq,
// 0 for unused symbols. It always gives at least two symbols a code, so the code is
// complete as decoders expect. sorted and a are scratch space; sorted is returned for reuse.
func buildLengths(freq []uint32, lens []uint8, maxLen int, sorted []uint64, a *[286]int) []uint64 {
	sorted = sorted[:0]
	for s, f := range freq {
		lens[s] = 0
		if f > 0 {
			sorted = append(sorted, uint64(f)<<16|uint64(s))
		}
	}
	for s := 0; len(sorted) < 2; s++ {
		if freq[s] == 0 {
			sorted = append(sorted, uint64(s)) // a code that is never used, frequency 0
		}
	}
	slices.Sort(sorted)
	n := len(sorted)

	// Moffat and Katajainen's in-place code lengths, over frequencies in ascending order.
	for i, e := range sorted {
		a[i] = int(e >> 16)
	}
	a[0] += a[1]
	root, leaf := 0, 2
	for next := 1; next < n-1; next++ {
		if leaf >= n || a[root] < a[leaf] {
			a[next] = a[root]
			a[root] = next
			root++
		} else {
			a[next] = a[leaf]
			leaf++
		}
		if leaf >= n || (root < next && a[root] < a[leaf]) {
			a[next] += a[root]
			a[root] = next
			root++
		} else {
			a[next] += a[leaf]
			leaf++
		}
	}
	a[n-2] = 0
	for next := n - 3; next >= 0; next-- {
		a[next] = a[a[next]] + 1
	}
	avail, used, depth := 1, 0, 0
	root, next := n-2, n-1
	for avail > 0 {
		for root >= 0 && a[root] == depth {
			used++
			root--
		}
		for avail > used {
			a[next] = depth
			next--
			avail--
		}
		avail, used = 2*used, 0
		depth++
	}

	// Count codes per length, cap them at maxLen and restore Kraft's equality (as miniz does).
	var count [33]int
	for i := range n {
		count[a[i]]++
	}
	for l := maxLen + 1; l < len(count); l++ {
		count[maxLen] += count[l]
	}
	total := 0
	for l := maxLen; l > 0; l-- {
		total += count[l] << (maxLen - l)
	}
	for ; total != 1<<maxLen; total-- {
		count[maxLen]--
		for l := maxLen - 1; l > 0; l-- {
			if count[l] != 0 {
				count[l]--
				count[l+1] += 2
				break
			}
		}
	}
	// The most frequent symbols get the shortest codes.
	j := n
	for l := 1; l <= maxLen; l++ {
		for range count[l] {
			j--
			lens[sorted[j]&0xffff] = uint8(l)
		}
	}
	return sorted
}

// buildCodes assigns the canonical codes (RFC 1951 3.2.2) for lens, bit-reversed.
func buildCodes(lens []uint8, codes []huffCode) {
	var count [16]uint16
	for _, l := range lens {
		count[l]++
	}
	count[0] = 0
	var next [16]uint16
	code := uint16(0)
	for b := 1; b < 16; b++ {
		code = (code + count[b-1]) << 1
		next[b] = code
	}
	for s, l := range lens {
		codes[s] = huffCode{}
		if l != 0 {
			codes[s] = huffCode{reverse(next[l], l), l}
			next[l]++
		}
	}
}
