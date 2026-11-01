//! Single-pass gzip: greedy LZ77 over a two-way hash table, emitted straight into one fixed-Huffman deflate block.
//!
//! Fixed Huffman is about 20% larger than dynamic blocks on JSON feeds like this one (2.36 KB against 1.96 KB for a
//! 5.1 KB feed) but needs no histogram, no tree and no second pass, and both fit the same number of TCP segments.
//! There are no tokens in between: every literal and match is written to the bit accumulator as it is found.
const std = @import("std");
const crc32 = @import("crc32.zig").crc32;

const tabs = @import("deflate_tabs.zig");
const core = @import("deflate_core.zig");
const hash_bits = tabs.hash_bits;
const interior_cap = tabs.interior_cap;
const min_match = tabs.min_match;
const max_match = tabs.max_match;
const max_dist = tabs.max_dist;
const lit_tab = tabs.lit_tab;
const len_tab = tabs.len_tab;
const dist_code5 = tabs.dist_code5;

/// Worst-case output size for `n` input bytes (every byte a 9-bit literal, plus header, end of block and trailer).
pub fn bound(n: usize) usize {
    return n + n / 8 + 10 + 8 + 16;
}

/// Test hook: set to compare the assembly loop with the Zig one.
pub var force_zig: bool = false;

pub const Deflater = struct {
    /// Two most recent positions per hash bucket, stored as position + base so the table never needs clearing.
    tab: [2 << hash_bits]u32 = @splat(0),
    base: u32 = 1,

    inline fn load32(p: []const u8, i: usize) u32 {
        return std.mem.readInt(u32, p[i..][0..4], .little);
    }

    /// Writes a gzip member for `in` into `out` (at least `bound(in.len)` bytes). Returns its length.
    pub fn gzip(self: *Deflater, out: []u8, in: []const u8) usize {
        std.debug.assert(out.len >= bound(in.len));
        const hdr = [10]u8{ 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3 };
        @memcpy(out[0..10], &hdr);
        var pos: usize = 10;
        var acc: u64 = 0b011; // BFINAL = 1, BTYPE = 01 (fixed Huffman), LSB first
        var nb: u6 = 3;

        // Table entries are positions + base; keep them from wrapping.
        if (self.base > 0xF000_0000) {
            @memset(&self.tab, 0);
            self.base = 1;
        }
        const base = self.base;
        self.base +%= @intCast(in.len + max_dist + 1);

        const n = in.len;
        var i: usize = 0;
        // The match finder reads 4 bytes at i and compares 8 at a time; stop it where that could run past the end.
        const limit: usize = if (n >= 8) n - 8 else 0;
        const use_core = core.available and n >= 8 and n <= max_dist + 8 and !force_zig;
        if (use_core) {
            var st: core.Core = .{ .in = in.ptr, .n = n, .tab = &self.tab, .base = base, .out = out.ptr + pos, .acc = acc, .nb = nb, .i = 0 };
            core.mu_deflate_core(&st);
            pos = st.out - out.ptr;
            acc = st.acc;
            nb = @intCast(st.nb);
            i = st.i;
        }
        while (i < limit and !use_core) {
            const h = (load32(in, i) *% 0x9E3779B1) >> (32 - hash_bits);
            const slot = h * 2;
            const c0 = self.tab[slot];
            const c1 = self.tab[slot + 1];
            self.tab[slot + 1] = c0;
            self.tab[slot] = base +% @as(u32, @intCast(i));

            var best_len: usize = 0;
            var best_dist: usize = 0;
            inline for (.{ c0, c1 }) |c| {
                if (c >= base) {
                    const cp: usize = c - base;
                    const d = i - cp;
                    if (cp < i and d <= max_dist and load32(in, cp) == load32(in, i)) {
                        var l: usize = 4;
                        const maxl = @min(max_match, n - i);
                        while (l + 8 <= maxl) {
                            const x = std.mem.readInt(u64, in[cp + l ..][0..8], .little) ^ std.mem.readInt(u64, in[i + l ..][0..8], .little);
                            if (x != 0) {
                                l += @ctz(x) >> 3;
                                break;
                            }
                            l += 8;
                        } else {
                            while (l < maxl and in[cp + l] == in[i + l]) l += 1;
                        }
                        if (l > best_len) {
                            best_len = @min(l, maxl);
                            best_dist = d;
                        }
                    }
                }
            }

            if (best_len >= min_match) {
                const lt = len_tab[best_len];
                const d1 = best_dist - 1;
                var dbits: u32 = undefined;
                var dn: u32 = undefined;
                if (d1 < 4) {
                    dbits = dist_code5[d1];
                    dn = 5;
                } else {
                    const b: u6 = @intCast(63 - @clz(@as(u64, d1)));
                    const eb: u6 = b - 1;
                    const code: usize = 2 * @as(usize, b) + ((d1 >> eb) & 1);
                    dbits = dist_code5[code] | (@as(u32, @intCast(d1 & ((@as(usize, 1) << eb) - 1))) << 5);
                    dn = 5 + @as(u32, eb);
                }
                // length symbol + extra, then distance symbol + extra: at most 13 + 18 bits
                acc |= @as(u64, lt & 0xFFFF) << nb;
                nb += @intCast(lt >> 16);
                if (nb >= 32) {
                    std.mem.writeInt(u32, out[pos..][0..4], @truncate(acc), .little);
                    pos += 4;
                    acc >>= 32;
                    nb -= 32;
                }
                acc |= @as(u64, dbits) << nb;
                nb += @intCast(dn);
                if (nb >= 32) {
                    std.mem.writeInt(u32, out[pos..][0..4], @truncate(acc), .little);
                    pos += 4;
                    acc >>= 32;
                    nb -= 32;
                }
                // index the interior of the match so later text can refer to it
                var k: usize = i + 1;
                const end = @min(i + best_len, limit);
                if (best_len > interior_cap) k = @max(k, end -| 2);
                while (k < end) : (k += 1) {
                    const hk = (load32(in, k) *% 0x9E3779B1) >> (32 - hash_bits);
                    self.tab[hk * 2 + 1] = self.tab[hk * 2];
                    self.tab[hk * 2] = base +% @as(u32, @intCast(k));
                }
                i += best_len;
            } else {
                const lt = lit_tab[in[i]];
                acc |= @as(u64, lt & 0xFFFF) << nb;
                nb += @intCast(lt >> 16);
                if (nb >= 32) {
                    std.mem.writeInt(u32, out[pos..][0..4], @truncate(acc), .little);
                    pos += 4;
                    acc >>= 32;
                    nb -= 32;
                }
                i += 1;
            }
        }
        // tail: literals only
        while (i < n) : (i += 1) {
            const lt = lit_tab[in[i]];
            acc |= @as(u64, lt & 0xFFFF) << nb;
            nb += @intCast(lt >> 16);
            if (nb >= 32) {
                std.mem.writeInt(u32, out[pos..][0..4], @truncate(acc), .little);
                pos += 4;
                acc >>= 32;
                nb -= 32;
            }
        }
        // end of block (symbol 256: seven zero bits), then pad to a byte
        nb += 7;
        if (nb >= 32) {
            std.mem.writeInt(u32, out[pos..][0..4], @truncate(acc), .little);
            pos += 4;
            acc >>= 32;
            nb -= 32;
        }
        var left: u8 = nb;
        while (left > 0) {
            out[pos] = @truncate(acc);
            pos += 1;
            acc >>= 8;
            left -|= 8;
        }
        std.mem.writeInt(u32, out[pos..][0..4], crc32(in), .little);
        std.mem.writeInt(u32, out[pos + 4 ..][0..4], @truncate(n), .little);
        return pos + 8;
    }
};

