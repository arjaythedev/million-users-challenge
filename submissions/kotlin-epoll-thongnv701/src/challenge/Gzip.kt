package challenge

import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout
import java.nio.ByteOrder
import java.util.zip.CRC32

private val HEADER = byteArrayOf(0x1f, 0x8b.toByte(), 8, 0, 0, 0, 0, 0, 0, 0xff.toByte())
private val LE_INT: ValueLayout.OfInt = ValueLayout.JAVA_INT_UNALIGNED.withOrder(ByteOrder.LITTLE_ENDIAN)
private val LE_LONG: ValueLayout.OfLong = ValueLayout.JAVA_LONG_UNALIGNED.withOrder(ByteOrder.LITTLE_ENDIAN)

private const val WINDOW = 32768
private const val MIN_MATCH = 4
private const val MAX_MATCH = 258
private const val HASH_BITS = 14
private const val LITLEN = 286
private const val DIST = 30
private const val CODELEN = 19
private const val EOB = 256
private const val MAX_BITS = 15
private const val MAX_CL_BITS = 7
private const val ENOUGH_BITS = 8L * 1200

private val LEN_BASE = intArrayOf(3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
    115, 131, 163, 195, 227, 258)
private val LEN_EXTRA = intArrayOf(0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0)
private val DIST_BASE = intArrayOf(1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
    1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577)
private val DIST_EXTRA = intArrayOf(0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
    12, 13, 13)
private val CODELEN_ORDER = intArrayOf(16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15)
private val CODELEN_EXTRA = intArrayOf(2, 3, 7)

private val LEN_CODE = ByteArray(MAX_MATCH + 1)
private val DIST_CODE = ByteArray(512)
private val FIXED_LIT_LEN = ByteArray(288)
private val FIXED_DIST_LEN = ByteArray(32)
private val FIXED_LIT_CODE = IntArray(288)
private val FIXED_DIST_CODE = IntArray(32)

private fun hash(x: Int): Int = (x * 0x1E35A7BD) ushr (32 - HASH_BITS)

private fun distSymbol(dist: Int): Int {
    val d = dist - 1
    return DIST_CODE[if (d < 256) d else 256 + (d ushr 7)].toInt()
}

/** Canonical Huffman codes from code lengths, bit-reversed for DEFLATE's LSB-first order. */
private fun codes(len: ByteArray, n: Int, code: IntArray, next: IntArray) {
    next.fill(0)
    for (s in 0 until n) next[len[s].toInt()]++
    next[0] = 0
    var c = 0
    for (b in 1..MAX_BITS) {
        val count = next[b]
        next[b] = c
        c = (c + count) shl 1
    }
    for (s in 0 until n) {
        val l = len[s].toInt()
        if (l != 0) code[s] = Integer.reverse(next[l]++) ushr (32 - l)
    }
}

private val TABLES_READY: Boolean = run {
    for (c in LEN_BASE.indices) {
        val to = if (c + 1 < LEN_BASE.size) LEN_BASE[c + 1] else MAX_MATCH + 1
        for (l in LEN_BASE[c] until to) LEN_CODE[l] = c.toByte()
    }
    for (c in 0 until DIST) {
        var d = DIST_BASE[c] - 1
        val to = d + (1 shl DIST_EXTRA[c])
        while (d < to) {
            DIST_CODE[if (d < 256) d else 256 + (d ushr 7)] = c.toByte()
            d++
        }
    }
    for (s in 0 until 288) FIXED_LIT_LEN[s] = (if (s < 144) 8 else if (s < 256) 9 else if (s < 280) 7 else 8).toByte()
    FIXED_DIST_LEN.fill(5)
    codes(FIXED_LIT_LEN, 288, FIXED_LIT_CODE, IntArray(MAX_BITS + 2))
    codes(FIXED_DIST_LEN, 32, FIXED_DIST_CODE, IntArray(MAX_BITS + 2))
    true
}

/**
 * A DEFLATE encoder for one small body at a time: greedy LZ77 over a hash table that is never cleared (a moving
 * base turns old entries stale), then fixed Huffman codes when they fit in one packet, else the smallest of
 * dynamic, fixed and stored.
 */
