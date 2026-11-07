// gzip (RFC 1952) around one deflate block (RFC 1951) whose Huffman codes are built for the body: what a response
// becomes when the client accepts gzip and the body is big enough to gain from it (in practice the feed, 5 KB to
// about 2). Matches are found greedily through a hash of the next four bytes, as the fastest levels of zlib and
// libdeflate find them; the codes are the body's own (a dynamic block), so both the JSON's repeated keys and its
// skewed letters shrink. The tables live as long as the compressor, so compressing a body allocates nothing.

/// A compressor, owned by one thread.
public struct Gzip: ~Copyable, ~Sendable {
    /// The last position each 4-byte hash was seen at, plus `base`: entries below `base` are from an earlier body.
    private let head: UnsafeMutablePointer<Int32>
    private var base: Int32 = 1
    /// The body as deflate symbols: a literal byte, or `matchFlag | (distance - 1) << 8 | (length - 3)`.
    private var symbols: UnsafeMutablePointer<UInt32>
    private var symbolCapacity: Int
    /// Symbol counts, code lengths and codes: literal/length (286), distance (30), code-length (19) alphabets.
    private let litFreq: UnsafeMutablePointer<UInt32>
    private let distFreq: UnsafeMutablePointer<UInt32>
    private let litLen: UnsafeMutablePointer<UInt8>
    private let distLen: UnsafeMutablePointer<UInt8>
    private let litCode: UnsafeMutablePointer<UInt16>
    private let distCode: UnsafeMutablePointer<UInt16>
    private let clFreq: UnsafeMutablePointer<UInt32>
    private let clLen: UnsafeMutablePointer<UInt8>
    private let clCode: UnsafeMutablePointer<UInt16>
    /// Scratch for building codes: frequencies with their symbols, sorted, then the code lengths.
    private let sortScratch: UnsafeMutablePointer<UInt64>
    private let depthScratch: UnsafeMutablePointer<UInt32>
    /// CRC-32 tables for eight bytes at a time.
    private let crcTable: UnsafeMutablePointer<UInt32>

