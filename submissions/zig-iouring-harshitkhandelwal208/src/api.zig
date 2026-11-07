//! The five endpoints of SPEC.md: routing, auth, validation, statement parameters and JSON rendering.
const std = @import("std");
const http = @import("http.zig");
const pg = @import("pg.zig");
const jp = @import("jp.zig");
const jw = @import("jw.zig");
const jwt = @import("jwt.zig");
const sql = @import("sql.zig");
const server = @import("server.zig");
const Worker = server.Worker;
const Req = server.Req;
const gpa = @import("buf.zig").gpa;

pub const Kind = enum(u8) { health, feed, post, create, like };

pub const max_body_chars = 500;

// ---------------------------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------------------------

const Ctx = struct { w: *Worker, slot: u32, gen: u32, head: http.Head };

fn errorBody(w: *Worker, msg: []const u8) void {
    w.beginBody();
    w.out.putSlice("{\"error\":\"");
    w.out.putSlice(msg); // fixed messages: nothing to escape
    w.out.putSlice("\"}");
}

fn fail(c: *const Ctx, code: u16, msg: []const u8) void {
    errorBody(c.w, msg);
    c.w.finish(c.slot, c.gen, code, c.head.keep_alive, false);
}

fn failReq(w: *Worker, r: *Req, code: u16, msg: []const u8) void {
    errorBody(w, msg);
    done(w, r, code);
}

fn done(w: *Worker, r: *Req, code: u16) void {
    w.finish(r.slot, r.gen, code, r.keep_alive, r.gzip);
    w.freeReq(r);
}

// ---------------------------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------------------------

pub fn handle(w: *Worker, slot: u32, gen: u32, head: http.Head, body: []const u8) void {
    const c = Ctx{ .w = w, .slot = slot, .gen = gen, .head = head };
    const path = head.path;
    if (head.method == .get) {
        if (std.mem.eql(u8, path, "/feed")) return simple(&c, .feed, sql.Stmt.feed);
        if (std.mem.eql(u8, path, "/health")) return simple(&c, .health, sql.Stmt.health);
        if (std.mem.startsWith(u8, path, "/posts/") and std.mem.indexOfScalarPos(u8, path, 7, '/') == null) {
            return getPost(&c, path[7..]);
        }
    } else if (head.method == .post) {
        if (std.mem.eql(u8, path, "/posts")) return createPost(&c, body);
        // "/posts/" + at least one character + "/like": shorter paths would make the prefix and the suffix overlap
        if (path.len >= 13 and std.mem.startsWith(u8, path, "/posts/") and std.mem.endsWith(u8, path, "/like")) {
            const mid = path[7 .. path.len - 5];
            if (mid.len == 0 or std.mem.indexOfScalar(u8, mid, '/') != null) return fail(&c, 404, "not found");
            return likePost(&c, mid);
        }
    }
    fail(&c, 404, "not found");
}

fn newReq(c: *const Ctx, kind: Kind) *Req {
    const r = c.w.newReq();
    r.slot = c.slot;
    r.gen = c.gen;
    r.kind = kind;
    r.keep_alive = c.head.keep_alive;
    r.gzip = c.head.gzip;
    return r;
}

fn simple(c: *const Ctx, kind: Kind, stmt: sql.Stmt) void {
    const w = c.w;
    w.enc.reset();
    var e = pg.Enc.init(&w.enc);
    e.bind(sql.id(stmt));
    e.exec();
    e.sync();
    w.pgSubmit(.read, newReq(c, kind), w.enc.slice());
}

/// A positive integer made of ASCII digits. `error.TooBig` for digit strings beyond bigint range.
fn parseId(s: []const u8) error{ Invalid, TooBig }!i64 {
    if (s.len == 0) return error.Invalid;
    for (s) |ch| if (ch < '0' or ch > '9') return error.Invalid;
    var t = s;
    while (t.len > 1 and t[0] == '0') t = t[1..];
    if (t.len > 18) return error.TooBig;
    const v = std.fmt.parseInt(i64, t, 10) catch return error.Invalid;
    if (v <= 0) return error.Invalid;
    return v;
}

