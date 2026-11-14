//! Constant tables and parameters of the gzip encoder, shared by the Zig and the assembly versions of its main loop.
const std = @import("std");

pub const hash_bits = 12;
/// Matches longer than this index only their first and last positions in the hash table: a long match is rarely
/// referenced from its interior, and skipping the rest saves time for +0.3% size on the feeds measured.
pub const interior_cap: usize = 4;
pub const min_match = 4;
pub const max_match = 258;
pub const max_dist = 32768;

/// Packed (bits | nbits << 16) for each literal, in deflate's LSB-first bit order.
pub const lit_tab: [256]u32 = blk: {
    @setEvalBranchQuota(100_000);
    var t: [256]u32 = undefined;
    for (&t, 0..) |*e, b| {
        const code: u32 = if (b < 144) 0x30 + b else 0x190 + (b - 144);
        const n: u32 = if (b < 144) 8 else 9;
        e.* = rev(code, n) | (n << 16);
    }
    break :blk t;
};

fn rev(v: u32, n: u32) u32 {
    var r: u32 = 0;
    var x = v;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

/// Length symbol (257..285) base values and extra bits.
const len_base = [_]u16{ 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const len_extra = [_]u8{ 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };

/// For match length 3..258: packed (bits | nbits << 16) of the length symbol plus its extra bits.
pub const len_tab: [259]u32 = blk: {
    @setEvalBranchQuota(100_000);
    var t: [259]u32 = undefined;
    @memset(&t, 0);
    var sym: usize = 0;
    while (sym < 29) : (sym += 1) {
        const hi: usize = if (sym == 28) 258 else len_base[sym] + (@as(usize, 1) << @intCast(len_extra[sym])) - 1;
        var l: usize = len_base[sym];
        while (l <= hi and l <= 258) : (l += 1) {
            if (sym == 27 and l == 258) break; // 258 is symbol 285, not the end of symbol 284's range
            const s: u32 = 257 + @as(u32, @intCast(sym));
            const code: u32 = if (s < 280) s - 256 else 0xC0 + (s - 280);
            const cn: u32 = if (s < 280) 7 else 8;
            const extra: u32 = @intCast(l - len_base[sym]);
            t[l] = rev(code, cn) | (extra << @intCast(cn)) | ((cn + len_extra[sym]) << 16);
        }
    }
    break :blk t;
};

pub const dist_code5: [30]u8 = blk: {
    @setEvalBranchQuota(100_000);
    var t: [30]u8 = undefined;
    for (&t, 0..) |*e, i| e.* = @intCast(rev(i, 5));
    break :blk t;
};