    private static let hashBits = 13
    private static let windowSize = 32768
    private static let matchFlag: UInt32 = 1 << 31
    /// The order code-length code lengths are sent in (RFC 1951 3.2.7).
    private static let codeLengthOrder: InlineArray<19, UInt8> = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]

    public init() {
        head = .allocate(capacity: 1 << Self.hashBits)
        head.initialize(repeating: 0, count: 1 << Self.hashBits)
        symbolCapacity = 16 * 1024
        symbols = .allocate(capacity: symbolCapacity)
        litFreq = .allocate(capacity: 286)
        distFreq = .allocate(capacity: 30)
        litLen = .allocate(capacity: 286)
        distLen = .allocate(capacity: 30)
        litCode = .allocate(capacity: 286)
        distCode = .allocate(capacity: 30)
        clFreq = .allocate(capacity: 19)
        clLen = .allocate(capacity: 19)
        clCode = .allocate(capacity: 19)
        sortScratch = .allocate(capacity: 286)
        depthScratch = .allocate(capacity: 286)
        crcTable = .allocate(capacity: 8 * 256)
        for n in 0..<256 {
            var c = UInt32(n)
            for _ in 0..<8 { c = c & 1 != 0 ? 0xEDB8_8320 ^ (c >> 1) : c >> 1 }
            crcTable[n] = c
        }
        for n in 0..<256 {
            var c = crcTable[n]
            for k in 1..<8 {
                c = crcTable[Int(c & 0xFF)] ^ (c >> 8)
                crcTable[k * 256 + n] = c
            }
        }
    }

    deinit {
        head.deallocate()
        symbols.deallocate()
        litFreq.deallocate()
        distFreq.deallocate()
        litLen.deallocate()
        distLen.deallocate()
        litCode.deallocate()
        distCode.deallocate()
        clFreq.deallocate()
        clLen.deallocate()
        clCode.deallocate()
        sortScratch.deallocate()
        depthScratch.deallocate()
        crcTable.deallocate()
    }

    /// The most bytes `compress` writes for `count` bytes of input: a dynamic block is never much bigger than its
    /// input, and this covers its header and the gzip frame too.
    public static func bound(_ count: Int) -> Int { count + count / 8 + 1024 }

    /// `input` as a gzip member into `out`, which has room for `bound(input.count)` bytes; returns how many it wrote.
    public mutating func compress(_ input: UnsafeRawBufferPointer, into out: UnsafeMutableRawBufferPointer) -> Int {
        let n = input.count
        precondition(n > 0 && out.count >= Self.bound(n))
        let src = input.baseAddress!.assumingMemoryBound(to: UInt8.self)
        let count = findMatches(src, n)
        countSymbols(count)
        var w = BitWriter(out.baseAddress!.assumingMemoryBound(to: UInt8.self))
        // ID1 ID2, CM = deflate, no flags, no mtime, no extra flags, OS = Unix.
        w.uint32LE(0x0008_8B1F)
        w.uint32LE(0)
        w.byte(0x00)
        w.byte(0x03)
        writeBlock(&w, count)
        w.alignToByte()
        w.uint32LE(crc32(src, n))
        w.uint32LE(UInt32(truncatingIfNeeded: n))
        return w.offset
    }

    // ---------- matches ----------

    @inline(__always) private static func load32(_ p: UnsafePointer<UInt8>, _ i: Int) -> UInt32 {
        UnsafeRawPointer(p + i).loadUnaligned(as: UInt32.self)
    }

    /// Greedy matching: at each position, the last one with the same 4-byte hash, if its bytes match and it is in the
    /// window; else a literal. Returns the number of symbols.
    private mutating func findMatches(_ src: UnsafePointer<UInt8>, _ n: Int) -> Int {
        if symbolCapacity < n {
            symbols.deallocate()
            symbolCapacity = n
            symbols = .allocate(capacity: n)
        }
        if Int(base) > Int(Int32.max) - n - 1 {
            head.update(repeating: 0, count: 1 << Self.hashBits)
            base = 1
        }
        let b = Int(base)
        var count = 0
        var i = 0
        while i + 4 <= n {
            let v = Self.load32(src, i)
            let h = Int((v &* 0x9E37_79B1) >> UInt32(32 - Self.hashBits))
            let candidate = Int(head[h]) - b
            head[h] = Int32(b + i)
            if candidate >= 0 && i - candidate <= Self.windowSize && Self.load32(src, candidate) == v {
                var length = 4
                let limit = min(258, n - i)
                while length + 8 <= limit {
                    let x = UnsafeRawPointer(src + i + length).loadUnaligned(as: UInt64.self)
                        ^ UnsafeRawPointer(src + candidate + length).loadUnaligned(as: UInt64.self)
                    if x != 0 {
                        length += x.trailingZeroBitCount >> 3
                        break
                    }
                    length += 8
                }
                if length + 8 > limit {
                    while length < limit && src[i + length] == src[candidate + length] { length += 1 }
                }
                symbols[count] = Self.matchFlag | UInt32(i - candidate - 1) << 8 | UInt32(length - 3)
                count += 1
                i += length
            } else {
                symbols[count] = UInt32(src[i])
                count += 1
                i += 1
            }
        }
        while i < n {
            symbols[count] = UInt32(src[i])
            count += 1
            i += 1
        }
        base = Int32(b + n + 1)
        return count
    }

    // ---------- symbols ----------

    /// The literal/length code of a match length (3...258), its extra bits and their value.
    @inline(__always) private static func lengthSymbol(_ length: Int) -> (code: Int, extraBits: Int, extra: Int) {
        let l = length - 3
        if l < 8 { return (257 + l, 0, 0) }
        if l == 255 { return (285, 0, 0) }
        let k = Int.bitWidth - 1 - l.leadingZeroBitCount
        let low = (l >> (k - 2)) & 3
        return (257 + 4 * (k - 1) + low, k - 2, l - ((4 | low) << (k - 2)))
    }

    /// The distance code of a distance (1...32768), its extra bits and their value.
    @inline(__always) private static func distanceSymbol(_ distance: Int) -> (code: Int, extraBits: Int, extra: Int) {
        let x = distance - 1
        if x < 4 { return (x, 0, 0) }
        let k = Int.bitWidth - 1 - x.leadingZeroBitCount
        let low = (x >> (k - 1)) & 1
        return (2 * k + low, k - 1, x - ((2 | low) << (k - 1)))
    }

    private func countSymbols(_ count: Int) {
        litFreq.update(repeating: 0, count: 286)
        distFreq.update(repeating: 0, count: 30)
        for j in 0..<count {
            let s = symbols[j]
            if s & Self.matchFlag == 0 {
                litFreq[Int(s)] += 1
            } else {
                litFreq[Self.lengthSymbol(Int(s & 0xFF) + 3).code] += 1
                distFreq[Self.distanceSymbol(Int((s >> 8) & 0x7FFF) + 1).code] += 1
            }
        }
        litFreq[256] = 1
        // Two distance codes at least, so the tree is complete even for a body without matches.
        if distFreq[0] == 0 { distFreq[0] = 1 }
        if distFreq[1] == 0 { distFreq[1] = 1 }
    }

    // ---------- Huffman codes ----------

    /// Code lengths of at most `limit` bits for `n` symbols with frequencies `freq` (zero: unused) into `lengths`:
    /// minimum-redundancy lengths computed in place over the frequencies sorted (Moffat and Katajainen), then, if some
    /// are too long, the counts per length rebalanced as zlib does and handed out again, longest to the rarest.
    private func buildLengths(_ freq: UnsafePointer<UInt32>, _ n: Int, limit: Int, into lengths: UnsafeMutablePointer<UInt8>) {
        lengths.update(repeating: 0, count: n)
        var used = 0
        for s in 0..<n where freq[s] > 0 {
            sortScratch[used] = UInt64(freq[s]) << 16 | UInt64(s)
            used += 1
        }
        if used == 0 { return }
        if used == 1 {
            lengths[Int(sortScratch[0] & 0xFFFF)] = 1
            return
        }
        Self.heapSort(sortScratch, used)
        let a = depthScratch
        for i in 0..<used { a[i] = UInt32(sortScratch[i] >> 16) }
        // First pass, left to right: pair the two smallest of the leaves and the internal nodes made so far; an
        // internal node's slot then holds its parent's index.
        a[0] += a[1]
        var root = 0
        var leaf = 2
        for next in 1..<(used - 1) {
            if leaf >= used || a[root] < a[leaf] {
                a[next] = a[root]
                a[root] = UInt32(next)
                root += 1
            } else {
                a[next] = a[leaf]
                leaf += 1
            }
            if leaf >= used || (root < next && a[root] < a[leaf]) {
                a[next] += a[root]
                a[root] = UInt32(next)
                root += 1
            } else {
                a[next] += a[leaf]
                leaf += 1
            }
        }
        // Second pass, right to left: the internal nodes' depths.
        a[used - 2] = 0
        if used >= 3 {
            for next in stride(from: used - 3, through: 0, by: -1) { a[next] = a[Int(a[next])] + 1 }
        }
        // Third pass: the leaves' depths, deepest to the rarest.
        var available = 1
        var inner = 0
        var depth: UInt32 = 0
        var r = used - 2
        var next = used - 1
        while available > 0 {
            while r >= 0 && a[r] == depth {
                inner += 1
                r -= 1
            }
            while available > inner {
                a[next] = depth
                next -= 1
                available -= 1
            }
            available = 2 * inner
            depth += 1
            inner = 0
        }
        // Counts per length, the too long ones folded into `limit`, and the tree made whole again.
        var perLength = InlineArray<16, Int>(repeating: 0)
        var overflow = 0
        for i in 0..<used {
            let l = Int(a[i])
            if l > limit {
                perLength[limit] += 1
                overflow += 1
            } else {
                perLength[l] += 1
            }
        }
        while overflow > 0 {
            var bits = limit - 1
            while perLength[bits] == 0 { bits -= 1 }
            perLength[bits] -= 1
            perLength[bits + 1] += 2
            perLength[limit] -= 1
            overflow -= 2
        }
        // The rarest symbols come first in sorted order: they take the longest codes.
        var i = 0
        for l in stride(from: limit, through: 1, by: -1) {
            for _ in 0..<perLength[l] {
                lengths[Int(sortScratch[i] & 0xFFFF)] = UInt8(l)
                i += 1
            }
        }
    }

    /// Sorts `n` keys in place, allocating nothing.
    private static func heapSort(_ a: UnsafeMutablePointer<UInt64>, _ n: Int) {
        func siftDown(_ start: Int, _ end: Int) {
            var root = start
            while 2 * root + 1 < end {
                var child = 2 * root + 1
                if child + 1 < end && a[child] < a[child + 1] { child += 1 }
                guard a[root] < a[child] else { return }
                (a[root], a[child]) = (a[child], a[root])
                root = child
            }
        }
        for start in stride(from: n / 2 - 1, through: 0, by: -1) { siftDown(start, n) }
        for end in stride(from: n - 1, to: 0, by: -1) {
            (a[0], a[end]) = (a[end], a[0])
            siftDown(0, end)
        }
    }

    /// Canonical codes for `lengths` (RFC 1951 3.2.2), bit-reversed: deflate sends Huffman codes from the high bit.
    private static func buildCodes(_ lengths: UnsafePointer<UInt8>, _ n: Int, into codes: UnsafeMutablePointer<UInt16>) {
        var perLength = InlineArray<16, UInt16>(repeating: 0)
        for s in 0..<n { perLength[Int(lengths[s])] += 1 }
        perLength[0] = 0
        var nextCode = InlineArray<16, UInt16>(repeating: 0)
        var code: UInt16 = 0
        for bits in 1..<16 {
            code = (code + perLength[bits - 1]) << 1
            nextCode[bits] = code
        }
        for s in 0..<n {
            let l = Int(lengths[s])
            guard l > 0 else {
                codes[s] = 0
                continue
            }
            let c = nextCode[l]
            nextCode[l] += 1
            var reversed: UInt16 = 0
            for k in 0..<l { reversed |= (c >> UInt16(k) & 1) << UInt16(l - 1 - k) }
            codes[s] = reversed
        }
    }

    // ---------- the block ----------

    private func writeBlock(_ w: inout BitWriter, _ count: Int) {
        buildLengths(litFreq, 286, limit: 15, into: litLen)
        buildLengths(distFreq, 30, limit: 15, into: distLen)
        Self.buildCodes(litLen, 286, into: litCode)
        Self.buildCodes(distLen, 30, into: distCode)
        var litCount = 286
        while litCount > 257 && litLen[litCount - 1] == 0 { litCount -= 1 }
        var distCount = 30
        while distCount > 1 && distLen[distCount - 1] == 0 { distCount -= 1 }

        // The two code-length sequences, run-length coded (16: repeat the previous 3-6 times, 17: 3-10 zeros, 18:
        // 11-138 zeros), each run as its symbol and extra bits.
        var all = InlineArray<316, UInt8>(repeating: 0)
        for s in 0..<litCount { all[s] = litLen[s] }
        for s in 0..<distCount { all[litCount + s] = distLen[s] }
        let total = litCount + distCount
        var runs = InlineArray<316, UInt16>(repeating: 0)
        var runCount = 0
        clFreq.update(repeating: 0, count: 19)
        var i = 0
        while i < total {
            let l = all[i]
            var run = 1
            while i + run < total && all[i + run] == l { run += 1 }
            i += run
            if l == 0 {
                while run >= 11 {
                    let r = min(run, 138)
                    runs[runCount] = 18 | UInt16(r - 11) << 8
                    runCount += 1
                    clFreq[18] += 1
                    run -= r
                }
                if run >= 3 {
                    runs[runCount] = 17 | UInt16(run - 3) << 8
                    runCount += 1
                    clFreq[17] += 1
                    run = 0
                }
            } else {
                runs[runCount] = UInt16(l)
                runCount += 1
                clFreq[Int(l)] += 1
                run -= 1
                while run >= 3 {
                    let r = min(run, 6)
                    runs[runCount] = 16 | UInt16(r - 3) << 8
                    runCount += 1
                    clFreq[16] += 1
                    run -= r
                }
            }
            for _ in 0..<run {
                runs[runCount] = UInt16(l)
                runCount += 1
                clFreq[Int(l)] += 1
            }
        }
        buildLengths(clFreq, 19, limit: 7, into: clLen)
        Self.buildCodes(clLen, 19, into: clCode)
        var clCount = 19
        while clCount > 4 && clLen[Int(Self.codeLengthOrder[clCount - 1])] == 0 { clCount -= 1 }

        // BFINAL = 1, BTYPE = 2 (dynamic codes), then the header of the codes.
        w.put(1, 1)
        w.put(2, 2)
        w.put(UInt64(litCount - 257), 5)
        w.put(UInt64(distCount - 1), 5)
        w.put(UInt64(clCount - 4), 4)
        for k in 0..<clCount { w.put(UInt64(clLen[Int(Self.codeLengthOrder[k])]), 3) }
        for k in 0..<runCount {
            let symbol = Int(runs[k] & 0xFF)
            let extra = UInt64(runs[k] >> 8)
            w.put(UInt64(clCode[symbol]), Int(clLen[symbol]))
            switch symbol {
            case 16: w.put(extra, 2)
            case 17: w.put(extra, 3)
            case 18: w.put(extra, 7)
            default: break
            }
        }

        // The body.
        for j in 0..<count {
            let s = symbols[j]
            if s & Self.matchFlag == 0 {
                w.put(UInt64(litCode[Int(s)]), Int(litLen[Int(s)]))
                continue
            }
            let length = Self.lengthSymbol(Int(s & 0xFF) + 3)
            w.put(UInt64(litCode[length.code]), Int(litLen[length.code]))
            if length.extraBits > 0 { w.put(UInt64(length.extra), length.extraBits) }
            let distance = Self.distanceSymbol(Int((s >> 8) & 0x7FFF) + 1)
            w.put(UInt64(distCode[distance.code]), Int(distLen[distance.code]))
            if distance.extraBits > 0 { w.put(UInt64(distance.extra), distance.extraBits) }
        }
        w.put(UInt64(litCode[256]), Int(litLen[256]))
    }

    // ---------- CRC-32 ----------

    /// CRC-32 (the gzip polynomial), eight bytes per step.
    private func crc32(_ p: UnsafePointer<UInt8>, _ n: Int) -> UInt32 {
        var c: UInt32 = 0xFFFF_FFFF
        var i = 0
        let t = crcTable
        while i + 8 <= n {
            let lo = UnsafeRawPointer(p + i).loadUnaligned(as: UInt32.self).littleEndian ^ c
            let hi = UnsafeRawPointer(p + i + 4).loadUnaligned(as: UInt32.self).littleEndian
            c = t[7 * 256 + Int(lo & 0xFF)] ^ t[6 * 256 + Int((lo >> 8) & 0xFF)]
                ^ t[5 * 256 + Int((lo >> 16) & 0xFF)] ^ t[4 * 256 + Int(lo >> 24)]
                ^ t[3 * 256 + Int(hi & 0xFF)] ^ t[2 * 256 + Int((hi >> 8) & 0xFF)]
                ^ t[1 * 256 + Int((hi >> 16) & 0xFF)] ^ t[Int(hi >> 24)]
            i += 8
        }
        while i < n {
            c = t[Int((c ^ UInt32(p[i])) & 0xFF)] ^ (c >> 8)
            i += 1
        }
        return ~c
    }
}

/// Bits into bytes, least significant first, as deflate packs them.
private struct BitWriter {
    let out: UnsafeMutablePointer<UInt8>
    private(set) var offset = 0
    private var bits: UInt64 = 0
    private var count = 0

    init(_ out: UnsafeMutablePointer<UInt8>) {
        self.out = out
    }

    @inline(__always) mutating func put(_ value: UInt64, _ n: Int) {
        bits |= value << UInt64(count)
        count += n
        if count >= 32 {
            UnsafeMutableRawPointer(out + offset).storeBytes(of: UInt32(truncatingIfNeeded: bits).littleEndian, as: UInt32.self)
            offset += 4
            bits >>= 32
            count -= 32
        }
    }

    mutating func alignToByte() {
        while count > 0 {
            out[offset] = UInt8(truncatingIfNeeded: bits)
            offset += 1
            bits >>= 8
            count = max(count - 8, 0)
        }
        bits = 0
    }

    @inline(__always) mutating func byte(_ b: UInt8) {
        out[offset] = b
        offset += 1
    }

    mutating func uint32LE(_ v: UInt32) {
        for k in 0..<4 { out[offset + k] = UInt8(truncatingIfNeeded: v >> UInt32(8 * k)) }
        offset += 4
    }
}