class Gzip(capacity: Int) {
    private val crc = CRC32()
    private val inb = ByteArray(capacity + 8)
    private val inSeg: MemorySegment = MemorySegment.ofArray(inb)
    @JvmField val buf = ByteArray(capacity + capacity / 8 + 1024)
    private val bufSeg: MemorySegment = MemorySegment.ofArray(buf)
    private var size = 0

    private val head = IntArray(1 shl HASH_BITS)
    private var base = 1
    private val tokens = IntArray(capacity)

    private val litFreq = IntArray(LITLEN)
    private val distFreq = IntArray(DIST)
    private val clFreq = IntArray(CODELEN)
    private val litLen = ByteArray(LITLEN)
    private val distLen = ByteArray(DIST)
    private val clLen = ByteArray(CODELEN)
    private val litCode = IntArray(LITLEN)
    private val distCode = IntArray(DIST)
    private val clCode = IntArray(CODELEN)
    private val lens = ByteArray(LITLEN + DIST)
    private val rle = IntArray(LITLEN + DIST)
    private val leaves = IntArray(LITLEN)
    private val weight = IntArray(2 * LITLEN)
    private val parent = IntArray(2 * LITLEN)
    private val depthCount = IntArray(2 * LITLEN)
    private val nextCode = IntArray(MAX_BITS + 2)

    private var bits = 0L
    private var nbits = 0
    private var pos = 0

    init {
        check(TABLES_READY)
    }

    fun begin() {
        size = 0
    }

    fun add(a: ByteArray, off: Int, n: Int) {
        if (size + n > tokens.size) throw IllegalStateException("gzip buffer full")
        System.arraycopy(a, off, inb, size, n)
        size += n
    }

    /** Finishes the member; the gzip bytes are buf[0, result). */
    fun end(): Int {
        crc.reset()
        crc.update(inb, 0, size)
        System.arraycopy(HEADER, 0, buf, 0, HEADER.size)
        pos = HEADER.size
        bits = 0
        nbits = 0
        block(tokenize())
        align()
        bufSeg.set(LE_INT, pos.toLong(), crc.value.toInt())
        bufSeg.set(LE_INT, pos.toLong() + 4, size)
        return pos + 8
    }

    private fun tokenize(): Int {
        val b = inb
        val seg = inSeg
        val n = size
        var nt = 0
        var i = 0
        val last = n - MIN_MATCH
        val h = head
        val t = tokens
        val lf = litFreq
        val df = distFreq
        lf.fill(0)
        df.fill(0)
        if (base > Int.MAX_VALUE - n) {
            h.fill(0)
            base = 1
        }
        val bs = base
        while (i <= last) {
            val x = seg.get(LE_INT, i.toLong())
            val k = hash(x)
            val cand = h[k] - bs
            h[k] = i + bs
            if (cand < 0 || i - cand > WINDOW || seg.get(LE_INT, cand.toLong()) != x) {
                val c = b[i++].toInt() and 0xff
                t[nt++] = c
                lf[c]++
                continue
            }
            val max = minOf(MAX_MATCH, n - i)
            var len = MIN_MATCH
            while (len < max) {
                val d = seg.get(LE_LONG, (cand + len).toLong()) xor seg.get(LE_LONG, (i + len).toLong())
                if (d != 0L) {
                    len += d.countTrailingZeroBits() ushr 3
                    break
                }
                len += 8
            }
            if (len > max) len = max
            val dist = i - cand
            t[nt++] = (len shl 16) or dist
            lf[EOB + 1 + LEN_CODE[len]]++
            df[distSymbol(dist)]++
            val end = i + len
            var p = i + 1
            val to = minOf(end, last + 1)
            while (p < to) {
                h[hash(seg.get(LE_INT, p.toLong()))] = p + bs
                p++
            }
            i = end
        }
        while (i < n) {
            val c = b[i++].toInt() and 0xff
            t[nt++] = c
            lf[c]++
        }
        lf[EOB] = 1
        base = bs + n
        return nt
    }

