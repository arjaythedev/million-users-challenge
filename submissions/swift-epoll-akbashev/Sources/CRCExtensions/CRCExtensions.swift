// CRC-32 (gzip's polynomial) on the x86 carry-less multiply (PCLMULQDQ): 128 bytes folded per step in eight 16-byte
// lanes, the lanes folded into one, then down to 32 bits with a Barrett reduction, as ISA-L's crc32_gzip_refl_by8 and
// Intel's "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction" (2009) do it. Folding a lane D
// bits forward multiplies its halves by x^(D+32) and x^(D-32) mod P, bit-reflected. Swift reaches the instruction only
// through its `Builtin` module, which this module imports (with the compiler's hidden -enable-builtin-module flag, as
// SHAExtensions does), and it is built with PCLMULQDQ and SSE4.1 enabled (Package.swift): only a CPU that has them may
// run any of it. Gzip checks CPUID and the result against its tables first; nothing here is inlinable.
#if arch(x86_64)
import Builtin

/// The CRC register (not complemented) after the `count` bytes at `data`, a multiple of 16 and at least 128, starting
/// from `crc`. Never inlined: its instructions must stay in this module.
@inline(never) public func crc32Fold(_ crc: UInt32, _ data: UnsafeRawPointer, count: Int) -> UInt32 {
    // x^(D+32) and x^(D-32) mod P, bit-reflected and shifted left by one, for folding D bits forward.
    let by1024 = SIMD2<UInt64>(0x1_E88E_F372, 0x1_4A7F_E880)
    let by896 = SIMD2<UInt64>(0x1_EA89_367E, 0x1_D7CF_C6AC)
    let by768 = SIMD2<UInt64>(0xDF06_8DC2, 0x1_8CB4_4E58)
    let by640 = SIMD2<UInt64>(0x1_C756_9E54, 0xAE0B_5394)
    let by512 = SIMD2<UInt64>(0x1_5444_2BD4, 0x1_C6E4_1596)
    let by384 = SIMD2<UInt64>(0x3DB1_ECDC, 0x1_7435_9406)
    let by256 = SIMD2<UInt64>(0xF1DA_05AA, 0x1_5A54_6366)
    let by128 = SIMD2<UInt64>(0x1_7519_97D0, 0xCCAA_009E)
    var x0 = load(data, 0) ^ SIMD2(UInt64(crc), 0)
    var x1 = load(data, 16)
    var x2 = load(data, 32)
    var x3 = load(data, 48)
    var x4 = load(data, 64)
    var x5 = load(data, 80)
    var x6 = load(data, 96)
    var x7 = load(data, 112)
    var i = 128
    while count - i >= 128 {
        x0 = fold(x0, by1024) ^ load(data, i)
        x1 = fold(x1, by1024) ^ load(data, i + 16)
        x2 = fold(x2, by1024) ^ load(data, i + 32)
        x3 = fold(x3, by1024) ^ load(data, i + 48)
        x4 = fold(x4, by1024) ^ load(data, i + 64)
        x5 = fold(x5, by1024) ^ load(data, i + 80)
        x6 = fold(x6, by1024) ^ load(data, i + 96)
        x7 = fold(x7, by1024) ^ load(data, i + 112)
        i += 128
    }
    var r = x7 ^ fold(x0, by896) ^ fold(x1, by768) ^ fold(x2, by640) ^ fold(x3, by512) ^ fold(x4, by384)
        ^ fold(x5, by256) ^ fold(x6, by128)
    while count - i >= 16 {
        r = fold(r, by128) ^ load(data, i)
        i += 16
    }
    return reduce(r)
}

/// 128 bits to 64 (with 32 zero bits appended), to 32 more, then Barrett: q = (x mod x^32) * floor(x^64 / P), and the
/// CRC is bits 32...63 of x + (q mod x^32) * P.
@inline(__always) private func reduce(_ r: SIMD2<UInt64>) -> UInt32 {
    let by128 = SIMD2<UInt64>(0x1_7519_97D0, 0xCCAA_009E)
    let x64 = SIMD2<UInt64>(0x1_63CD_6124, 0)
    let barrett = SIMD2<UInt64>(0x1_DB71_0641, 0x1_F701_1641)
    let low32 = SIMD2<UInt64>(0xFFFF_FFFF, 0)
    var x = multiply(by128, r, 0x01) ^ SIMD2(r[1], 0)
    x = multiply(x & low32, x64, 0x00) ^ SIMD2(x[0] >> 32 | x[1] << 32, x[1] >> 32)
    var q = multiply(x & low32, barrett, 0x10)
    q = multiply(q & low32, barrett, 0x00)
    return UInt32(truncatingIfNeeded: (q ^ x)[0] >> 32)
}

@inline(__always) private func load(_ data: UnsafeRawPointer, _ offset: Int) -> SIMD2<UInt64> {
    data.loadUnaligned(fromByteOffset: offset, as: SIMD2<UInt64>.self)
}

/// One lane folded forward: its low half times `k`'s low, its high half times `k`'s high.
@inline(__always) private func fold(_ x: SIMD2<UInt64>, _ k: SIMD2<UInt64>) -> SIMD2<UInt64> {
    multiply(x, k, 0x00) ^ multiply(x, k, 0x11)
}

/// PCLMULQDQ: the carry-less product of `a`'s half chosen by `select`'s bit 0 and `b`'s chosen by its bit 4.
@_transparent private func multiply(_ a: SIMD2<UInt64>, _ b: SIMD2<UInt64>, _ select: Int8) -> SIMD2<UInt64> {
    var r = SIMD2<UInt64>()
    r._storage._value = Builtin.int_x86_pclmulqdq(a._storage._value, b._storage._value, select._value)
    return r
}
#endif
