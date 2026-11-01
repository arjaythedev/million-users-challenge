//! HS256 JWT verification. The HMAC pad states are derived from the secret once at startup; every request still
//! verifies the signature, the algorithm and `exp` in full (no verification cache).
const std = @import("std");
const jp = @import("jp.zig");
const Scratch = @import("scratch.zig").Scratch;
const Sha256 = std.crypto.hash.sha2.Sha256;
const b64 = std.base64.url_safe_no_pad;

const header_b64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"; // {"alg":"HS256","typ":"JWT"}

pub const Claims = struct {
    uid: i64,
    /// Lives in the scratch allocator: valid until it is reset.
    name: []const u8,
};

pub const Result = union(enum) {
    ok: Claims,
    /// Bad signature, expired, not HS256, or malformed.
    invalid,
    /// A genuine token whose `sub` is not a positive integer string or whose `username` is not a string.
    payload,
};

pub const Key = struct {
    inner: Sha256,
    outer: Sha256,

    pub fn init(secret: []const u8) Key {
        var k: [64]u8 = @splat(0);
        if (secret.len > 64) Sha256.hash(secret, k[0..32], .{}) else @memcpy(k[0..secret.len], secret);
        var ip: [64]u8 = undefined;
        var op: [64]u8 = undefined;
        for (k, 0..) |c, i| {
            ip[i] = c ^ 0x36;
            op[i] = c ^ 0x5c;
        }
        var key: Key = .{ .inner = Sha256.init(.{}), .outer = Sha256.init(.{}) };
        key.inner.update(&ip);
        key.outer.update(&op);
        return key;
    }

    pub fn mac(self: *const Key, msg: []const u8) [32]u8 {
        var h = self.inner;
        h.update(msg);
        var d: [32]u8 = undefined;
        h.final(&d);
        var o = self.outer;
        o.update(&d);
        o.final(&d);
        return d;
    }

    pub fn verify(self: *const Key, token: []const u8, now: i64, sc: *Scratch) Result {
        const d1 = std.mem.indexOfScalar(u8, token, '.') orelse return .invalid;
        const d2 = std.mem.indexOfScalarPos(u8, token, d1 + 1, '.') orelse return .invalid;
        const sig_b = token[d2 + 1 ..];
        if (sig_b.len != 43) return .invalid;
        var sig: [32]u8 = undefined;
        b64.Decoder.decode(&sig, sig_b) catch return .invalid;
        const want = self.mac(token[0..d2]);
        var diff: u8 = 0;
        for (sig, want) |a, b| diff |= a ^ b;
        if (diff != 0) return .invalid;
        // The signature is ours; now the header's algorithm (fast path: the canonical header).
        const hdr = token[0..d1];
        if (!std.mem.eql(u8, hdr, header_b64)) {
            var hb: [128]u8 = undefined;
            const hl = b64.Decoder.calcSizeForSlice(hdr) catch return .invalid;
            if (hl > hb.len) return .invalid;
            b64.Decoder.decode(hb[0..hl], hdr) catch return .invalid;
            var f: [1]jp.Field = undefined;
            jp.parseObject(hb[0..hl], sc, &.{"alg"}, &f) catch return .invalid;
            if (f[0].kind != .string or !std.mem.eql(u8, f[0].s, "HS256")) return .invalid;
        }
        var pb: [512]u8 = undefined;
        const pl = b64.Decoder.calcSizeForSlice(token[d1 + 1 .. d2]) catch return .invalid;
        if (pl > pb.len) return .invalid;
        b64.Decoder.decode(pb[0..pl], token[d1 + 1 .. d2]) catch return .invalid;
        var f: [4]jp.Field = undefined;
        jp.parseObject(pb[0..pl], sc, &.{ "sub", "username", "exp", "nbf" }, &f) catch return .invalid;
        // exp: optional, but when present it must be a number in the future
        switch (f[2].kind) {
            .absent => {},
            .number => {
                const exp = parseNumericDate(f[2].s) orelse return .invalid;
                if (exp <= now) return .invalid;
            },
            else => return .invalid,
        }
        if (f[3].kind == .number) {
            const nbf = parseNumericDate(f[3].s) orelse return .invalid;
            if (nbf > now) return .invalid;
        } else if (f[3].kind != .absent and f[3].kind != .null) return .invalid;
        if (f[1].kind != .string) return .payload;
        const sub = f[0].s;
        if (f[0].kind != .string or sub.len == 0 or sub.len > 18) return .payload;
        var uid: i64 = 0;
        for (sub) |c| {
            if (c < '0' or c > '9') return .payload;
            uid = uid * 10 + (c - '0');
        }
        if (uid == 0) return .payload;
        // `f[1].s` may point into the stack buffer above: hand out a copy that lives as long as the scratch allocator.
        const name = sc.alloc(f[1].s.len) catch return .invalid;
        @memcpy(name, f[1].s);
        return .{ .ok = .{ .uid = uid, .name = name } };
    }
};

