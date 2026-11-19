// CRC-32 (gzip's polynomial) on AVX-512's carry-less multiply (VPCLMULQDQ, four 128-bit products per instruction): 256
// bytes folded per step in four 64-byte registers, as ISA-L's crc32_gzip_refl_by16_10 does, then the registers and
// their lanes folded into one 16-byte lane and reduced as CRCExtensions does. It is a module of its own because it is
// built with AVX-512 enabled (Package.swift), so the compiler may use AVX-512 anywhere in it: only a CPU with
// AVX-512F, VPCLMULQDQ and PCLMULQDQ may run any of it, which Gzip checks (CPUID, then the result against its tables).
#if arch(x86_64)
import Builtin

/// The CRC register (not complemented) after the `count` bytes at `data`, a multiple of 16 and at least 256, starting
/// from `crc`. Never inlined: its instructions must stay in this module.
@inline(never) public func crc32Fold512(_ crc: UInt32, _ data: UnsafeRawPointer, count: Int) -> UInt32 {
    // x^(D+32) and x^(D-32) mod P, bit-reflected and shifted left by one, for folding D bits forward.
    let by384 = SIMD2<UInt64>(0x3DB1_ECDC, 0x1_7435_9406)
    let by256 = SIMD2<UInt64>(0xF1DA_05AA, 0x1_5A54_6366)
    let by128 = SIMD2<UInt64>(0x1_7519_97D0, 0xCCAA_009E)
    let by2048 = SIMD8<UInt64>(0x1_1542_778A, 0x1_322D_1430, 0x1_1542_778A, 0x1_322D_1430,
                               0x1_1542_778A, 0x1_322D_1430, 0x1_1542_778A, 0x1_322D_1430)
    let by512Wide = SIMD8<UInt64>(0x1_5444_2BD4, 0x1_C6E4_1596, 0x1_5444_2BD4, 0x1_C6E4_1596,
                                  0x1_5444_2BD4, 0x1_C6E4_1596, 0x1_5444_2BD4, 0x1_C6E4_1596)
    var z0 = load512(data, 0) ^ SIMD8(UInt64(crc), 0, 0, 0, 0, 0, 0, 0)
    var z1 = load512(data, 64)
    var z2 = load512(data, 128)
    var z3 = load512(data, 192)
    var i = 256
    while count - i >= 256 {
        z0 = fold512(z0, by2048) ^ load512(data, i)
        z1 = fold512(z1, by2048) ^ load512(data, i + 64)
        z2 = fold512(z2, by2048) ^ load512(data, i + 128)
        z3 = fold512(z3, by2048) ^ load512(data, i + 192)
        i += 256
    }
    // Each register 64 bytes forward onto the next, then the last one's four lanes into its highest.
    z1 ^= fold512(z0, by512Wide)
    z2 ^= fold512(z1, by512Wide)
    z3 ^= fold512(z2, by512Wide)
    var r = SIMD2(z3[6], z3[7]) ^ fold(SIMD2(z3[0], z3[1]), by384) ^ fold(SIMD2(z3[2], z3[3]), by256)
        ^ fold(SIMD2(z3[4], z3[5]), by128)
    while count - i >= 16 {
        r = fold(r, by128) ^ load(data, i)
        i += 16
    }
    return reduce(r)
}

/// As CRCExtensions: 128 bits to 64, to 32 more, then Barrett.
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

@inline(__always) private func load512(_ data: UnsafeRawPointer, _ offset: Int) -> SIMD8<UInt64> {
    data.loadUnaligned(fromByteOffset: offset, as: SIMD8<UInt64>.self)
}

@inline(__always) private func load(_ data: UnsafeRawPointer, _ offset: Int) -> SIMD2<UInt64> {
    data.loadUnaligned(fromByteOffset: offset, as: SIMD2<UInt64>.self)
}

/// Four lanes folded forward at once.
@inline(__always) private func fold512(_ x: SIMD8<UInt64>, _ k: SIMD8<UInt64>) -> SIMD8<UInt64> {
    multiply512(x, k, 0x00) ^ multiply512(x, k, 0x11)
}

@inline(__always) private func fold(_ x: SIMD2<UInt64>, _ k: SIMD2<UInt64>) -> SIMD2<UInt64> {
    multiply(x, k, 0x00) ^ multiply(x, k, 0x11)
}

/// VPCLMULQDQ: in each 128-bit lane, the carry-less product of `a`'s half chosen by `select`'s bit 0 and `b`'s by bit 4.
@_transparent private func multiply512(_ a: SIMD8<UInt64>, _ b: SIMD8<UInt64>, _ select: Int8) -> SIMD8<UInt64> {
    var r = SIMD8<UInt64>()
    r._storage._value = Builtin.int_x86_pclmulqdq_512(a._storage._value, b._storage._value, select._value)
    return r
}

@_transparent private func multiply(_ a: SIMD2<UInt64>, _ b: SIMD2<UInt64>, _ select: Int8) -> SIMD2<UInt64> {
    var r = SIMD2<UInt64>()
    r._storage._value = Builtin.int_x86_pclmulqdq(a._storage._value, b._storage._value, select._value)
    return r
}
#endif
