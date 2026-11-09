// gzip (RFC 1952) around one deflate block (RFC 1951): what a response becomes when the client accepts gzip and the
// body is big enough to gain from it (in practice the feed, about 4 to 5 KB). The block's Huffman codes are ISA-L's
// default table, the one its fastest level uses: a dynamic block header that ships with the library, whose code
// lengths are read out of it once. So a body is compressed in one pass, each literal or match written out as it is
// found, with no codes to build for it. Matches are found greedily through a hash of the next four bytes, as the
// fastest levels of zlib, libdeflate and ISA-L find them. The CRC-32 runs on AVX-512's carry-less multiply where the
// CPU has it (CRCExtensions512), on the 128-bit one where it has that (CRCExtensions), and eight bytes per step from
// tables otherwise. The tables live as long as the compressor,
// so compressing a body allocates nothing.
//
// The block header below is ISA-L 2.31.1's (igzip/hufftables_c.c, hufftables_default for a 32 KB window), under
// this notice:
//
//   Copyright(c) 2011-2016 Intel Corporation All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
//   following conditions are met:
//     * Redistributions of source code must retain the above copyright notice, this list of conditions and the
//       following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
//       following disclaimer in the documentation and/or other materials provided with the distribution.
//     * Neither the name of Intel Corporation nor the names of its contributors may be used to endorse or promote
//       products derived from this software without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
//   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
//   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
//   USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#if arch(x86_64)
import CRCExtensions
import CRCExtensions512
import Syscalls
#endif

/// A compressor, owned by one thread.
public struct Gzip: ~Copyable, ~Sendable {
    /// The last position each 4-byte hash was seen at, its low 16 bits (ISA-L's level-0 table). An entry may be from
    /// an earlier body or point at the wrong bytes: a candidate is only taken inside this body, after its bytes match.
    private let head: UnsafeMutablePointer<UInt16>
    /// The table's codes (bit-reversed, as deflate sends them) and their lengths in bits 16...23: literals and end of
    /// block (0...256).
    private let literalCode: UnsafeMutablePointer<UInt32>
    /// A match length's whole code (3...258): its length symbol's code with the extra bits after it, and their count.
    private let lengthCode: UnsafeMutablePointer<UInt32>
    private let lengthBits: UnsafeMutablePointer<UInt8>
    /// The distance codes (0...29).
    private let distanceCode: UnsafeMutablePointer<UInt16>
    private let distanceBits: UnsafeMutablePointer<UInt8>
    /// CRC-32 tables for eight bytes at a time.
    private let crcTable: UnsafeMutablePointer<UInt32>
    /// `header`, kept here so a body doesn't go through the static array.
    private let headerBytes: UnsafeMutablePointer<UInt8>
    /// What the CRC runs on: the widest carry-less multiply the CPU has that agreed with the tables at start.
    private var crcPath = CRCPath.tables

    private enum CRCPath {
        case tables, carryless, carryless512
    }

    /// The instructions the CRC runs on, for the start-up line.
    public var crcInstructions: String {
        switch crcPath {
        case .tables: "tables"
        case .carryless: "PCLMULQDQ"
        case .carryless512: "VPCLMULQDQ"
        }
    }

    private static let hashBits = 13
    private static let windowSize = 32768

    /// ISA-L's block header: BFINAL = 1, BTYPE = 2 (dynamic codes), the code lengths of all 286 literal/length and 30
    /// distance symbols; 110 bytes and 6 bits.
    private static let header: [UInt8] = [
        0xed, 0xfd, 0x09, 0x80, 0x1c, 0x45, 0xf9, 0xbf, 0x81, 0xf7, 0x66, 0x37, 0xd7, 0x24, 0x9b, 0x04,
        0x40, 0x45, 0x45, 0x52, 0x04, 0x20, 0x09, 0xcc, 0x2e, 0xbb, 0x9b, 0x3b, 0x81, 0x24, 0xbb, 0xb9,
        0x21, 0x17, 0x49, 0xb8, 0x04, 0x85, 0xde, 0x99, 0xde, 0x9d, 0x26, 0x33, 0xd3, 0x43, 0xf7, 0x4c,
        0x36, 0x8b, 0x08, 0xf1, 0x56, 0x51, 0xc1, 0xfb, 0x56, 0x54, 0xc4, 0x5b, 0x51, 0xf1, 0x16, 0x0d,
        0x89, 0x8a, 0x37, 0x78, 0xdf, 0x1a, 0x45, 0x05, 0xef, 0x20, 0xaa, 0xa0, 0x90, 0xfd, 0xff, 0x9f,
        0xf7, 0xad, 0x9e, 0xa9, 0xdd, 0xdd, 0x70, 0xfa, 0xfd, 0xfd, 0xbe, 0xbf, 0xff, 0xdf, 0xcd, 0x66,
        0x67, 0xba, 0xbb, 0xaa, 0xea, 0x7d, 0xdf, 0x7a, 0xeb, 0xad, 0x4f, 0xbd, 0xf5, 0x56, 0x35,
    ]
    private static let headerBits = 110 * 8 + 6