    private fun block(nt: Int) {
        if (fixedBits() <= ENOUGH_BITS) {
            put(1 or (1 shl 1), 3)
            data(nt, FIXED_LIT_CODE, FIXED_LIT_LEN, FIXED_DIST_CODE, FIXED_DIST_LEN)
            return
        }
        lengths(litFreq, LITLEN, MAX_BITS, litLen)
        lengths(distFreq, DIST, MAX_BITS, distLen)
        var hlit = LITLEN
        var hdist = DIST
        while (hlit > 257 && litLen[hlit - 1].toInt() == 0) hlit--
        while (hdist > 1 && distLen[hdist - 1].toInt() == 0) hdist--
        System.arraycopy(litLen, 0, lens, 0, hlit)
        System.arraycopy(distLen, 0, lens, hlit, hdist)
        val nr = runs(hlit + hdist)
        lengths(clFreq, CODELEN, MAX_CL_BITS, clLen)
        var hclen = CODELEN
        while (hclen > 4 && clLen[CODELEN_ORDER[hclen - 1]].toInt() == 0) hclen--

        var extra = 0L
        var dynamic = 17 + 3L * hclen
        var fixed = 3L
        for (c in LEN_BASE.indices) extra += litFreq[EOB + 1 + c].toLong() * LEN_EXTRA[c]
        for (c in 0 until DIST) extra += distFreq[c].toLong() * DIST_EXTRA[c]
        for (s in 0 until LITLEN) {
            dynamic += litFreq[s].toLong() * litLen[s]
            fixed += litFreq[s].toLong() * FIXED_LIT_LEN[s]
        }
        for (c in 0 until DIST) {
            dynamic += distFreq[c].toLong() * distLen[c]
            fixed += distFreq[c].toLong() * FIXED_DIST_LEN[c]
        }
        for (s in 0 until CODELEN) {
            dynamic += clFreq[s].toLong() * (clLen[s] + (if (s >= 16) CODELEN_EXTRA[s - 16] else 0))
        }
        val stored = 8L * (size + 5L * maxOf(1, (size + 65534) / 65535))
        dynamic += extra
        fixed += extra

        if (stored < dynamic && stored < fixed) {
            stored()
        } else if (fixed <= dynamic) {
            put(1 or (1 shl 1), 3)
            data(nt, FIXED_LIT_CODE, FIXED_LIT_LEN, FIXED_DIST_CODE, FIXED_DIST_LEN)
        } else {
            codes(litLen, LITLEN, litCode, nextCode)
            codes(distLen, DIST, distCode, nextCode)
            codes(clLen, CODELEN, clCode, nextCode)
            put(1 or (2 shl 1), 3)
            put(hlit - 257, 5)
            put(hdist - 1, 5)
            put(hclen - 4, 4)
            for (k in 0 until hclen) put(clLen[CODELEN_ORDER[k]].toInt(), 3)
            for (k in 0 until nr) {
                val s = rle[k] and 31
                put(clCode[s], clLen[s].toInt())
                if (s >= 16) put(rle[k] ushr 5, CODELEN_EXTRA[s - 16])
            }
            data(nt, litCode, litLen, distCode, distLen)
        }
    }

    private fun fixedBits(): Long {
        var n = 3L
        for (s in 0 until LITLEN) {
            n += litFreq[s].toLong() * (FIXED_LIT_LEN[s] + (if (s > EOB) LEN_EXTRA[s - EOB - 1] else 0))
        }
        for (c in 0 until DIST) n += distFreq[c].toLong() * (FIXED_DIST_LEN[c] + DIST_EXTRA[c])
        return n
    }

    private fun runs(total: Int): Int {
        clFreq.fill(0)
        var nr = 0
        var k = 0
        while (k < total) {
            val v = lens[k].toInt()
            var run = 1
            while (k + run < total && lens[k + run].toInt() == v) run++
            k += run
            if (v == 0) {
                while (run >= 11) {
                    val r = minOf(run, 138)
                    nr = run(nr, 18, r - 11)
                    run -= r
                }
                if (run >= 3) {
                    nr = run(nr, 17, run - 3)
                    run = 0
                }
            } else {
                nr = run(nr, v, 0)
                run--
                while (run >= 3) {
                    val r = minOf(run, 6)
                    nr = run(nr, 16, r - 3)
                    run -= r
                }
            }
            while (run-- > 0) nr = run(nr, v, 0)
        }
        return nr
    }

    private fun run(nr: Int, symbol: Int, extra: Int): Int {
        rle[nr] = symbol or (extra shl 5)
        clFreq[symbol]++
        return nr + 1
    }