test "assembly main loop produces the same bytes as the Zig loop" {
    if (!core.available) return error.SkipZigTest;
    const gpa = std.testing.allocator;
    var prng = std.Random.DefaultPrng.init(0xDEF1A7E);
    const r = prng.random();
    const a = try gpa.create(Deflater);
    defer gpa.destroy(a);
    const b = try gpa.create(Deflater);
    defer gpa.destroy(b);
    a.* = .{};
    b.* = .{};
    const vocab = [_][]const u8{ "says hi at ", "2026-10-24T13:00:", "\",\"created_at\":\"", "{\"id\":", "author", "keepalive ", "the ", "a", "VU ", "iter ", "load ", "x" };
    const in = try gpa.alloc(u8, 40000);
    defer gpa.free(in);
    const out1 = try gpa.alloc(u8, bound(40000));
    defer gpa.free(out1);
    const out2 = try gpa.alloc(u8, bound(40000));
    defer gpa.free(out2);
    var round: usize = 0;
    while (round < 4000) : (round += 1) {
        const n: usize = switch (round % 5) {
            0 => r.uintLessThan(usize, 40),
            1 => r.uintLessThan(usize, 600),
            2 => r.uintLessThan(usize, 5000),
            3 => 32768 + r.uintLessThan(usize, 12) - 6, // around the size the assembly path accepts
            else => r.uintLessThan(usize, 40000),
        };
        const kind = r.uintLessThan(u8, 5);
        var p: usize = 0;
        while (p < n) {
            switch (kind) {
                0 => { // random bytes
                    in[p] = r.int(u8);
                    p += 1;
                },
                1 => { // long runs
                    const len = @min(n - p, 1 + r.uintLessThan(usize, 600));
                    @memset(in[p..][0..len], r.int(u8));
                    p += len;
                },
                2 => { // words from a small vocabulary
                    const w = vocab[r.uintLessThan(usize, vocab.len)];
                    const len = @min(n - p, w.len);
                    @memcpy(in[p..][0..len], w[0..len]);
                    p += len;
                },
                3 => { // digits and a few separators (timestamps, ids)
                    in[p] = "0123456789-:.TZ,\""[r.uintLessThan(usize, 17)];
                    p += 1;
                },
                else => { // copies of earlier text at random distances
                    if (p < 8) {
                        in[p] = r.int(u8);
                        p += 1;
                    } else {
                        const d = 1 + r.uintLessThan(usize, @min(p, 40000));
                        const len = @min(n - p, 4 + r.uintLessThan(usize, 300));
                        for (0..len) |k| in[p + k] = in[p + k - d];
                        p += len;
                    }
                },
            }
        }
        force_zig = false;
        const n1 = a.gzip(out1, in[0..n]);
        force_zig = true;
        const n2 = b.gzip(out2, in[0..n]);
        force_zig = false;
        try std.testing.expectEqual(n2, n1);
        try std.testing.expectEqualSlices(u8, out2[0..n2], out1[0..n1]);
    }
}