    public init() {
        head = .allocate(capacity: 1 << Self.hashBits)
        head.initialize(repeating: 0, count: 1 << Self.hashBits)
        literalCode = .allocate(capacity: 257)
        lengthCode = .allocate(capacity: 259)
        lengthBits = .allocate(capacity: 259)
        distanceCode = .allocate(capacity: 30)
        distanceBits = .allocate(capacity: 30)
        crcTable = .allocate(capacity: 8 * 256)
        headerBytes = .allocate(capacity: Self.header.count)
        headerBytes.initialize(from: Self.header, count: Self.header.count)

        let (literalLengths, distanceLengths) = Self.codeLengths()
        let literalCodes = Self.canonicalCodes(literalLengths)
        let distanceCodes = Self.canonicalCodes(distanceLengths)
        for s in 0...256 {
            literalCode[s] = UInt32(literalCodes[s]) | UInt32(literalLengths[s]) << 16
        }
        lengthCode.initialize(repeating: 0, count: 259)
        lengthBits.initialize(repeating: 0, count: 259)
        for length in 3...258 {
            let (symbol, extraBits, extra) = Self.lengthSymbol(length)
            let bits = Int(literalLengths[symbol])
            lengthCode[length] = UInt32(literalCodes[symbol]) | UInt32(extra) << UInt32(bits)
            lengthBits[length] = UInt8(bits + extraBits)
        }
        for s in 0..<30 {
            distanceCode[s] = distanceCodes[s]
            distanceBits[s] = distanceLengths[s]
        }

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
        #if arch(x86_64)
        // Each path the CPU has, on made-up bytes at lengths that take every part of its fold, against the tables.
        let sample = (0..<4103).map { UInt8(truncatingIfNeeded: $0 &* 131 &+ $0 >> 5) }
        let (carryless, wide) = Self.carrylessMultiply
        for (path, usable) in [(CRCPath.carryless, carryless), (CRCPath.carryless512, wide)] where usable {
            var agree = true
            sample.withUnsafeBufferPointer { p in
                for n in [128, 144, 256, 272, 1000, 4103] {
                    crcPath = .tables
                    let tables = crc32(p.baseAddress!, n)
                    crcPath = path
                    agree = agree && crc32(p.baseAddress!, n) == tables
                }
            }
            crcPath = agree ? path : .tables
            if !agree { break }
        }
        #endif
    }

    #if arch(x86_64)
    /// Whether the CPU has, usable, PCLMULQDQ and SSE4.1 (CPUID leaf 1, ECX bits 1 and 19), and on top of them AVX-512F
    /// and VPCLMULQDQ (leaf 7, EBX bit 16 and ECX bit 10): read through glibc (<sys/platform/x86.h>, where
    /// `CPUID_INDEX_1` is 0 and `CPUID_INDEX_7` 1, and the usable registers are words 4...7).
    private static var carrylessMultiply: (Bool, Bool) {
        guard let leaf1 = sys_x86_get_cpuid_feature_leaf(0), let leaf7 = sys_x86_get_cpuid_feature_leaf(1) else {
            return (false, false)
        }
        let narrow = leaf1[4 + 2] >> 1 & 1 != 0 && leaf1[4 + 2] >> 19 & 1 != 0
        return (narrow, narrow && leaf7[4 + 1] >> 16 & 1 != 0 && leaf7[4 + 2] >> 10 & 1 != 0)
    }
    #endif

    deinit {
        head.deallocate()
        literalCode.deallocate()
        lengthCode.deallocate()
        lengthBits.deallocate()
        distanceCode.deallocate()
        distanceBits.deallocate()
        crcTable.deallocate()
        headerBytes.deallocate()
    }

