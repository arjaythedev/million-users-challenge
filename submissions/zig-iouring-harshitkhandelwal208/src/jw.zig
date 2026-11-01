//! JSON response writer: appends to a Buf; strings are escaped with a vectorised scan.
const std = @import("std");
const Buf = @import("buf.zig").Buf;

const V32 = @Vector(32, u8);

/// Writes the JSON escape for `c` (a byte below 0x20, `"` or `\\`) and returns its length (2 or 6).
inline fn escapeOne(dst: [*]u8, c: u8) usize {
    const hex = "0123456789abcdef";
    switch (c) {
        '"' => dst[0..2].* = .{ '\\', '"' },
        '\\' => dst[0..2].* = .{ '\\', '\\' },
        '\n' => dst[0..2].* = .{ '\\', 'n' },
        '\r' => dst[0..2].* = .{ '\\', 'r' },
        '\t' => dst[0..2].* = .{ '\\', 't' },
        else => {
            dst[0..6].* = .{ '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };
            return 6;
        },
    }
    return 2;
}

/// Escapes `s` into `dst` and returns the number of bytes written. `dst` must have room for `6 * s.len` bytes: clean
/// 32-byte blocks are stored unconditionally and only the position of the first byte that needs an escape is
/// extracted from the comparison mask.
pub fn escapeInto(dst0: [*]u8, s: []const u8) usize {
    var dst = dst0;
    var i: usize = 0;
    const lo: V32 = @splat(0x20);
    const quote: V32 = @splat('"');
    const back: V32 = @splat('\\');
    while (i + 32 <= s.len) {
        const v: V32 = s[i..][0..32].*;
        const mask: u32 = @bitCast((v < lo) | (v == quote) | (v == back));
        dst[0..32].* = v;
        if (mask == 0) {
            dst += 32;
            i += 32;
            continue;
        }
        const k = @ctz(mask);
        dst += k;
        i += k;
        dst += escapeOne(dst, s[i]);
        i += 1;
    }
    while (i < s.len) : (i += 1) {
        const c = s[i];
        if (c < 0x20 or c == '"' or c == '\\') {
            dst += escapeOne(dst, c);
        } else {
            dst[0] = c;
            dst += 1;
        }
    }
    return dst - dst0;
}

/// Append `s` as the contents of a JSON string (no surrounding quotes).
pub fn putEscaped(b: *Buf, s: []const u8) void {
    b.ensure(6 * s.len + 32);
    b.len += escapeInto(b.ptr + b.len, s);
}

pub inline fn str(b: *Buf, s: []const u8) void {
    b.ensure(6 * s.len + 34);
    b.ptr[b.len] = '"';
    b.len += 1;
    b.len += escapeInto(b.ptr + b.len, s);
    b.ptr[b.len] = '"';
    b.len += 1;
}

const digits2: [200]u8 = blk: {
    var t: [200]u8 = undefined;
    for (0..100) |i| {
        t[2 * i] = '0' + i / 10;
        t[2 * i + 1] = '0' + i % 10;
    }
    break :blk t;
};

/// Decimal digits of `v` written right-aligned into `buf`; returns the start index.
inline fn digitsInto(buf: *[20]u8, v: u64) usize {
    var i: usize = 20;
    var x = v;
    while (x >= 100) {
        const q = x / 100;
        const r: usize = @intCast(x - q * 100);
        i -= 2;
        buf[i..][0..2].* = digits2[2 * r ..][0..2].*;
        x = q;
    }
    if (x >= 10) {
        i -= 2;
        buf[i..][0..2].* = digits2[2 * @as(usize, @intCast(x)) ..][0..2].*;
    } else {
        i -= 1;
        buf[i] = '0' + @as(u8, @intCast(x));
    }
    return i;
}

pub fn putUint(b: *Buf, v: u64) void {
    var tmp: [20]u8 = undefined;
    const i = digitsInto(&tmp, v);
    b.putSlice(tmp[i..]);
}

pub fn putInt(b: *Buf, v: i64) void {
    if (v < 0) {
        b.put('-');
        putUint(b, @as(u64, @intCast(-(v + 1))) + 1);
    } else putUint(b, @intCast(v));
}

inline fn put2(dst: []u8, v: u64) void {
    dst[0..2].* = digits2[2 * @as(usize, @intCast(v)) ..][0..2].*;
}

/// Calendar date (year, month, day) of a day count since 0000-03-01 (proleptic Gregorian), unsigned arithmetic only.
inline fn civil(z: u64) struct { y: u64, m: u64, d: u64 } {
    const era = z / 146097;
    const doe = z - era * 146097;
    const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const mp = (5 * doy + 2) / 153;
    const d = doy - (153 * mp + 2) / 5 + 1;
    const m = if (mp < 10) mp + 3 else mp - 9;
    return .{ .y = yoe + era * 400 + @intFromBool(m <= 2), .m = m, .d = d };
}

/// The last day formatted, so the (up to) twenty posts of a feed that share a date compute it once. It is a pure
/// function of the day number, not data from Postgres.
threadlocal var last_day: u64 = std.math.maxInt(u64);
threadlocal var last_date: [11]u8 = undefined; // `"YYYY-MM-DD`

/// Postgres timestamptz (binary: microseconds since 2000-01-01 UTC) as RFC 3339 with 3 fractional digits, quoted.
pub fn putTimestamp(b: *Buf, pg_micros: i64) void {
    if (pg_micros < 0 or pg_micros >= 2_900_000 * 86_400_000_000) return putTimestampSlow(b, pg_micros); // years 2000..9937
    b.ensure(32);
    const us: u64 = @intCast(pg_micros);
    const days = us / 86_400_000_000;
    const rem = us - days * 86_400_000_000;
    if (days != last_day) {
        const c = civil(days + 10957 + 719468);
        last_date[0] = '"';
        put2(last_date[1..], c.y / 100);
        put2(last_date[3..], c.y % 100);
        last_date[5] = '-';
        put2(last_date[6..], c.m);
        last_date[8] = '-';
        put2(last_date[9..], c.d);
        last_day = days;
    }
    const secs = rem / 1_000_000;
    const ms = (rem - secs * 1_000_000) / 1000;
    const hh = secs / 3600;
    const mm = (secs - hh * 3600) / 60;
    const ss = secs - hh * 3600 - mm * 60;
    const o = b.ptr + b.len;
    o[0..11].* = last_date;
    o[11] = 'T';
    put2(o[12..14], hh);
    o[14] = ':';
    put2(o[15..17], mm);
    o[17] = ':';
    put2(o[18..20], ss);
    o[20] = '.';
    const msh = ms / 100;
    o[21] = '0' + @as(u8, @intCast(msh));
    put2(o[22..24], ms - msh * 100);
    o[24] = 'Z';
    o[25] = '"';
    b.len += 26;
}

/// General version (any date, including before 2000): the reference for the fast path.
pub fn putTimestampSlow(b: *Buf, pg_micros: i64) void {
    const us_per_day: i64 = 86_400_000_000;
    const days = @divFloor(pg_micros, us_per_day);
    const rem: u64 = @intCast(pg_micros - days * us_per_day);
    // civil from days (days since 2000-01-01 -> days since 0000-03-01 shift)
    const z: i64 = days + 10957 + 719468; // 10957 days from 1970-01-01 to 2000-01-01
    const era = @divFloor(z, 146097);
    const doe: i64 = z - era * 146097;
    const yoe = @divFloor(doe - @divFloor(doe, 1460) + @divFloor(doe, 36524) - @divFloor(doe, 146096), 365);
    var y = yoe + era * 400;
    const doy = doe - (365 * yoe + @divFloor(yoe, 4) - @divFloor(yoe, 100));
    const mp = @divFloor(5 * doy + 2, 153);
    const d = doy - @divFloor(153 * mp + 2, 5) + 1;
    const m = if (mp < 10) mp + 3 else mp - 9;
    if (m <= 2) y += 1;
    const secs = rem / 1_000_000;
    const ms = (rem % 1_000_000) / 1000;
    const hh = secs / 3600;
    const mm = (secs / 60) % 60;
    const ss = secs % 60;
    var o: [26]u8 = undefined;
    const yy: u64 = @intCast(@max(0, @min(9999, y)));
    o[0] = '"';
    o[1] = '0' + @as(u8, @intCast(yy / 1000));
    o[2] = '0' + @as(u8, @intCast(yy / 100 % 10));
    o[3] = '0' + @as(u8, @intCast(yy / 10 % 10));
    o[4] = '0' + @as(u8, @intCast(yy % 10));
    o[5] = '-';
    o[6] = '0' + @as(u8, @intCast(@divFloor(m, 10)));
    o[7] = '0' + @as(u8, @intCast(@mod(m, 10)));
    o[8] = '-';
    o[9] = '0' + @as(u8, @intCast(@divFloor(d, 10)));
    o[10] = '0' + @as(u8, @intCast(@mod(d, 10)));
    o[11] = 'T';
    o[12] = '0' + @as(u8, @intCast(hh / 10));
    o[13] = '0' + @as(u8, @intCast(hh % 10));
    o[14] = ':';
    o[15] = '0' + @as(u8, @intCast(mm / 10));
    o[16] = '0' + @as(u8, @intCast(mm % 10));
    o[17] = ':';
    o[18] = '0' + @as(u8, @intCast(ss / 10));
    o[19] = '0' + @as(u8, @intCast(ss % 10));
    o[20] = '.';
    o[21] = '0' + @as(u8, @intCast(ms / 100));
    o[22] = '0' + @as(u8, @intCast(ms / 10 % 10));
    o[23] = '0' + @as(u8, @intCast(ms % 10));
    o[24] = 'Z';
    o[25] = '"';
    b.putSlice(&o);
}

test "escape" {
    var b = try Buf.init(16);
    defer b.deinit();
    putEscaped(&b, "plain text that is long enough to need several vector chunks 0123456789 and then \"quoted\" \\ back\nslash\x01 end");
    try std.testing.expectEqualStrings("plain text that is long enough to need several vector chunks 0123456789 and then \\\"quoted\\\" \\\\ back\\nslash\\u0001 end", b.slice());
    b.reset();
    putEscaped(&b, "");
    try std.testing.expectEqual(@as(usize, 0), b.len);
    b.reset();
    putEscaped(&b, "ünïcode ✓ ok");
    try std.testing.expectEqualStrings("ünïcode ✓ ok", b.slice());
}

test "timestamp" {
    var b = try Buf.init(32);
    defer b.deinit();
    // 2025-04-01T12:00:00.123456Z -> micros since 2000-01-01
    const days_2000_to_20250401: i64 = 9222; // 25 years * 365 + 7 leap days + 90 days
    const pg: i64 = days_2000_to_20250401 * 86_400_000_000 + 12 * 3_600_000_000 + 123456;
    putTimestamp(&b, pg);
    try std.testing.expectEqualStrings("\"2025-04-01T12:00:00.123Z\"", b.slice());
    b.reset();
    putTimestamp(&b, 0);
    try std.testing.expectEqualStrings("\"2000-01-01T00:00:00.000Z\"", b.slice());
    b.reset();
    putTimestamp(&b, -1); // 1999-12-31T23:59:59.999999
    try std.testing.expectEqualStrings("\"1999-12-31T23:59:59.999Z\"", b.slice());
    b.reset();
    putTimestamp(&b, 825_638_400_000_000); // 2026-03-01 00:00 -> check leap logic around Feb/Mar
    try std.testing.expectEqualStrings("\"2026-03-01T00:00:00.000Z\"", b.slice());
}

test "fast timestamp equals the general one" {
    var a = try Buf.init(64);
    defer a.deinit();
    var b = try Buf.init(64);
    defer b.deinit();
    var prng = std.Random.DefaultPrng.init(42);
    const r = prng.random();
    var k: usize = 0;
    while (k < 200_000) : (k += 1) {
        const v: i64 = switch (k % 4) {
            0 => r.intRangeLessThan(i64, 0, 80_000_000 * 86_400_000_000),
            1 => r.intRangeLessThan(i64, 0, 2_000_000_000_000_000),
            2 => @as(i64, r.intRangeLessThan(i64, 0, 40000)) * 86_400_000_000 + r.intRangeLessThan(i64, -2, 2),
            else => r.intRangeLessThan(i64, -3_000_000_000_000_000, 3_000_000_000_000_000),
        };
        a.reset();
        b.reset();
        putTimestamp(&a, v);
        putTimestampSlow(&b, v);
        try std.testing.expectEqualStrings(b.slice(), a.slice());
    }
}

test "uint digits" {
    var b = try Buf.init(32);
    defer b.deinit();
    const cases = [_]u64{ 0, 1, 9, 10, 99, 100, 101, 999, 1000, 12345, 500000, 4294967295, std.math.maxInt(u64) };
    for (cases) |v| {
        b.reset();
        putUint(&b, v);
        var tmp: [20]u8 = undefined;
        try std.testing.expectEqualStrings(std.fmt.bufPrint(&tmp, "{d}", .{v}) catch unreachable, b.slice());
    }
}

test "escape matches a byte-wise reference on random input" {
    var prng = std.Random.DefaultPrng.init(7);
    const r = prng.random();
    var src: [300]u8 = undefined;
    var out: [6 * 300 + 64]u8 = undefined;
    var k: usize = 0;
    while (k < 20_000) : (k += 1) {
        const n = r.uintLessThan(usize, 300);
        for (src[0..n]) |*c| c.* = switch (r.uintLessThan(u8, 8)) {
            0 => r.uintLessThan(u8, 0x20),
            1 => '"',
            2 => '\\',
            else => 0x20 + r.uintLessThan(u8, 0x5f),
        };
        const got = out[0..escapeInto(&out, src[0..n])];
        var want = try Buf.init(16);
        defer want.deinit();
        for (src[0..n]) |c| {
            if (c < 0x20 or c == '"' or c == '\\') {
                var tmp: [8]u8 = undefined;
                want.putSlice(tmp[0..escapeOne(&tmp, c)]);
            } else want.put(c);
        }
        try std.testing.expectEqualStrings(want.slice(), got);
    }
}
