//! CRC-32 (IEEE, as gzip uses it). Inputs of 64 bytes or more are folded 64 bytes per iteration with carry-less
//! multiplication (PCLMULQDQ) in inline assembly; the remainder uses a byte table.
const std = @import("std");
const builtin = @import("builtin");

const has_clmul = builtin.cpu.arch == .x86_64 and std.Target.x86.featureSetHas(builtin.cpu.features, .pclmul);

const table: [256]u32 = blk: {
    @setEvalBranchQuota(10_000);
    var t: [256]u32 = undefined;
    for (&t, 0..) |*e, i| {
        var c: u32 = @intCast(i);
        for (0..8) |_| c = if (c & 1 != 0) (c >> 1) ^ 0xEDB88320 else c >> 1;
        e.* = c;
    }
    break :blk t;
};

inline fn tableUpdate(crc: u32, data: []const u8) u32 {
    var c = crc;
    for (data) |b| c = table[(c ^ b) & 0xff] ^ (c >> 8);
    return c;
}

const V = @Vector(2, u64);

inline fn clmul(comptime imm: u8, a: V, b: V) V {
    return asm ("pclmulqdq %[imm], %[b], %[r]"
        : [r] "=x" (-> V),
        : [a] "0" (a),
          [b] "x" (b),
          [imm] "i" (imm),
    );
}

inline fn srl8(a: V) V { // _mm_srli_si128(a, 8)
    return asm ("psrldq $8, %[r]"
        : [r] "=x" (-> V),
        : [a] "0" (a),
    );
}

inline fn srl4(a: V) V { // _mm_srli_si128(a, 4)
    return asm ("psrldq $4, %[r]"
        : [r] "=x" (-> V),
        : [a] "0" (a),
    );
}

inline fn load(p: []const u8) V {
    return @bitCast(p[0..16].*);
}

/// Running (not final-xored) CRC state over `buf`, whose length is a multiple of 16 and at least 64.
fn fold(buf: []const u8, crc: u32) u32 {
    const k1k2: V = .{ 0x0154442bd4, 0x01c6e41596 };
    const k3k4: V = .{ 0x01751997d0, 0x00ccaa009e };
    const k5k0: V = .{ 0x0163cd6124, 0 };
    const poly: V = .{ 0x01db710641, 0x01f7011641 };
    const mask32: V = .{ 0x00000000ffffffff, 0x00000000ffffffff };
    var p = buf;
    var x1 = load(p[0..]) ^ V{ crc, 0 };
    var x2 = load(p[16..]);
    var x3 = load(p[32..]);
    var x4 = load(p[48..]);
    p = p[64..];
    while (p.len >= 64) {
        const x5 = clmul(0x00, x1, k1k2);
        const x6 = clmul(0x00, x2, k1k2);
        const x7 = clmul(0x00, x3, k1k2);
        const x8 = clmul(0x00, x4, k1k2);
        x1 = clmul(0x11, x1, k1k2);
        x2 = clmul(0x11, x2, k1k2);
        x3 = clmul(0x11, x3, k1k2);
        x4 = clmul(0x11, x4, k1k2);
        x1 ^= x5 ^ load(p[0..]);
        x2 ^= x6 ^ load(p[16..]);
        x3 ^= x7 ^ load(p[32..]);
        x4 ^= x8 ^ load(p[48..]);
        p = p[64..];
    }
    // fold the four lanes into one
    inline for (.{ x2, x3, x4 }) |nx| {
        const t = clmul(0x00, x1, k3k4);
        x1 = clmul(0x11, x1, k3k4) ^ nx ^ t;
    }
    while (p.len >= 16) {
        const t = clmul(0x00, x1, k3k4);
        x1 = clmul(0x11, x1, k3k4) ^ load(p[0..]) ^ t;
        p = p[16..];
    }
    // 128 -> 64 bits
    var t2 = clmul(0x10, x1, k3k4);
    x1 = srl8(x1) ^ t2;
    t2 = srl4(x1);
    x1 &= mask32;
    x1 = clmul(0x00, x1, k5k0) ^ t2;
    // Barrett reduction to 32 bits
    t2 = x1 & mask32;
    t2 = clmul(0x10, t2, poly);
    t2 &= mask32;
    t2 = clmul(0x00, t2, poly);
    x1 ^= t2;
    const lanes: @Vector(4, u32) = @bitCast(x1);
    return lanes[1];
}

pub fn crc32(data: []const u8) u32 {
    var c: u32 = 0xFFFFFFFF;
    var rest = data;
    if (has_clmul and rest.len >= 64) {
        const n = rest.len & ~@as(usize, 15);
        c = fold(rest[0..n], c);
        rest = rest[n..];
    }
    return ~tableUpdate(c, rest);
}

test "matches std crc32" {
    var prng = std.Random.DefaultPrng.init(42);
    const r = prng.random();
    var buf: [5000]u8 = undefined;
    r.bytes(&buf);
    var n: usize = 0;
    while (n <= buf.len) : (n += if (n < 200) 1 else 37) {
        try std.testing.expectEqual(std.hash.Crc32.hash(buf[0..n]), crc32(buf[0..n]));
    }
    try std.testing.expectEqual(@as(u32, 0xCBF43926), crc32("123456789"));
}