    /// The most bytes `compress` writes for `count` bytes of input: no code is longer than 15 bits, so at most two
    /// bytes per input byte, plus the headers and the trailer.
    public static func bound(_ count: Int) -> Int { 2 * count + 256 }

    /// `input` as a gzip member into `out`, which has room for `bound(input.count)` bytes; returns how many it wrote.
    public mutating func compress(_ input: UnsafeRawBufferPointer, into out: UnsafeMutableRawBufferPointer) -> Int {
        let n = input.count
        precondition(n > 0 && out.count >= Self.bound(n))
        let src = input.baseAddress!.assumingMemoryBound(to: UInt8.self)
        let dst = out.baseAddress!.assumingMemoryBound(to: UInt8.self)
        // ID1 ID2, CM = deflate, no flags, no mtime, no extra flags, OS = Unix; then the block header.
        var w = BitWriter(dst)
        w.uint32LE(0x0008_8B1F)
        w.uint32LE(0)
        w.byte(0x00)
        w.byte(0x03)
        w.copy(headerBytes, bits: Self.headerBits)
        writeBody(src, n, &w)
        writeLiteral(256, &w)
        w.alignToByte()
        w.uint32LE(crc32(src, n))
        w.uint32LE(UInt32(truncatingIfNeeded: n))
        return w.offset
    }

    // ---------- the body ----------

    @inline(__always) private static func load32(_ p: UnsafePointer<UInt8>, _ i: Int) -> UInt32 {
        UnsafeRawPointer(p + i).loadUnaligned(as: UInt32.self)
    }

    /// Greedy matching, as ISA-L's level-0 loop (isal_deflate_body) does it: two positions per step, both hashed and
    /// looked up, 8 bytes at each compared with their candidate's at once; a match at the first is written alone, a
    /// match at the second after the first's literal in the same write, and otherwise both literals in one write. A
    /// candidate is only taken inside this body. After a match the two positions after the step's are hashed too.
    /// With ISA-L's table a literal and a match take at most 11 + 20 + 19 bits, so one write never passes 57.
    private mutating func writeBody(_ src: UnsafePointer<UInt8>, _ n: Int, _ w: inout BitWriter) {
        var i = 0
        // A step reads 8 bytes at i + 1.
        while i + 9 <= n {
            let here = Self.load64(src, i)
            let next = Self.load64(src, i + 1)
            let h0 = Self.hash(UInt32(truncatingIfNeeded: here))
            let h1 = Self.hash(UInt32(truncatingIfNeeded: next))
            let d0 = (i &- Int(head[h0])) & 0xFFFF
            head[h0] = UInt16(truncatingIfNeeded: i)
            let d1 = (i + 1 &- Int(head[h1])) & 0xFFFF
            head[h1] = UInt16(truncatingIfNeeded: i + 1)
            if Self.inBody(d0, i) {
                let x = here ^ Self.load64(src, i - d0)
                if x & 0xFFFF_FFFF == 0 {
                    let length = x == 0 ? Self.longMatch(src, i, i - d0, n) : x.trailingZeroBitCount >> 3
                    let (code, bits) = matchCode(length, d0)
                    w.put(code, bits)
                    hashAfterMatch(src, i, n)
                    i += length
                    continue
                }
            }
            if Self.inBody(d1, i + 1) {
                let x = next ^ Self.load64(src, i + 1 - d1)
                if x & 0xFFFF_FFFF == 0 {
                    let length = x == 0 ? Self.longMatch(src, i + 1, i + 1 - d1, n) : x.trailingZeroBitCount >> 3
                    let literal = literalCode[Int(truncatingIfNeeded: here & 0xFF)]
                    let (code, bits) = matchCode(length, d1)
                    w.put(UInt64(literal & 0xFFFF) | code &<< UInt64(literal >> 16), Int(literal >> 16) + bits)
                    hashAfterMatch(src, i, n)
                    i += 1 + length
                    continue
                }
            }
            let first = literalCode[Int(truncatingIfNeeded: here & 0xFF)]
            let second = literalCode[Int(truncatingIfNeeded: here >> 8 & 0xFF)]
            w.put(UInt64(first & 0xFFFF) | UInt64(second & 0xFFFF) &<< UInt64(first >> 16), Int(first >> 16 + second >> 16))
            i += 2
        }
        // The last few bytes, one position at a time.
        while i + 4 <= n {
            let v = Self.load32(src, i)
            let h = Self.hash(v)
            let distance = (i &- Int(head[h])) & 0xFFFF
            head[h] = UInt16(truncatingIfNeeded: i)
            if Self.inBody(distance, i) && Self.load32(src, i - distance) == v {
                var length = 4
                while i + length < n && src[i + length] == src[i - distance + length] && length < 258 { length += 1 }
                let (code, bits) = matchCode(length, distance)
                w.put(code, bits)
                i += length
            } else {
                writeLiteral(Int(src[i]), &w)
                i += 1
            }
        }
        while i < n {
            writeLiteral(Int(src[i]), &w)
            i += 1
        }
    }