fn getPost(c: *const Ctx, raw_id: []const u8) void {
    if (raw_id.len == 0) return fail(c, 404, "not found");
    const id = parseId(raw_id) catch |e| switch (e) {
        error.Invalid => return fail(c, 400, "invalid post id"),
        error.TooBig => return fail(c, 404, "post not found"),
    };
    const w = c.w;
    w.enc.reset();
    var en = pg.Enc.init(&w.enc);
    en.bind(sql.id(.post));
    en.pInt8(id);
    en.exec();
    en.sync();
    const r = newReq(c, .post);
    r.aux = id;
    w.pgSubmit(.read, r, w.enc.slice());
}

// ---------------------------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------------------------

const Auth = union(enum) { ok: jwt.Claims, no_token, invalid, payload };

fn authenticate(c: *const Ctx) Auth {
    const h = c.head.auth orelse return .no_token;
    if (!std.mem.startsWith(u8, h, "Bearer ")) return .no_token;
    const tok = std.mem.trim(u8, h[7..], " ");
    if (tok.len == 0) return .invalid;
    return switch (c.w.shared.jwt.verify(tok, c.w.now, &c.w.scratch)) {
        .ok => |cl| .{ .ok = cl },
        .invalid => .invalid,
        .payload => .payload,
    };
}

/// Answers the auth failure; returns the claims when the token is good.
fn requireAuth(c: *const Ctx) ?jwt.Claims {
    switch (authenticate(c)) {
        .ok => |cl| return cl,
        .no_token => fail(c, 401, "missing bearer token"),
        .invalid => fail(c, 401, "invalid or expired token"),
        .payload => fail(c, 401, "invalid token payload"),
    }
    return null;
}

// ---------------------------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------------------------

/// Length of the JavaScript whitespace (String.prototype.trim) at the start of `s`, or 0.
fn wsAt(s: []const u8) usize {
    if (s.len == 0) return 0;
    const b = s[0];
    if (b == ' ' or (b >= 9 and b <= 13)) return 1;
    if (b < 0x80) return 0;
    if (s.len >= 2 and b == 0xc2 and s[1] == 0xa0) return 2; // U+00A0
    if (s.len >= 3) {
        const b1 = s[1];
        const b2 = s[2];
        if (b == 0xe1 and b1 == 0x9a and b2 == 0x80) return 3; // U+1680
        if (b == 0xe2 and b1 == 0x80 and ((b2 >= 0x80 and b2 <= 0x8a) or b2 == 0xa8 or b2 == 0xa9 or b2 == 0xaf)) return 3; // U+2000-200A, 2028, 2029, 202F
        if (b == 0xe2 and b1 == 0x81 and b2 == 0x9f) return 3; // U+205F
        if (b == 0xe3 and b1 == 0x80 and b2 == 0x80) return 3; // U+3000
        if (b == 0xef and b1 == 0xbb and b2 == 0xbf) return 3; // U+FEFF
    }
    return 0;
}

/// Length of the whitespace character that ends `s`, or 0.
fn wsBefore(s: []const u8) usize {
    if (s.len == 0) return 0;
    const b = s[s.len - 1];
    if (b == ' ' or (b >= 9 and b <= 13)) return 1;
    if (b < 0x80) return 0;
    var n: usize = 2;
    while (n <= 3 and n <= s.len) : (n += 1) {
        if (wsAt(s[s.len - n ..]) == n) return n;
    }
    return 0;
}

fn trimJs(s: []const u8) []const u8 {
    var a: usize = 0;
    var b: usize = s.len;
    while (true) {
        const n = wsAt(s[a..b]);
        if (n == 0) break;
        a += n;
    }
    while (b > a) {
        const n = wsBefore(s[a..b]);
        if (n == 0) break;
        b -= n;
    }
    return s[a..b];
}

fn codePoints(s: []const u8) usize {
    var n: usize = 0;
    for (s) |b| n += @intFromBool(b & 0xc0 != 0x80);
    return n;
}