fn parseNumericDate(s: []const u8) ?i64 {
    if (std.fmt.parseInt(i64, s, 10)) |v| return v else |_| {}
    const f = std.fmt.parseFloat(f64, s) catch return null;
    if (!(f > -1e15 and f < 1e15)) return null;
    return @intFromFloat(@floor(f));
}

fn sign(key: *const Key, out: []u8, payload: []const u8) []const u8 {
    @memcpy(out[0..header_b64.len], header_b64);
    var n: usize = header_b64.len;
    out[n] = '.';
    n += 1;
    n += b64.Encoder.encode(out[n..], payload).len;
    const sig = key.mac(out[0..n]);
    out[n] = '.';
    n += 1;
    n += b64.Encoder.encode(out[n..], &sig).len;
    return out[0..n];
}

test "claims" {
    var sc = try Scratch.init(1024);
    const key = Key.init("million-users-challenge");
    var buf: [400]u8 = undefined;
    const now: i64 = 1_700_000_000;

    const ok = key.verify(sign(&key, &buf, "{\"sub\":\"42\",\"username\":\"golden_ember_1\",\"iat\":1,\"exp\":1700003600}"), now, &sc);
    try std.testing.expectEqual(@as(i64, 42), ok.ok.uid);
    try std.testing.expectEqualStrings("golden_ember_1", ok.ok.name);

    try std.testing.expect(key.verify(sign(&key, &buf, "{\"sub\":\"42\",\"username\":\"a\",\"exp\":1699999999}"), now, &sc) == .invalid);
    try std.testing.expect(key.verify(sign(&key, &buf, "{\"sub\":\"abc\",\"username\":\"a\",\"exp\":1700003600}"), now, &sc) == .payload);
    try std.testing.expect(key.verify(sign(&key, &buf, "{\"sub\":42,\"username\":\"a\",\"exp\":1700003600}"), now, &sc) == .payload);
    try std.testing.expect(key.verify(sign(&key, &buf, "{\"sub\":\"0\",\"username\":\"a\",\"exp\":1700003600}"), now, &sc) == .payload);
    try std.testing.expect(key.verify(sign(&key, &buf, "{\"sub\":\"4\",\"exp\":1700003600}"), now, &sc) == .payload);
    try std.testing.expect(key.verify(sign(&key, &buf, "{\"sub\":\"4\",\"username\":7,\"exp\":1700003600}"), now, &sc) == .payload);

    // signature tampering and structure
    const t = sign(&key, &buf, "{\"sub\":\"1\",\"username\":\"a\",\"exp\":1700003600}");
    var bad: [400]u8 = undefined;
    @memcpy(bad[0..t.len], t);
    bad[t.len - 1] = if (bad[t.len - 1] == 'A') 'B' else 'A';
    try std.testing.expect(key.verify(bad[0..t.len], now, &sc) == .invalid);
    try std.testing.expect(key.verify("abc", now, &sc) == .invalid);
    try std.testing.expect(key.verify("", now, &sc) == .invalid);
    const other = Key.init("another secret");
    try std.testing.expect(other.verify(t, now, &sc) == .invalid);
}