    /// Whether a candidate `distance` back from `position` is in the window and in this body.
    @inline(__always) private static func inBody(_ distance: Int, _ position: Int) -> Bool {
        distance != 0 && distance <= windowSize && distance <= position
    }

    @inline(__always) private static func load64(_ p: UnsafePointer<UInt8>, _ i: Int) -> UInt64 {
        UnsafeRawPointer(p + i).loadUnaligned(as: UInt64.self)
    }

    /// The length of a match whose first 8 bytes agree (at most 258, and what is left of the body).
    @inline(__always) private static func longMatch(_ src: UnsafePointer<UInt8>, _ at: Int, _ candidate: Int, _ n: Int) -> Int {
        let limit = min(258, n - at)
        var length = 8
        while length + 8 <= limit {
            let x = load64(src, at + length) ^ load64(src, candidate + length)
            if x != 0 { return length + x.trailingZeroBitCount >> 3 }
            length += 8
        }
        while length < limit && src[at + length] == src[candidate + length] { length += 1 }
        return length
    }

    /// A match's length and distance codes, joined, and their bit count.
    @inline(__always) private func matchCode(_ length: Int, _ distance: Int) -> (UInt64, Int) {
        let d = Self.distanceSymbol(distance)
        let lengthBitCount = UInt64(lengthBits[length])
        let distanceBitCount = UInt64(distanceBits[d.code])
        let code = UInt64(lengthCode[length])
            | (UInt64(distanceCode[d.code]) | UInt64(d.extra) &<< distanceBitCount) &<< lengthBitCount
        return (code, Int(lengthBitCount + distanceBitCount) + d.extraBits)
    }

    /// The two positions after a step that found a match at `step` or `step + 1`, hashed (ISA-L's limited update).
    @inline(__always) private func hashAfterMatch(_ src: UnsafePointer<UInt8>, _ step: Int, _ n: Int) {
        for k in (step + 2)...(step + 3) where k + 4 <= n {
            head[Self.hash(Self.load32(src, k))] = UInt16(truncatingIfNeeded: k)
        }
    }

    @inline(__always) private static func hash(_ v: UInt32) -> Int {
        Int((v &* 0x9E37_79B1) >> UInt32(32 - hashBits))
    }

    @inline(__always) private func writeLiteral(_ symbol: Int, _ w: inout BitWriter) {
        let entry = literalCode[symbol]
        w.put(UInt64(entry & 0xFFFF), Int(entry >> 16))
    }