fn createPost(c: *const Ctx, body: []const u8) void {
    const claims = requireAuth(c) orelse return;
    const w = c.w;
    var f: [1]jp.Field = undefined;
    if (body.len == 0) {
        f[0] = .{};
    } else jp.parseObject(body, &w.scratch, &.{"body"}, &f) catch return fail(c, 400, "malformed JSON body");
    if (f[0].kind != .string) return fail(c, 400, "body is required");
    const text = trimJs(f[0].s);
    if (text.len == 0) return fail(c, 400, "body is required");
    if (text.len > max_body_chars and codePoints(text) > max_body_chars) return fail(c, 400, "body must be at most 500 characters");

    const r = newReq(c, .create);
    r.uid = claims.uid;
    r.aux = @intCast(text.len);
    const blob = gpa.alloc(u8, text.len + claims.name.len) catch {
        w.freeReq(r);
        return fail(c, 500, "internal server error");
    };
    @memcpy(blob[0..text.len], text);
    @memcpy(blob[text.len..], claims.name);
    r.blob = blob;
    w.enc.reset();
    var e = pg.Enc.init(&w.enc);
    e.bind(sql.id(.new_post));
    e.pInt8(claims.uid);
    e.pText(text);
    e.exec();
    e.sync();
    w.writeSubmit(r, w.enc.slice());
}

fn likePost(c: *const Ctx, raw_id: []const u8) void {
    const claims = requireAuth(c) orelse return;
    const id = parseId(raw_id) catch |e| switch (e) {
        error.Invalid => return fail(c, 400, "invalid post id"),
        error.TooBig => return fail(c, 404, "post not found"),
    };
    const w = c.w;
    const r = newReq(c, .like);
    r.uid = claims.uid;
    r.aux = id;
    w.enc.reset();
    var e = pg.Enc.init(&w.enc);
    e.bind(sql.id(.like));
    e.pInt8(claims.uid);
    e.pInt8(id);
    e.exec();
    e.sync();
    w.writeSubmit(r, w.enc.slice());
}

// ---------------------------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------------------------

const Reply = struct {
    rd: pg.Reader,
    err: ?[]const u8 = null, // ErrorResponse body
    tag: []const u8 = "", // CommandComplete tag

    fn init(batch: []const u8) Reply {
        return .{ .rd = .{ .data = batch } };
    }

    /// Next DataRow body; records errors and the command tag as it goes.
    fn row(self: *Reply) ?[]const u8 {
        while (self.rd.next()) |m| switch (m.kind) {
            'D' => return m.body,
            'E' => self.err = m.body,
            'C' => self.tag = std.mem.trimEnd(u8, m.body, "\x00"),
            else => {},
        };
        return null;
    }

    fn finish(self: *Reply) void {
        while (self.row()) |_| {}
    }
};

fn writePost(w: *Worker, row: []const u8) void {
    const col = pg.cols(row, 5);
    const b = &w.out;
    b.putSlice("{\"id\":");
    jw.putInt(b, pg.i64Of(col[0].?));
    b.putSlice(",\"body\":");
    jw.str(b, col[1].?);
    b.putSlice(",\"created_at\":");
    jw.putTimestamp(b, pg.i64Of(col[2].?));
    b.putSlice(",\"author\":");
    jw.str(b, col[3].?);
    b.putSlice(",\"like_count\":");
    jw.putInt(b, pg.i32Of(col[4].?));
    b.put('}');
}