    private fun data(nt: Int, lc: IntArray, ll: ByteArray, dc: IntArray, dl: ByteArray) {
        val t = tokens
        val o = bufSeg
        var bb = bits
        var nb = nbits
        var p = pos
        for (k in 0 until nt) {
            val v = t[k]
            if (v < 256) {
                bb = bb or (lc[v].toLong() shl nb)
                nb += ll[v]
            } else {
                val len = v ushr 16
                val dist = v and 0xffff
                val c = LEN_CODE[len].toInt()
                val s = EOB + 1 + c
                val d = distSymbol(dist)
                bb = bb or ((lc[s] or ((len - LEN_BASE[c]) shl ll[s].toInt())).toLong() shl nb)
                nb += ll[s] + LEN_EXTRA[c]
                if (nb >= 32) {
                    o.set(LE_INT, p.toLong(), bb.toInt())
                    p += 4
                    bb = bb ushr 32
                    nb -= 32
                }
                bb = bb or ((dc[d] or ((dist - DIST_BASE[d]) shl dl[d].toInt())).toLong() shl nb)
                nb += dl[d] + DIST_EXTRA[d]
            }
            if (nb >= 32) {
                o.set(LE_INT, p.toLong(), bb.toInt())
                p += 4
                bb = bb ushr 32
                nb -= 32
            }
        }
        bits = bb
        nbits = nb
        pos = p
        put(lc[EOB], ll[EOB].toInt())
    }

    private fun stored() {
        var from = 0
        do {
            val n = minOf(size - from, 65535)
            put(if (from + n == size) 1 else 0, 3)
            align()
            buf[pos] = n.toByte()
            buf[pos + 1] = (n ushr 8).toByte()
            buf[pos + 2] = n.inv().toByte()
            buf[pos + 3] = (n.inv() ushr 8).toByte()
            System.arraycopy(inb, from, buf, pos + 4, n)
            pos += 4 + n
            from += n
        } while (from < size)
    }

    private fun put(value: Int, n: Int) {
        bits = bits or (value.toLong() shl nbits)
        nbits += n
        if (nbits >= 32) {
            bufSeg.set(LE_INT, pos.toLong(), bits.toInt())
            pos += 4
            bits = bits ushr 32
            nbits -= 32
        }
    }

    private fun align() {
        while (nbits > 0) {
            buf[pos++] = bits.toByte()
            bits = bits ushr 8
            nbits -= 8
        }
        bits = 0
        nbits = 0
    }

    /** Huffman code lengths for freq[0, n), limited to `limit` bits (two-queue build, then depth limiting). */
    private fun lengths(freq: IntArray, n: Int, limit: Int, len: ByteArray) {
        var m = 0
        for (s in 0 until n) {
            len[s] = 0
            if (freq[s] > 0) leaves[m++] = (freq[s] shl 9) or s
        }
        if (m < 2) {
            val s = if (m == 1) leaves[0] and 511 else 0
            len[s] = 1
            len[if (s == 0) 1 else 0] = 1
            return
        }
        leaves.sort(0, m)
        for (k in 0 until m) weight[k] = leaves[k] ushr 9
        var leaf = 0
        var node = m
        var next = m
        for (k in 0 until m - 1) {
            val a = if (leaf < m && (node >= next || weight[leaf] <= weight[node])) leaf++ else node++
            val b = if (leaf < m && (node >= next || weight[leaf] <= weight[node])) leaf++ else node++
            weight[next] = weight[a] + weight[b]
            parent[a] = next
            parent[b] = next
            next++
        }
        val root = next - 1
        var deepest = 0
        weight[root] = 0
        for (k in root - 1 downTo 0) {
            weight[k] = weight[parent[k]] + 1
            if (k < m) {
                depthCount[weight[k]]++
                deepest = maxOf(deepest, weight[k])
            }
        }
        for (d in deepest downTo limit + 1) {
            while (depthCount[d] > 0) {
                var j = d - 2
                while (depthCount[j] == 0) j--
                depthCount[d] -= 2
                depthCount[d - 1]++
                depthCount[j + 1] += 2
                depthCount[j]--
            }
        }
        var k = m - 1
        for (d in 1..minOf(deepest, limit)) {
            for (c in depthCount[d] downTo 1) len[leaves[k--] and 511] = d.toByte()
            depthCount[d] = 0
        }
    }
}