    /// The literal/length code of a match length (3...258), its extra bits and their value.
    private static func lengthSymbol(_ length: Int) -> (code: Int, extraBits: Int, extra: Int) {
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

    // ---------- the table ----------

    /// The code lengths the header carries: 286 literal/length and 30 distance ones (RFC 1951 3.2.7).
    private static func codeLengths() -> ([UInt8], [UInt8]) {
        var r = BitReader(header)
        // Read first, then checked: -Ounchecked drops a precondition's condition, reads and all.
        let final = r.take(1)
        let type = r.take(2)
        precondition(final == 1 && type == 2)
        let literalCount = r.take(5) + 257
        let distanceCount = r.take(5) + 1
        let codeLengthCount = r.take(4) + 4
        let order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
        var codeLengthLengths = [UInt8](repeating: 0, count: 19)
        for k in 0..<codeLengthCount { codeLengthLengths[order[k]] = UInt8(r.take(3)) }
        let codeLengthCodes = canonicalCodes(codeLengthLengths)
        var lengths: [UInt8] = []
        while lengths.count < literalCount + distanceCount {
            // The symbol whose (bit-reversed) code the next bits are: codes are prefix-free, so try them shortest first.
            var symbol = -1
            var code = 0
            for bits in 1...7 {
                code |= r.take(1) << (bits - 1)
                if let s = (0..<19).first(where: { Int(codeLengthLengths[$0]) == bits && Int(codeLengthCodes[$0]) == code }) {
                    symbol = s
                    break
                }
            }
            switch symbol {
            case 0..<16: lengths.append(UInt8(symbol))
            case 16: lengths += repeatElement(lengths.last!, count: 3 + r.take(2))
            case 17: lengths += repeatElement(0, count: 3 + r.take(3))
            case 18: lengths += repeatElement(0, count: 11 + r.take(7))
            default: preconditionFailure("not a code in the header")
            }
        }
        precondition(r.position == headerBits && literalCount == 286 && distanceCount == 30)
        return (Array(lengths[0..<286]), Array(lengths[286..<316]))
    }

    /// Canonical codes for `lengths` (RFC 1951 3.2.2), bit-reversed: deflate sends Huffman codes from the high bit.
    private static func canonicalCodes(_ lengths: [UInt8]) -> [UInt16] {
        var perLength = [UInt16](repeating: 0, count: 16)
        for l in lengths where l > 0 { perLength[Int(l)] += 1 }
        var nextCode = [UInt16](repeating: 0, count: 16)
        var code: UInt16 = 0
        for bits in 1..<16 {
            code = (code + perLength[bits - 1]) << 1
            nextCode[bits] = code
        }
        return lengths.map { length in
            let l = Int(length)
            guard l > 0 else { return 0 }
            let c = nextCode[l]
            nextCode[l] += 1
            var reversed: UInt16 = 0
            for k in 0..<l { reversed |= (c >> UInt16(k) & 1) << UInt16(l - 1 - k) }
            return reversed
        }
    }

    // ---------- CRC-32 ----------

    /// CRC-32 (the gzip polynomial): whole 16-byte blocks on the carry-less multiply when it is in use, the rest eight
    /// bytes per step from the tables.
    private func crc32(_ p: UnsafePointer<UInt8>, _ n: Int) -> UInt32 {
        var c: UInt32 = 0xFFFF_FFFF
        var i = 0
        #if arch(x86_64)
        if crcPath == .carryless512 && n >= 256 {
            i = n & ~15
            c = crc32Fold512(c, p, count: i)
        } else if crcPath != .tables && n >= 128 {
            i = n & ~15
            c = crc32Fold(c, p, count: i)
        }
        #endif
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

/// Bits into bytes, least significant first, as deflate packs them; as ISA-L's `write_bits`, each write stores the
/// whole 64-bit buffer and moves on by the bytes it filled, with no branch: the output needs 8 bytes of room past it.
private struct BitWriter {
    let out: UnsafeMutablePointer<UInt8>
    private(set) var offset = 0
    private var bits: UInt64 = 0
    /// At most 7 between writes.
    private var count = 0

    init(_ out: UnsafeMutablePointer<UInt8>) {
        self.out = out
    }

    /// `n` bits of `value`, at most 56.
    @inline(__always) mutating func put(_ value: UInt64, _ n: Int) {
        bits |= value &<< UInt64(count)
        count += n
        UnsafeMutableRawPointer(out + offset).storeBytes(of: bits.littleEndian, as: UInt64.self)
        let whole = count & ~7
        offset += whole >> 3
        bits = bits &>> UInt64(whole)
        count -= whole
    }

    /// The first `bits` bits of `bytes`, at a byte boundary.
    mutating func copy(_ bytes: UnsafePointer<UInt8>, bits n: Int) {
        precondition(count == 0)
        let whole = n / 8
        (out + offset).update(from: bytes, count: whole)
        offset += whole
        if n % 8 > 0 { put(UInt64(bytes[whole]) & ((1 << UInt64(n % 8)) - 1), n % 8) }
    }

    mutating func alignToByte() {
        if count > 0 {
            out[offset] = UInt8(truncatingIfNeeded: bits)
            offset += 1
        }
        bits = 0
        count = 0
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

/// Bits out of bytes, least significant first: for reading the header once.
private struct BitReader {
    let bytes: [UInt8]
    private(set) var position = 0

    init(_ bytes: [UInt8]) {
        self.bytes = bytes
    }

    mutating func take(_ n: Int) -> Int {
        var v = 0
        for k in 0..<n {
            v |= Int(bytes[(position + k) / 8] >> UInt8((position + k) % 8) & 1) << k
        }
        position += n
        return v
    }
}