pub fn onPg(w: *Worker, r: *Req, batch: []const u8) void {
    var rep = Reply.init(batch);
    switch (r.kind) {
        .health => {
            _ = rep.row();
            rep.finish();
            if (rep.err) |e| return degraded(w, r, errorMessage(e));
            w.beginBody();
            w.out.putSlice("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            jw.putUint(&w.out, w.uptimeSecs());
            w.out.putSlice("}");
            done(w, r, 200);
        },
        .feed => {
            var first = rep.row();
            if (first == null and rep.err != null) return internal(w, r);
            w.beginBody();
            w.out.putSlice("{\"posts\":[");
            var n: usize = 0;
            while (first) |row| : (first = rep.row()) {
                if (n != 0) w.out.put(',');
                n += 1;
                writePost(w, row);
            }
            if (rep.err != null) return internal(w, r);
            w.out.putSlice("]}");
            done(w, r, 200);
        },
        .post => {
            const first = rep.row();
            if (first == null) {
                rep.finish();
                if (rep.err != null) return internal(w, r);
                return failReq(w, r, 404, "post not found");
            }
            w.beginBody();
            w.out.putSlice("{\"post\":");
            writePost(w, first.?);
            w.out.put('}');
            done(w, r, 200);
        },
        .create => {
            const first = rep.row();
            if (first == null) return internal(w, r);
            const col = pg.cols(first.?, 2);
            const text = r.blob[0..@intCast(r.aux)];
            const name = r.blob[@intCast(r.aux)..];
            w.beginBody();
            const b = &w.out;
            b.putSlice("{\"post\":{\"id\":");
            jw.putInt(b, pg.i64Of(col[0].?));
            b.putSlice(",\"body\":");
            jw.str(b, text);
            b.putSlice(",\"created_at\":");
            jw.putTimestamp(b, pg.i64Of(col[1].?));
            b.putSlice(",\"author\":");
            jw.str(b, name);
            b.putSlice(",\"like_count\":0}}");
            done(w, r, 201);
        },
        .like => {
            rep.finish();
            if (rep.err) |e| {
                if (pg.sqlstate(e)) |st| {
                    if (std.mem.eql(u8, st, "23503")) return failReq(w, r, 404, "post not found");
                }
                return internal(w, r);
            }
            // CommandComplete "INSERT 0 1" (new like) or "INSERT 0 0" (already there)
            const fresh = rep.tag.len >= 2 and rep.tag[rep.tag.len - 2] == ' ' and rep.tag[rep.tag.len - 1] == '1';
            if (rep.tag.len == 0) return internal(w, r);
            w.beginBody();
            w.out.putSlice(if (fresh) "{\"liked\":true,\"already_liked\":false,\"post_id\":" else "{\"liked\":true,\"already_liked\":true,\"post_id\":");
            jw.putInt(&w.out, r.aux);
            w.out.put('}');
            done(w, r, if (fresh) 201 else 200);
        },
    }
}

/// The request could not reach Postgres, or its connection broke while the request was in flight.
pub fn onPgFailure(w: *Worker, r: *Req) void {
    if (r.kind == .health) return degraded(w, r, "database connection lost");
    internal(w, r);
}

fn internal(w: *Worker, r: *Req) void {
    failReq(w, r, 500, "internal server error");
}

fn degraded(w: *Worker, r: *Req, msg: []const u8) void {
    w.beginBody();
    w.out.putSlice("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
    jw.str(&w.out, msg);
    w.out.put('}');
    done(w, r, 503);
}

/// The 'M' field of an ErrorResponse.
fn errorMessage(body: []const u8) []const u8 {
    var i: usize = 0;
    while (i < body.len and body[i] != 0) {
        const t = body[i];
        const end = std.mem.indexOfScalarPos(u8, body, i + 1, 0) orelse return "database error";
        if (t == 'M') return body[i + 1 .. end];
        i = end + 1;
    }
    return "database error";
}

test "trim like JavaScript" {
    try std.testing.expectEqualStrings("a b", trimJs("  \t\n a b \r\n"));
    try std.testing.expectEqualStrings("", trimJs(" \u{a0}\u{3000}\u{feff} "));
    try std.testing.expectEqualStrings("é", trimJs("\u{2003}é\u{2028}"));
    try std.testing.expectEqualStrings("x\u{85}", trimJs("x\u{85}")); // NEL is not JS whitespace
    try std.testing.expectEqualStrings("✓", trimJs("✓"));
    try std.testing.expectEqualStrings("", trimJs(""));
}

test "ids" {
    try std.testing.expectEqual(@as(i64, 12), try parseId("12"));
    try std.testing.expectEqual(@as(i64, 7), try parseId("007"));
    try std.testing.expectError(error.Invalid, parseId("0"));
    try std.testing.expectError(error.Invalid, parseId("-1"));
    try std.testing.expectError(error.Invalid, parseId("abc"));
    try std.testing.expectError(error.Invalid, parseId("1.5"));
    try std.testing.expectError(error.Invalid, parseId("+3"));
    try std.testing.expectError(error.Invalid, parseId(""));
    try std.testing.expectError(error.TooBig, parseId("99999999999999999999"));
}
