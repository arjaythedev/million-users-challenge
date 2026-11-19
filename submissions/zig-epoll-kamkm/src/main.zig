//! One epoll loop per core. A loop owns its listening sockets, the clients it
//! accepted and its PostgreSQL sessions, so a request is read, parsed, queued,
//! answered and written on one thread. libpq connects, authenticates and
//! prepares the statements; the loop then reads and writes the session's socket
//! itself (see "PostgreSQL sessions" below).
const std = @import("std");
const http = @import("http.zig");
const c = @cImport({
    @cInclude("errno.h");
    @cInclude("fcntl.h");
    @cInclude("igzip_lib.h");
    @cInclude("libpq-fe.h");
    @cInclude("netdb.h");
    @cInclude("netinet/in.h");
    @cInclude("netinet/tcp.h");
    @cInclude("openssl/evp.h");
    @cInclude("openssl/hmac.h");
    @cInclude("sys/epoll.h");
    @cInclude("sys/eventfd.h");
    @cInclude("sys/socket.h");
    @cInclude("sys/types.h");
    @cInclude("time.h");
    @cInclude("unistd.h");
});
extern "c" fn accept4(fd: c_int, address: ?*anyopaque, length: ?*anyopaque, flags: c_int) c_int;

const Allocator = std.mem.Allocator;
const MAX_REQUEST = http.max_request;
const DB_FEED_SQL = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const DB_POST_SQL = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1";
const DB_INSERT_SQL = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
const DB_LIKE_SQL = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";

const EVENT_BATCH = 1024;
const READ_CHUNK = 64 * 1024;
const ACCEPT_BURST = 256;
const WRITE_GROUP = 64;
const MAX_BATCH = 256;
const WIRE_BUFFER = 256 * 1024;
const DATABASE_TIMEOUT_US: i64 = 5_000_000;
const REDIAL_US: i64 = 250_000;

// epoll event bits (Linux ABI).
const EV_IN: u32 = 0x001;
const EV_OUT: u32 = 0x004;
const EV_ERR: u32 = 0x008;
const EV_HUP: u32 = 0x010;
const EV_RDHUP: u32 = 0x2000;
const EV_ET: u32 = 1 << 31;

// What an epoll event refers to, in the low bits of its data word. A client
// is a pointer (aligned, so its low bits are zero); the others carry a
// descriptor or an index above the tag.
const TAG_CLIENT: u64 = 0;
const TAG_LISTENER: u64 = 1;
const TAG_WAKE: u64 = 2;
const TAG_SESSION: u64 = 3;

const Plan = struct {
    readers: usize,
    writers: usize,
    batch: usize,
    read_window: i64,
    write_window: i64,
};

const App = struct {
    allocator: Allocator,
    db_url: [:0]const u8,
    jwt_secret: []const u8,
    started_ns: i128,
    plan: Plan,
};

const HttpRequest = http.Request;
threadlocal var chunk_body: [MAX_REQUEST]u8 = undefined;

const Route = enum { health, feed, post, create, like, unknown };

const Auth = struct {
    user_id: u64,
    username: []const u8,
};

const BodyStatus = enum { ok, malformed, required, too_long };
const BodyResult = struct { status: BodyStatus, body: []u8 };

const JsonParser = struct {
    data: []const u8,
    pos: usize = 0,
    parsed_len: usize = 0,

    fn ws(self: *JsonParser) void {
        while (self.pos < self.data.len and isJsonWs(self.data[self.pos])) self.pos += 1;
    }

    fn parseString(self: *JsonParser, out: []u8) !usize {
        if (self.pos >= self.data.len or self.data[self.pos] != '"') return error.InvalidJson;
        self.pos += 1;
        var n: usize = 0;
        while (self.pos < self.data.len) {
            const ch = self.data[self.pos];
            self.pos += 1;
            if (ch == '"') return n;
            if (ch < 0x20) return error.InvalidJson;
            if (ch != '\\') {
                if (n >= out.len) return error.BufferTooSmall;
                out[n] = ch;
                n += 1;
                continue;
            }
            if (self.pos >= self.data.len) return error.InvalidJson;
            const esc = self.data[self.pos];
            self.pos += 1;
            switch (esc) {
                '"', '\\', '/' => {
                    if (n >= out.len) return error.BufferTooSmall;
                    out[n] = esc;
                    n += 1;
                },
                'b' => {
                    if (n >= out.len) return error.BufferTooSmall;
                    out[n] = 8;
                    n += 1;
                },
                'f' => {
                    if (n >= out.len) return error.BufferTooSmall;
                    out[n] = 12;
                    n += 1;
                },
                'n' => {
                    if (n >= out.len) return error.BufferTooSmall;
                    out[n] = '\n';
                    n += 1;
                },
                'r' => {
                    if (n >= out.len) return error.BufferTooSmall;
                    out[n] = '\r';
                    n += 1;
                },
                't' => {
                    if (n >= out.len) return error.BufferTooSmall;
                    out[n] = '\t';
                    n += 1;
                },
                'u' => {
                    const cp = try self.parseUnicodeEscape();
                    n += try appendCodepoint(out[n..], cp);
                },
                else => return error.InvalidJson,
            }
        }
        return error.InvalidJson;
    }

    fn parseUnicodeEscape(self: *JsonParser) !u21 {
        if (self.pos + 4 > self.data.len) return error.InvalidJson;
        const first = try hex4(self.data[self.pos .. self.pos + 4]);
        self.pos += 4;
        if (first >= 0xd800 and first <= 0xdbff) {
            if (self.pos + 6 > self.data.len or self.data[self.pos] != '\\' or self.data[self.pos + 1] != 'u') return error.InvalidJson;
            const second = try hex4(self.data[self.pos + 2 .. self.pos + 6]);
            if (second < 0xdc00 or second > 0xdfff) return error.InvalidJson;
            self.pos += 6;
            return @intCast(0x10000 + (@as(u32, first) - 0xd800) * 0x400 + (@as(u32, second) - 0xdc00));
        }
        if (first >= 0xdc00 and first <= 0xdfff) return error.InvalidJson;
        return first;
    }

    fn skipString(self: *JsonParser) !void {
        var scratch: [1]u8 = undefined;
        _ = self.parseString(&scratch) catch |err| switch (err) {
            error.BufferTooSmall => return self.skipStringSlow(),
            else => return err,
        };
    }

    fn skipStringSlow(self: *JsonParser) !void {
        // This path handles strings larger than the one-byte scratch without allocating.
        if (self.pos >= self.data.len or self.data[self.pos] != '"') return error.InvalidJson;
        self.pos += 1;
        var escaped = false;
        while (self.pos < self.data.len) {
            const ch = self.data[self.pos];
            self.pos += 1;
            if (escaped) {
                escaped = false;
                if (ch == 'u') {
                    if (self.pos + 4 > self.data.len) return error.InvalidJson;
                    _ = try hex4(self.data[self.pos .. self.pos + 4]);
                    self.pos += 4;
                } else if (ch != '"' and ch != '\\' and ch != '/' and ch != 'b' and ch != 'f' and ch != 'n' and ch != 'r' and ch != 't') return error.InvalidJson;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                return;
            } else if (ch < 0x20) return error.InvalidJson;
        }
        return error.InvalidJson;
    }

    fn skipValue(self: *JsonParser, depth: usize) !void {
        if (depth > 16) return error.InvalidJson;
        self.ws();
        if (self.pos >= self.data.len) return error.InvalidJson;
        switch (self.data[self.pos]) {
            '"' => try self.skipStringSlow(),
            '{' => {
                self.pos += 1;
                self.ws();
                if (self.pos < self.data.len and self.data[self.pos] == '}') {
                    self.pos += 1;
                    return;
                }
                while (true) {
                    try self.skipStringSlow();
                    self.ws();
                    if (self.pos >= self.data.len or self.data[self.pos] != ':') return error.InvalidJson;
                    self.pos += 1;
                    try self.skipValue(depth + 1);
                    self.ws();
                    if (self.pos >= self.data.len) return error.InvalidJson;
                    if (self.data[self.pos] == '}') {
                        self.pos += 1;
                        return;
                    }
                    if (self.data[self.pos] != ',') return error.InvalidJson;
                    self.pos += 1;
                    self.ws();
                }
            },
            '[' => {
                self.pos += 1;
                self.ws();
                if (self.pos < self.data.len and self.data[self.pos] == ']') {
                    self.pos += 1;
                    return;
                }
                while (true) {
                    try self.skipValue(depth + 1);
                    self.ws();
                    if (self.pos >= self.data.len) return error.InvalidJson;
                    if (self.data[self.pos] == ']') {
                        self.pos += 1;
                        return;
                    }
                    if (self.data[self.pos] != ',') return error.InvalidJson;
                    self.pos += 1;
                    self.ws();
                }
            },
            't' => try self.literal("true"),
            'f' => try self.literal("false"),
            'n' => try self.literal("null"),
            '-', '0'...'9' => try self.number(),
            else => return error.InvalidJson,
        }
    }

    fn literal(self: *JsonParser, lit: []const u8) !void {
        if (self.pos + lit.len > self.data.len or !std.mem.eql(u8, self.data[self.pos .. self.pos + lit.len], lit)) return error.InvalidJson;
        self.pos += lit.len;
    }

    fn number(self: *JsonParser) !void {
        const start = self.pos;
        if (self.data[self.pos] == '-') self.pos += 1;
        if (self.pos >= self.data.len) return error.InvalidJson;
        if (self.data[self.pos] == '0') self.pos += 1 else {
            if (self.data[self.pos] < '1' or self.data[self.pos] > '9') return error.InvalidJson;
            while (self.pos < self.data.len and self.data[self.pos] >= '0' and self.data[self.pos] <= '9') self.pos += 1;
        }
        if (self.pos < self.data.len and self.data[self.pos] == '.') {
            self.pos += 1;
            if (self.pos >= self.data.len or self.data[self.pos] < '0' or self.data[self.pos] > '9') return error.InvalidJson;
            while (self.pos < self.data.len and self.data[self.pos] >= '0' and self.data[self.pos] <= '9') self.pos += 1;
        }
        if (self.pos < self.data.len and (self.data[self.pos] == 'e' or self.data[self.pos] == 'E')) {
            self.pos += 1;
            if (self.pos < self.data.len and (self.data[self.pos] == '+' or self.data[self.pos] == '-')) self.pos += 1;
            if (self.pos >= self.data.len or self.data[self.pos] < '0' or self.data[self.pos] > '9') return error.InvalidJson;
            while (self.pos < self.data.len and self.data[self.pos] >= '0' and self.data[self.pos] <= '9') self.pos += 1;
        }
        if (self.pos == start) return error.InvalidJson;
    }

    fn parseBody(self: *JsonParser, out: []u8) BodyStatus {
        if (!std.unicode.utf8ValidateSlice(self.data)) return .malformed;
        self.ws();
        if (self.pos >= self.data.len) return .malformed;
        if (self.data[self.pos] != '{') {
            self.skipValue(0) catch return .malformed;
            self.ws();
            return if (self.pos == self.data.len) .required else .malformed;
        }
        self.pos += 1;
        var key_buf: [128]u8 = undefined;
        var found = false;
        var body_len: usize = 0;
        self.ws();
        if (self.pos < self.data.len and self.data[self.pos] == '}') {
            self.pos += 1;
            self.ws();
            return if (self.pos == self.data.len) .required else .malformed;
        }
        while (true) {
            const key_len = self.parseString(&key_buf) catch return .malformed;
            self.ws();
            if (self.pos >= self.data.len or self.data[self.pos] != ':') return .malformed;
            self.pos += 1;
            self.ws();
            if (std.mem.eql(u8, key_buf[0..key_len], "body")) {
                if (self.pos >= self.data.len or self.data[self.pos] != '"') {
                    self.skipValue(0) catch return .malformed;
                    found = true;
                    body_len = 0;
                } else {
                    body_len = self.parseString(out) catch return .malformed;
                    found = true;
                }
            } else self.skipValue(0) catch return .malformed;
            self.ws();
            if (self.pos >= self.data.len) return .malformed;
            if (self.data[self.pos] == '}') {
                self.pos += 1;
                break;
            }
            if (self.data[self.pos] != ',') return .malformed;
            self.pos += 1;
            self.ws();
        }
        self.ws();
        if (self.pos != self.data.len) return .malformed;
        if (!found) return .required;
        const trimmed = trimBodySpace(out[0..body_len]);
        if (trimmed.len == 0) return .required;
        if (utf8Count(trimmed) > 500) return .too_long;
        std.mem.copyForwards(u8, out[0..trimmed.len], trimmed);
        self.parsed_len = trimmed.len;
        return .ok;
    }
};

fn isJsonWs(ch: u8) bool {
    return ch == ' ' or ch == '\n' or ch == '\r' or ch == '\t';
}

fn trimSpace(value: []const u8) []const u8 {
    var a: usize = 0;
    var b = value.len;
    while (a < b and isSpace(value[a])) a += 1;
    while (b > a and isSpace(value[b - 1])) b -= 1;
    return value[a..b];
}
fn isSpace(ch: u8) bool {
    return ch == ' ' or ch == '\t' or ch == '\r' or ch == '\n' or ch == '\x0b' or ch == '\x0c';
}

// Unicode White_Space, matching the string trimming used by the other servers.
fn trimBodySpace(value: []const u8) []const u8 {
    var cursor: usize = 0;
    var start: usize = 0;
    var end: usize = 0;
    var found = false;
    while (cursor < value.len) {
        const width = std.unicode.utf8ByteSequenceLength(value[cursor]) catch return value;
        if (width > value.len - cursor) return value;
        const cp = std.unicode.utf8Decode(value[cursor .. cursor + width]) catch return value;
        const whitespace = switch (cp) {
            0x09...0x0d, 0x20, 0x85, 0xa0, 0x1680, 0x2000...0x200a, 0x2028, 0x2029, 0x202f, 0x205f, 0x3000 => true,
            else => false,
        };
        if (!whitespace) {
            if (!found) start = cursor;
            found = true;
            end = cursor + width;
        }
        cursor += width;
    }
    return value[start..end];
}

fn utf8Count(value: []const u8) usize {
    var i: usize = 0;
    var n: usize = 0;
    while (i < value.len) {
        const ch = value[i];
        if (ch < 0x80) i += 1 else if (ch & 0xe0 == 0xc0 and i + 1 < value.len) i += 2 else if (ch & 0xf0 == 0xe0 and i + 2 < value.len) i += 3 else if (ch & 0xf8 == 0xf0 and i + 3 < value.len) i += 4 else i += 1;
        n += 1;
    }
    return n;
}

fn hex4(value: []const u8) !u16 {
    if (value.len < 4) return error.InvalidJson;
    var out: u16 = 0;
    for (value[0..4]) |ch| {
        out <<= 4;
        out |= switch (ch) {
            '0'...'9' => ch - '0',
            'a'...'f' => ch - 'a' + 10,
            'A'...'F' => ch - 'A' + 10,
            else => return error.InvalidJson,
        };
    }
    return out;
}

fn appendCodepoint(out: []u8, cp: u21) !usize {
    if (cp <= 0x7f) {
        if (out.len < 1) return error.BufferTooSmall;
        out[0] = @intCast(cp);
        return 1;
    }
    if (cp <= 0x7ff) {
        if (out.len < 2) return error.BufferTooSmall;
        out[0] = @intCast(0xc0 | (cp >> 6));
        out[1] = @intCast(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp <= 0xffff) {
        if (out.len < 3) return error.BufferTooSmall;
        out[0] = @intCast(0xe0 | (cp >> 12));
        out[1] = @intCast(0x80 | ((cp >> 6) & 0x3f));
        out[2] = @intCast(0x80 | (cp & 0x3f));
        return 3;
    }
    if (out.len < 4) return error.BufferTooSmall;
    out[0] = @intCast(0xf0 | (cp >> 18));
    out[1] = @intCast(0x80 | ((cp >> 12) & 0x3f));
    out[2] = @intCast(0x80 | ((cp >> 6) & 0x3f));
    out[3] = @intCast(0x80 | (cp & 0x3f));
    return 4;
}

fn parseHttp(raw: []const u8) ?HttpRequest {
    return http.parseRequest(raw, &chunk_body) catch null;
}

fn routeFor(method: []const u8, path: []const u8) Route {
    if (std.mem.eql(u8, method, "GET") and std.mem.eql(u8, path, "/health")) return .health;
    if (std.mem.eql(u8, method, "GET") and std.mem.eql(u8, path, "/feed")) return .feed;
    if (std.mem.eql(u8, method, "POST") and std.mem.eql(u8, path, "/posts")) return .create;
    if (std.mem.eql(u8, method, "GET") and std.mem.startsWith(u8, path, "/posts/")) {
        const id_part = path[7..];
        if (id_part.len > 0 and std.mem.indexOfScalar(u8, id_part, '/') == null) return .post;
    }
    if (std.mem.eql(u8, method, "POST") and std.mem.startsWith(u8, path, "/posts/") and std.mem.endsWith(u8, path, "/like")) {
        const id_part = path[7 .. path.len - 5];
        if (id_part.len > 0 and std.mem.indexOfScalar(u8, id_part, '/') == null) return .like;
    }
    return .unknown;
}

fn parsePositiveId(value: []const u8) ?u64 {
    if (value.len == 0) return null;
    var out: u64 = 0;
    for (value) |ch| {
        if (ch < '0' or ch > '9') return null;
        const digit: u64 = ch - '0';
        if (out > (std.math.maxInt(u64) - digit) / 10) return null;
        out = out * 10 + digit;
    }
    if (out == 0 or out > std.math.maxInt(i64)) return null;
    return out;
}

fn postIdFromPath(path: []const u8, like: bool) ?u64 {
    if (!std.mem.startsWith(u8, path, "/posts/")) return null;
    var value = path[7..];
    if (like) {
        if (!std.mem.endsWith(u8, value, "/like")) return null;
        value = value[0 .. value.len - 5];
    }
    if (std.mem.indexOfScalar(u8, value, '/') != null) return null;
    return parsePositiveId(value);
}

fn base64UrlDecode(input: []const u8, out: []u8) !usize {
    var acc: u32 = 0;
    var bits: u8 = 0;
    var n: usize = 0;
    for (input) |ch| {
        const v: u8 = switch (ch) {
            'A'...'Z' => ch - 'A',
            'a'...'z' => ch - 'a' + 26,
            '0'...'9' => ch - '0' + 52,
            '-' => 62,
            '_' => 63,
            else => return error.InvalidBase64,
        };
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out.len) return error.BufferTooSmall;
            out[n] = @intCast((acc >> @as(u5, @intCast(bits))) & 0xff);
            n += 1;
        }
    }
    if (bits >= 6) return error.InvalidBase64;
    if (bits > 0 and (acc & ((@as(u32, 1) << @as(u5, @intCast(bits))) - 1)) != 0) return error.InvalidBase64;
    return n;
}

fn validJsonObject(raw: []const u8) bool {
    var p = JsonParser{ .data = raw };
    p.ws();
    if (p.pos >= raw.len or raw[p.pos] != '{') return false;
    p.skipValue(0) catch return false;
    p.ws();
    return p.pos == raw.len;
}

fn jsonStringField(raw: []const u8, key: []const u8, out: []u8) ?usize {
    var p = JsonParser{ .data = raw };
    p.ws();
    if (p.pos >= raw.len or raw[p.pos] != '{') return null;
    p.pos += 1;
    var key_buf: [128]u8 = undefined;
    p.ws();
    if (p.pos < raw.len and raw[p.pos] == '}') return null;
    while (p.pos < raw.len) {
        const n = p.parseString(&key_buf) catch return null;
        p.ws();
        if (p.pos >= raw.len or raw[p.pos] != ':') return null;
        p.pos += 1;
        p.ws();
        if (std.mem.eql(u8, key_buf[0..n], key)) return p.parseString(out) catch null;
        p.skipValue(0) catch return null;
        p.ws();
        if (p.pos >= raw.len) return null;
        if (raw[p.pos] == '}') break;
        if (raw[p.pos] != ',') return null;
        p.pos += 1;
        p.ws();
    }
    return null;
}

fn jsonNumberField(raw: []const u8, key: []const u8) ?i64 {
    var p = JsonParser{ .data = raw };
    p.ws();
    if (p.pos >= raw.len or raw[p.pos] != '{') return null;
    p.pos += 1;
    var key_buf: [128]u8 = undefined;
    p.ws();
    if (p.pos < raw.len and raw[p.pos] == '}') return null;
    while (p.pos < raw.len) {
        const n = p.parseString(&key_buf) catch return null;
        p.ws();
        if (p.pos >= raw.len or raw[p.pos] != ':') return null;
        p.pos += 1;
        p.ws();
        if (std.mem.eql(u8, key_buf[0..n], key)) {
            const start = p.pos;
            p.number() catch return null;
            return std.fmt.parseInt(i64, raw[start..p.pos], 10) catch null;
        }
        p.skipValue(0) catch return null;
        p.ws();
        if (p.pos >= raw.len) return null;
        if (raw[p.pos] == '}') break;
        if (raw[p.pos] != ',') return null;
        p.pos += 1;
        p.ws();
    }
    return null;
}

fn authenticate(app: *App, header: ?[]const u8, out_username: []u8) !Auth {
    const value = header orelse return error.MissingBearer;
    if (!std.mem.startsWith(u8, value, "Bearer ")) return error.MissingBearer;
    const token = value[7..];
    var dot1: usize = undefined;
    var dot2: usize = undefined;
    dot1 = std.mem.indexOfScalar(u8, token, '.') orelse return error.InvalidToken;
    dot2 = std.mem.indexOfScalarPos(u8, token, dot1 + 1, '.') orelse return error.InvalidToken;
    if (std.mem.indexOfScalarPos(u8, token, dot2 + 1, '.') != null or dot1 == 0 or dot2 == dot1 + 1 or dot2 + 1 >= token.len) return error.InvalidToken;
    const header_part = token[0..dot1];
    const payload_part = token[dot1 + 1 .. dot2];
    const sig_part = token[dot2 + 1 ..];
    var decoded_header: [1024]u8 = undefined;
    var decoded_payload: [8192]u8 = undefined;
    var decoded_sig: [64]u8 = undefined;
    const hn = base64UrlDecode(header_part, &decoded_header) catch return error.InvalidToken;
    const pn = base64UrlDecode(payload_part, &decoded_payload) catch return error.InvalidToken;
    const sn = base64UrlDecode(sig_part, &decoded_sig) catch return error.InvalidToken;
    if (sn != 32) return error.InvalidToken;
    if (!validJsonObject(decoded_header[0..hn]) or !validJsonObject(decoded_payload[0..pn])) return error.InvalidToken;
    var alg: [32]u8 = undefined;
    const alg_len = jsonStringField(decoded_header[0..hn], "alg", &alg) orelse return error.InvalidToken;
    if (!std.mem.eql(u8, alg[0..alg_len], "HS256")) return error.InvalidToken;
    var digest: [64]u8 = undefined;
    var digest_len: c_uint = 0;
    const signing = token[0..dot2];
    if (c.HMAC(c.EVP_sha256(), @ptrCast(app.jwt_secret.ptr), @intCast(app.jwt_secret.len), @ptrCast(signing.ptr), @intCast(signing.len), @ptrCast(&digest), &digest_len) == null or digest_len != 32) return error.InvalidToken;
    var diff: u8 = 0;
    for (decoded_sig[0..32], digest[0..32]) |a, b| diff |= a ^ b;
    if (diff != 0) return error.InvalidToken;
    var sub_buf: [128]u8 = undefined;
    const sub_len = jsonStringField(decoded_payload[0..pn], "sub", &sub_buf) orelse return error.InvalidPayload;
    const user_id = parsePositiveId(sub_buf[0..sub_len]) orelse return error.InvalidPayload;
    const username_len = jsonStringField(decoded_payload[0..pn], "username", out_username) orelse return error.InvalidPayload;
    const exp = jsonNumberField(decoded_payload[0..pn], "exp") orelse return error.InvalidToken;
    const now: i64 = std.time.timestamp();
    if (exp <= now) return error.InvalidToken;
    return .{ .user_id = user_id, .username = out_username[0..username_len] };
}

fn statusReason(status: u16) []const u8 {
    return switch (status) {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        500 => "Internal Server Error",
        503 => "Service Unavailable",
        else => "OK",
    };
}

fn appendTimestamp(out: *std.ArrayList(u8), raw: []const u8) !void {
    var text: [24]u8 = undefined;
    if (!formatTimestamp(&text, raw)) return error.InvalidTimestamp;
    try out.appendSlice(&text);
}

// A binary timestamptz (microseconds since 2000-01-01 00:00 UTC, big-endian)
// as ISO 8601 in UTC, truncated to milliseconds: 2026-11-18T17:23:45.123Z.
// False for anything that is not eight bytes or falls outside years 0..9999.
fn formatTimestamp(text: *[24]u8, raw: []const u8) bool {
    if (raw.len != 8) return false;
    const since = std.math.add(i64, std.mem.readInt(i64, raw[0..8], .big), SHIFT_US) catch return false;
    if (since < 0) return false;
    const millis = @as(u64, @intCast(since)) / 1000;
    const days = millis / 86_400_000;
    const of_day = millis - days * 86_400_000;
    // The civil date: 400-year eras of 146,097 days, and years that start in
    // March, so a leap day ends its year.
    const era = days / 146_097;
    const of_era = days - era * 146_097;
    const year_of_era = (of_era - of_era / 1460 + of_era / 36_524 - of_era / 146_096) / 365;
    const of_year = of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const shifted = (5 * of_year + 2) / 153;
    const date = of_year - (153 * shifted + 2) / 5 + 1;
    const month = if (shifted < 10) shifted + 3 else shifted - 9;
    const counted = year_of_era + era * 400 + @intFromBool(month <= 2);
    if (counted < 400 or counted > 400 + 9999) return false;
    const year = counted - 400;
    const ms = of_day % 1000;
    const secs = of_day / 1000;
    putPair(text[0..2], year / 100);
    putPair(text[2..4], year % 100);
    text[4] = '-';
    putPair(text[5..7], month);
    text[7] = '-';
    putPair(text[8..10], date);
    text[10] = 'T';
    putPair(text[11..13], secs / 3600);
    text[13] = ':';
    putPair(text[14..16], secs / 60 % 60);
    text[16] = ':';
    putPair(text[17..19], secs % 60);
    text[19] = '.';
    text[20] = @intCast('0' + ms / 100);
    putPair(text[21..23], ms % 100);
    text[23] = 'Z';
    return true;
}

// 2000-01-01 counted from 1 March of year -400 (astronomical), the start of
// a 400-year era: from there every timestamp of years 0..9999 is a positive
// count, so the arithmetic above is unsigned.
const SHIFT_US: i64 = (146_097 + 719_468 + 10_957) * 86_400_000_000;

// "00" to "99", so two digits cost one load.
const DIGIT_PAIRS: [200]u8 = blk: {
    var table: [200]u8 = undefined;
    for (0..100) |i| {
        table[2 * i] = @intCast('0' + i / 10);
        table[2 * i + 1] = @intCast('0' + i % 10);
    }
    break :blk table;
};

// value (0..99) as two decimal digits.
inline fn putPair(dst: *[2]u8, value: u64) void {
    dst.* = DIGIT_PAIRS[@intCast(value * 2)..][0..2].*;
}

fn setNonBlocking(fd: c_int) !void {
    const flags = c.fcntl(fd, c.F_GETFL, @as(c_int, 0));
    if (flags < 0 or c.fcntl(fd, c.F_SETFL, flags | c.O_NONBLOCK) < 0) return error.NonBlocking;
}

fn makeListener(host: []const u8, port: u16, allocator: Allocator) !c_int {
    var host_z = try allocator.alloc(u8, host.len + 1);
    defer allocator.free(host_z);
    std.mem.copyForwards(u8, host_z[0..host.len], host);
    host_z[host.len] = 0;
    var service: [8]u8 = undefined;
    const service_slice = try std.fmt.bufPrint(service[0..7], "{d}", .{port});
    service[service_slice.len] = 0;
    var hints: c.struct_addrinfo = std.mem.zeroes(c.struct_addrinfo);
    hints.ai_family = c.AF_UNSPEC;
    hints.ai_socktype = c.SOCK_STREAM;
    hints.ai_flags = c.AI_PASSIVE;
    var result: ?*c.struct_addrinfo = null;
    if (c.getaddrinfo(@ptrCast(host_z.ptr), @ptrCast(&service[0]), &hints, &result) != 0) return error.Address;
    defer if (result) |r| c.freeaddrinfo(r);
    var ai = result;
    while (ai) |entry| : (ai = entry.ai_next) {
        const fd = c.socket(entry.ai_family, entry.ai_socktype | c.SOCK_NONBLOCK | c.SOCK_CLOEXEC, entry.ai_protocol);
        if (fd < 0) continue;
        var yes: c_int = 1;
        _ = c.setsockopt(fd, c.SOL_SOCKET, c.SO_REUSEADDR, @ptrCast(&yes), @sizeOf(c_int));
        _ = c.setsockopt(fd, c.SOL_SOCKET, c.SO_REUSEPORT, @ptrCast(&yes), @sizeOf(c_int));
        // Accepted sockets inherit these, which saves two calls per accept.
        _ = c.setsockopt(fd, c.IPPROTO_TCP, c.TCP_NODELAY, @ptrCast(&yes), @sizeOf(c_int));
        _ = c.setsockopt(fd, c.SOL_SOCKET, c.SO_KEEPALIVE, @ptrCast(&yes), @sizeOf(c_int));
        // Keep the accept queue useful during the million-connection ramp.
        // The kernel may clamp this to net.core.somaxconn.
        const backlog: c_int = 262144;
        if (c.bind(fd, entry.ai_addr, entry.ai_addrlen) == 0 and c.listen(fd, backlog) == 0) return fd;
        _ = c.close(fd);
    }
    return error.Bind;
}

fn fatal(comptime message: []const u8) noreturn {
    std.debug.print("zig-epoll-kamkm: " ++ message ++ "\n", .{});
    std.process.exit(1);
}

fn monotonicMicros() i64 {
    var ts: c.struct_timespec = undefined;
    _ = c.clock_gettime(c.CLOCK_MONOTONIC, &ts);
    return @as(i64, ts.tv_sec) * 1_000_000 + @divTrunc(@as(i64, ts.tv_nsec), 1000);
}

fn put(out: *std.ArrayList(u8), bytes: []const u8) void {
    out.appendSlice(bytes) catch fatal("out of memory");
}

fn putInt(out: *std.ArrayList(u8), comptime T: type, value: T) void {
    var raw: [@sizeOf(T)]u8 = undefined;
    std.mem.writeInt(T, &raw, value, .big);
    put(out, &raw);
}

fn decimal(raw: *[20]u8, value: u64) []const u8 {
    return std.fmt.bufPrint(raw, "{d}", .{value}) catch unreachable;
}

fn jsonEscape(out: *std.ArrayList(u8), value: []const u8) void {
    var start: usize = 0;
    for (value, 0..) |ch, i| {
        if (ch >= 0x20 and ch != '"' and ch != '\\') continue;
        put(out, value[start..i]);
        start = i + 1;
        switch (ch) {
            '"' => put(out, "\\\""),
            '\\' => put(out, "\\\\"),
            8 => put(out, "\\b"),
            9 => put(out, "\\t"),
            10 => put(out, "\\n"),
            12 => put(out, "\\f"),
            13 => put(out, "\\r"),
            else => {
                const digits = "0123456789abcdef";
                put(out, "\\u00");
                put(out, &[_]u8{ digits[ch >> 4], digits[ch & 0x0f] });
            },
        }
    }
    put(out, value[start..]);
}

// ---------------------------------------------------------------------------
// PostgreSQL wire messages
//
// A batch is one write: a Bind and an Execute per statement, closed by one
// Sync. Nothing asks the server to describe a result, so no RowDescription
// comes back. Parameters are text and so are the results, except created_at,
// which comes as an eight-byte binary timestamp instead of 29 bytes of text;
// JSON is written straight from the DataRow bytes.

const SYNC = "S\x00\x00\x00\x04";

// Result format codes as Bind carries them: a count, then a code per column
// (0 text, 1 binary). Post rows are id, body, created_at, username and
// like_count; a new post returns its id and created_at.
const POST_FORMATS = "\x00\x05\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00";
const INSERT_FORMATS = "\x00\x02\x00\x00\x00\x01";
const TEXT_FORMATS = "\x00\x00";

fn appendBind(out: *std.ArrayList(u8), statement: []const u8, parameters: []const []const u8, formats: []const u8) void {
    const start = out.items.len;
    put(out, "B\x00\x00\x00\x00\x00"); // type, length (set below), unnamed portal
    put(out, statement);
    put(out, "\x00\x00\x00"); // end of the name; no parameter formats: all text
    putInt(out, u16, @intCast(parameters.len));
    for (parameters) |parameter| {
        putInt(out, u32, @intCast(parameter.len));
        put(out, parameter);
    }
    put(out, formats);
    std.mem.writeInt(u32, out.items[start + 1 ..][0..4], @intCast(out.items.len - start - 1), .big);
    put(out, "E\x00\x00\x00\x09\x00\x00\x00\x00\x00"); // Execute the unnamed portal, no row limit
}

fn appendQuery(out: *std.ArrayList(u8), job: Job) void {
    var first: [20]u8 = undefined;
    var second: [20]u8 = undefined;
    switch (job.kind) {
        .feed => appendBind(out, "feed", &[_][]const u8{}, POST_FORMATS),
        .health => appendBind(out, "health", &[_][]const u8{}, TEXT_FORMATS),
        .post => appendBind(out, "post", &[_][]const u8{decimal(&first, job.id)}, POST_FORMATS),
        .create => appendBind(out, "insert_post", &[_][]const u8{ decimal(&first, job.user), job.text.?[0..job.body_len] }, INSERT_FORMATS),
        .like => appendBind(out, "like_post", &[_][]const u8{ decimal(&first, job.user), decimal(&second, job.id) }, TEXT_FORMATS),
    }
}

// Splits the next column off a DataRow. NULL is null.
fn column(row: *[]const u8) ?[]const u8 {
    const rest = row.*;
    if (rest.len < 4) return null;
    const size = std.mem.readInt(i32, rest[0..4], .big);
    if (size < 0) return null;
    const length: usize = @intCast(size);
    if (length > rest.len - 4) return null;
    row.* = rest[4 + length ..];
    return rest[4 .. 4 + length];
}

fn isDecimal(value: []const u8) bool {
    if (value.len == 0 or value.len > 20) return false;
    for (value) |ch| if (ch < '0' or ch > '9') return false;
    return true;
}

// id, body, created_at, username, like_count as one JSON object.
fn appendPostRow(out: *std.ArrayList(u8), message: []const u8) bool {
    if (message.len < 2 or std.mem.readInt(u16, message[0..2], .big) != 5) return false;
    var row = message[2..];
    const id = column(&row) orelse return false;
    const text = column(&row) orelse return false;
    const created = column(&row) orelse return false;
    const author = column(&row) orelse return false;
    const likes = column(&row) orelse return false;
    if (!isDecimal(id) or !isDecimal(likes)) return false;
    put(out, "{\"id\":");
    put(out, id);
    put(out, ",\"body\":\"");
    jsonEscape(out, text);
    put(out, "\",\"created_at\":\"");
    appendTimestamp(out, created) catch return false;
    put(out, "\",\"author\":\"");
    jsonEscape(out, author);
    put(out, "\",\"like_count\":");
    put(out, likes);
    put(out, "}");
    return true;
}

// True when an ErrorResponse says the liked post does not exist.
fn missingPost(body: []const u8) bool {
    var code: []const u8 = "";
    var constraint: []const u8 = "";
    var rest = body;
    while (rest.len > 1) {
        const field = rest[0];
        const end = std.mem.indexOfScalar(u8, rest[1..], 0) orelse break;
        const value = rest[1 .. 1 + end];
        rest = rest[end + 2 ..];
        if (field == 'C') code = value else if (field == 'n') constraint = value;
    }
    return std.mem.eql(u8, code, "23503") and std.mem.endsWith(u8, constraint, "likes_post_id_fkey");
}

// True for a FATAL or PANIC ErrorResponse: the server closes the connection
// after sending it. 'V' is the severity in English ('S' may be translated).
fn endsSession(body: []const u8) bool {
    var rest = body;
    while (rest.len > 1) {
        const field = rest[0];
        const end = std.mem.indexOfScalar(u8, rest[1..], 0) orelse break;
        const value = rest[1 .. 1 + end];
        rest = rest[end + 2 ..];
        if (field == 'V') return std.mem.eql(u8, value, "FATAL") or std.mem.eql(u8, value, "PANIC");
    }
    return false;
}

// Runs on a helper thread: everything here blocks.
fn connectDatabase(app: *App) ?*c.PGconn {
    const keywords = [_][*c]const u8{ "connect_timeout", "dbname", null };
    const values = [_][*c]const u8{ "5", app.db_url.ptr, null };
    const pg = c.PQconnectdbParams(&keywords, &values, 1) orelse return null;
    if (c.PQstatus(pg) != c.CONNECTION_OK) {
        c.PQfinish(pg);
        return null;
    }
    const settings = c.PQexec(pg, "SET TIME ZONE 'UTC'; SET jit = off; SET plan_cache_mode = force_generic_plan");
    const configured = settings != null and c.PQresultStatus(settings) == c.PGRES_COMMAND_OK;
    if (settings) |result| c.PQclear(result);
    if (!configured) {
        c.PQfinish(pg);
        return null;
    }
    const statements = [_]struct { name: [*:0]const u8, sql: [*:0]const u8 }{
        .{ .name = "feed", .sql = DB_FEED_SQL },
        .{ .name = "post", .sql = DB_POST_SQL },
        .{ .name = "insert_post", .sql = DB_INSERT_SQL },
        .{ .name = "like_post", .sql = DB_LIKE_SQL },
        .{ .name = "health", .sql = "SELECT 1" },
    };
    for (statements) |statement| {
        const result = c.PQprepare(pg, statement.name, statement.sql, 0, null);
        const prepared = result != null and c.PQresultStatus(result) == c.PGRES_COMMAND_OK;
        if (result) |r| c.PQclear(r);
        if (!prepared) {
            c.PQfinish(pg);
            return null;
        }
    }
    return pg;
}

// ---------------------------------------------------------------------------
// Clients, jobs and sessions

const Client = struct {
    fd: c_int,
    // A request of this client is queued or with the database.
    busy: bool = false,
    // The socket may hold input that has not been read (edge-triggered).
    readable: bool = false,
    // The peer has closed its side; read to the end.
    hangup: bool = false,
    closed: bool = false,
    continued: bool = false,
    close_after: bool = false,
    keep_alive_1_0: bool = false,
    gzip: bool = false,
    interim: bool = false,
    // Input kept across reads: a partial request, or requests sent ahead.
    input: ?[]u8 = null,
    input_len: usize = 0,
    // The rest of a response the socket did not take.
    output: ?[]u8 = null,
    output_pos: usize = 0,
    next: ?*Client = null,
};

const JobKind = enum(u8) { create, like, feed, post, health };

const Job = struct {
    client: *Client,
    kind: JobKind,
    retried: bool = false,
    id: u64 = 0,
    user: u64 = 0,
    // A new post: its body, then the author's name.
    text: ?[]u8 = null,
    body_len: usize = 0,
};

// Creates first, then likes in key order: every group takes its like rows in
// the same order, so two groups can wait for each other but never in a cycle.
fn writeOrder(_: void, a: Job, b: Job) bool {
    if (a.kind != b.kind) return @intFromEnum(a.kind) < @intFromEnum(b.kind);
    if (a.kind != .like) return false;
    if (a.user != b.user) return a.user < b.user;
    return a.id < b.id;
}

const JobQueue = struct {
    items: std.ArrayList(Job),
    head: usize = 0,

    fn len(self: *const JobQueue) usize {
        return self.items.items.len - self.head;
    }

    fn push(self: *JobQueue, job: Job) bool {
        self.items.append(job) catch return false;
        return true;
    }

    fn take(self: *JobQueue, out: []Job) usize {
        const count = @min(out.len, self.len());
        @memcpy(out[0..count], self.items.items[self.head .. self.head + count]);
        self.head += count;
        if (self.head == self.items.items.len) {
            self.items.clearRetainingCapacity();
            self.head = 0;
        }
        return count;
    }

    fn unshift(self: *JobQueue, jobs: []const Job) bool {
        if (self.head >= jobs.len) {
            self.head -= jobs.len;
            @memcpy(self.items.items[self.head .. self.head + jobs.len], jobs);
            return true;
        }
        self.items.insertSlice(self.head, jobs) catch return false;
        return true;
    }
};

const WriteResult = struct {
    done: bool = false,
    not_found: bool = false,
    inserted: bool = false,
    id_len: u8 = 0,
    created_len: u8 = 0,
    id: [20]u8 = undefined,
    created: [40]u8 = undefined,
};

const Mode = enum { reads, group, alone };

const Session = struct {
    index: usize,
    reads: bool,
    writes: bool,
    pg: ?*c.PGconn = null,
    fd: c_int = -1,
    dialing: bool = false,
    retry_at: i64 = 0,
    // A batch is out and its replies are not complete.
    busy: bool = false,
    mode: Mode = .reads,
    sent_at: i64 = 0,
    last_send: i64 = 0,
    out: std.ArrayList(u8),
    out_pos: usize = 0,
    flushing: bool = false,
    incoming: []u8,
    r: usize = 0,
    w: usize = 0,
    // The jobs of the batch that is out. This and results live outside the
    // record so that the fields every pass reads stay a few cache lines apart.
    batch: []Job,
    count: usize = 0,
    // Jobs before this one have been answered.
    next: usize = 0,
    // Reads: the JSON of the read being received.
    body: std.ArrayList(u8),
    rows: usize = 0,
    bad_row: bool = false,
    skipping: bool = false,
    // Writes: the statement whose reply is being received, and what came back.
    cursor: usize = 0,
    failed: bool = false,
    results: []WriteResult,
};

const Dialed = struct { index: usize, pg: ?*c.PGconn };

threadlocal var gzip_buffer: [256 * 1024]u8 = undefined;
threadlocal var gzip_stream: c.struct_isal_zstream = undefined;

// ---------------------------------------------------------------------------
// The loop

const Loop = struct {
    app: *App,
    allocator: Allocator,
    epfd: c_int,
    wake_fd: c_int,
    now: i64 = 0,
    // The client whose buffered input serve() is working through.
    serving: ?*Client = null,
    // Closed clients, freed at the end of the pass so that nothing in the pass
    // can hold a pointer to freed memory.
    graves: ?*Client = null,
    wrote_last: bool = false,
    sessions: []Session,
    reads: JobQueue,
    writes: JobQueue,
    input: []u8,
    wire: std.ArrayList(u8),
    body: std.ArrayList(u8),
    inbox_mutex: std.Thread.Mutex = .{},
    inbox: std.ArrayList(Dialed),

    fn create(app: *App, host: []const u8, ports: []const u16) !*Loop {
        const allocator = app.allocator;
        const epfd = c.epoll_create1(c.EPOLL_CLOEXEC);
        if (epfd < 0) return error.Epoll;
        const wake_fd = c.eventfd(0, c.EFD_NONBLOCK | c.EFD_CLOEXEC);
        if (wake_fd < 0) return error.Event;
        const plan = app.plan;
        const sessions = try allocator.alloc(Session, plan.readers + plan.writers);
        for (sessions, 0..) |*s, i| {
            s.* = .{
                .index = i,
                .reads = i < plan.readers,
                .writes = plan.writers == 0 or i >= plan.readers,
                .out = std.ArrayList(u8).init(allocator),
                .incoming = try allocator.alloc(u8, WIRE_BUFFER),
                .batch = try allocator.alloc(Job, MAX_BATCH),
                .results = try allocator.alloc(WriteResult, WRITE_GROUP),
                .body = std.ArrayList(u8).init(allocator),
            };
        }
        const l = try allocator.create(Loop);
        l.* = .{
            .app = app,
            .allocator = allocator,
            .epfd = epfd,
            .wake_fd = wake_fd,
            .sessions = sessions,
            .reads = .{ .items = std.ArrayList(Job).init(allocator) },
            .writes = .{ .items = std.ArrayList(Job).init(allocator) },
            .input = try allocator.alloc(u8, READ_CHUNK),
            .wire = std.ArrayList(u8).init(allocator),
            .body = std.ArrayList(u8).init(allocator),
            .inbox = try std.ArrayList(Dialed).initCapacity(allocator, sessions.len),
        };
        if (!l.watch(c.EPOLL_CTL_ADD, wake_fd, EV_IN, TAG_WAKE)) return error.Epoll;
        for (ports) |port| {
            const fd = try makeListener(host, port, allocator);
            const tag = (@as(u64, @intCast(fd)) << 3) | TAG_LISTENER;
            if (!l.watch(c.EPOLL_CTL_ADD, fd, EV_IN, tag)) return error.Epoll;
        }
        return l;
    }

    fn watch(l: *Loop, op: c_int, fd: c_int, events: u32, tag: u64) bool {
        var ev: c.struct_epoll_event = undefined;
        ev.events = events;
        ev.data.u64 = tag;
        return c.epoll_ctl(l.epfd, op, fd, &ev) == 0;
    }

    fn run(l: *Loop) void {
        var events: [EVENT_BATCH]c.struct_epoll_event = undefined;
        var later: [EVENT_BATCH]u64 = undefined;
        l.now = monotonicMicros();
        for (l.sessions) |*s| l.dial(s);
        while (true) {
            const n = c.epoll_wait(l.epfd, &events, EVENT_BATCH, l.timeout());
            l.now = monotonicMicros();
            if (n < 0) {
                if (c.__errno_location().* == c.EINTR) continue;
                fatal("epoll_wait failed");
            }
            // Clients first, sessions after: a reply can close a client, and
            // by then that client's own event has been handled.
            var held: usize = 0;
            for (events[0..@intCast(n)]) |ev| {
                const tag = ev.data.u64;
                switch (tag & 7) {
                    TAG_CLIENT => l.clientEvent(@ptrFromInt(@as(usize, @intCast(tag))), ev.events),
                    TAG_LISTENER => l.accept(@intCast(tag >> 3)),
                    else => {
                        later[held] = tag;
                        held += 1;
                    },
                }
            }
            for (later[0..held]) |tag| {
                if (tag & 7 == TAG_WAKE) l.wakeEvent() else l.sessionEvent(&l.sessions[@intCast(tag >> 3)]);
            }
            l.timers();
            l.pump();
            while (l.graves) |cl| {
                l.graves = cl.next;
                l.allocator.destroy(cl);
            }
        }
    }

    // How long epoll may sleep: until a gather window ends, a redial is due or
    // a batch has been out too long.
    fn timeout(l: *Loop) c_int {
        const plan = l.app.plan;
        var wait: i64 = 1_000_000;
        for (l.sessions) |*s| {
            if (s.fd < 0) {
                if (!s.dialing) wait = @min(wait, s.retry_at - l.now);
            } else if (s.busy) {
                wait = @min(wait, s.sent_at + DATABASE_TIMEOUT_US - l.now);
            } else {
                if (s.reads and l.reads.len() > 0) wait = @min(wait, s.last_send + plan.read_window - l.now);
                if (s.writes and l.writes.len() > 0) wait = @min(wait, s.last_send + plan.write_window - l.now);
            }
        }
        if (wait <= 0) return 0;
        return @intCast(@divTrunc(wait + 999, 1000));
    }

    fn timers(l: *Loop) void {
        for (l.sessions) |*s| {
            if (s.fd < 0) {
                if (!s.dialing and l.now >= s.retry_at) l.dial(s);
            } else if (s.busy and l.now - s.sent_at >= DATABASE_TIMEOUT_US) {
                l.lost(s);
            }
        }
    }

    // ---- clients ----

    fn accept(l: *Loop, listener: c_int) void {
        var burst: usize = 0;
        while (burst < ACCEPT_BURST) : (burst += 1) {
            const fd = accept4(listener, null, null, c.SOCK_NONBLOCK | c.SOCK_CLOEXEC);
            if (fd < 0) return;
            const cl = l.allocator.create(Client) catch {
                _ = c.close(fd);
                continue;
            };
            cl.* = .{ .fd = fd };
            if (!l.watch(c.EPOLL_CTL_ADD, fd, EV_IN | EV_RDHUP | EV_ET, @intFromPtr(cl))) {
                _ = c.close(fd);
                l.allocator.destroy(cl);
            }
        }
    }

    fn closeClient(l: *Loop, cl: *Client) void {
        if (cl.closed) return;
        cl.closed = true;
        // Closing the descriptor also takes it out of the epoll set.
        _ = c.close(cl.fd);
        if (cl.input) |buffer| l.allocator.free(buffer);
        if (cl.output) |buffer| l.allocator.free(buffer);
        cl.input = null;
        cl.input_len = 0;
        cl.output = null;
        // A busy client is still named by a job; the job's answer buries it.
        if (!cl.busy) l.bury(cl);
    }

    fn bury(l: *Loop, cl: *Client) void {
        cl.next = l.graves;
        l.graves = cl;
    }

    fn clientEvent(l: *Loop, cl: *Client, events: u32) void {
        if (cl.closed) return;
        if (events & (EV_ERR | EV_HUP) != 0) return l.closeClient(cl);
        if (events & EV_OUT != 0 and cl.output != null) {
            l.flushClient(cl);
            if (cl.closed) return;
        }
        if (events & (EV_IN | EV_RDHUP) != 0) {
            cl.readable = true;
            if (events & EV_RDHUP != 0) cl.hangup = true;
            l.readClient(cl);
        }
    }

    // Reads while the client is idle. A client waiting for the database is not
    // read: what it sends meanwhile stays in the socket and is served, in
    // order, after the response.
    fn readClient(l: *Loop, cl: *Client) void {
        while (!cl.closed and !cl.busy and cl.output == null and cl.readable) {
            var target = l.input;
            if (cl.input_len > 0) {
                var held = cl.input.?;
                if (cl.input_len == held.len) {
                    if (held.len >= READ_CHUNK) return l.closeClient(cl);
                    held = l.allocator.realloc(held, held.len * 2) catch return l.closeClient(cl);
                    cl.input = held;
                }
                target = held[cl.input_len..];
            }
            const n = c.read(cl.fd, @ptrCast(target.ptr), target.len);
            if (n < 0) {
                const code = c.__errno_location().*;
                if (code == c.EINTR) continue;
                if (code == c.EAGAIN) {
                    cl.readable = false;
                    return;
                }
                return l.closeClient(cl);
            }
            if (n == 0) return l.closeClient(cl);
            const got: usize = @intCast(n);
            // A short read emptied the socket; whatever arrives later raises a
            // new event. After a hangup, keep reading to the end.
            if (got < target.len and !cl.hangup) cl.readable = false;
            if (cl.input_len > 0) {
                cl.input_len += got;
                l.serve(cl, cl.input.?[0..cl.input_len]);
            } else {
                l.serve(cl, target[0..got]);
            }
        }
    }

    // Handles the complete requests in data, one at a time, and keeps the rest.
    fn serve(l: *Loop, cl: *Client, data: []const u8) void {
        l.serving = cl;
        defer l.serving = null;
        var rest = data;
        while (rest.len > 0 and !cl.busy and cl.output == null) {
            const head = (http.parseHead(rest) catch |err| return l.reject(cl, err)) orelse {
                if (rest.len >= MAX_REQUEST) return l.closeClient(cl);
                break;
            };
            const needed = http.messageLength(head, rest) catch |err| return l.reject(cl, err);
            const used = needed orelse rest.len + 1;
            if (used > rest.len) {
                if (rest.len >= MAX_REQUEST) return l.closeClient(cl);
                if (head.expect_continue and !cl.continued) {
                    cl.continued = true;
                    l.send(cl, "HTTP/1.1 100 Continue\r\n\r\n", true);
                    if (cl.closed) return;
                }
                break;
            }
            cl.continued = false;
            cl.close_after = head.close_after;
            cl.keep_alive_1_0 = head.keep_alive_1_0;
            cl.gzip = head.accept_gzip;
            l.handle(cl, rest[0..used], head);
            if (cl.closed) return;
            rest = rest[used..];
        }
        l.stash(cl, rest);
    }

    // Keeps unserved input with the client. rest is either part of the
    // client's own buffer or of the loop's read buffer.
    fn stash(l: *Loop, cl: *Client, rest: []const u8) void {
        if (cl.closed) return;
        if (rest.len == 0) {
            if (cl.input) |buffer| l.allocator.free(buffer);
            cl.input = null;
            cl.input_len = 0;
            return;
        }
        if (cl.input) |buffer| {
            if (buffer.len >= rest.len) {
                std.mem.copyForwards(u8, buffer[0..rest.len], rest);
                cl.input_len = rest.len;
                return;
            }
            l.allocator.free(buffer);
            cl.input = null;
            cl.input_len = 0;
        }
        const size = @max(512, std.math.ceilPowerOfTwo(usize, rest.len) catch return l.closeClient(cl));
        const buffer = l.allocator.alloc(u8, size) catch return l.closeClient(cl);
        @memcpy(buffer[0..rest.len], rest);
        cl.input = buffer;
        cl.input_len = rest.len;
    }

    fn reject(l: *Loop, cl: *Client, err: http.ParseError) void {
        if (err == error.RequestTooLarge) return l.closeClient(cl);
        cl.close_after = true;
        cl.gzip = false;
        l.fail(cl, 400, "malformed JSON body");
    }

    fn handle(l: *Loop, cl: *Client, raw: []const u8, head: http.Head) void {
        switch (routeFor(head.method, head.path)) {
            .unknown => l.fail(cl, 404, "not found"),
            .health => l.enqueue(&l.reads, .{ .client = cl, .kind = .health }),
            .feed => l.enqueue(&l.reads, .{ .client = cl, .kind = .feed }),
            .post => {
                const id = postIdFromPath(head.path, false) orelse return l.fail(cl, 400, "invalid post id");
                l.enqueue(&l.reads, .{ .client = cl, .kind = .post, .id = id });
            },
            .like => {
                var name: [4096]u8 = undefined;
                const auth = authenticate(l.app, head.authorization, &name) catch |err| return l.unauthorized(cl, err);
                const id = postIdFromPath(head.path, true) orelse return l.fail(cl, 400, "invalid post id");
                l.enqueue(&l.writes, .{ .client = cl, .kind = .like, .id = id, .user = auth.user_id });
            },
            .create => {
                var name: [4096]u8 = undefined;
                const auth = authenticate(l.app, head.authorization, &name) catch |err| return l.unauthorized(cl, err);
                var body = raw[head.header_len..];
                if (head.chunked) {
                    const request = parseHttp(raw) orelse return l.fail(cl, 400, "malformed JSON body");
                    body = request.body;
                }
                var text: [MAX_REQUEST + 1]u8 = undefined;
                var parser = JsonParser{ .data = body };
                switch (parser.parseBody(&text)) {
                    .ok => {},
                    .malformed => return l.fail(cl, 400, "malformed JSON body"),
                    .required => return l.fail(cl, 400, "body is required"),
                    .too_long => return l.fail(cl, 400, "body must be at most 500 characters"),
                }
                const post = text[0..parser.parsed_len];
                // A text parameter cannot carry a NUL; never store a truncated body.
                if (std.mem.indexOfScalar(u8, post, 0) != null) return l.fail(cl, 500, "internal server error");
                const kept = l.allocator.alloc(u8, post.len + auth.username.len) catch return l.fail(cl, 500, "internal server error");
                @memcpy(kept[0..post.len], post);
                @memcpy(kept[post.len..], auth.username);
                l.enqueue(&l.writes, .{ .client = cl, .kind = .create, .user = auth.user_id, .text = kept, .body_len = post.len });
            },
        }
    }

    fn unauthorized(l: *Loop, cl: *Client, err: anyerror) void {
        l.fail(cl, 401, switch (err) {
            error.MissingBearer => "missing bearer token",
            error.InvalidPayload => "invalid token payload",
            else => "invalid or expired token",
        });
    }

    fn enqueue(l: *Loop, queue: *JobQueue, job: Job) void {
        job.client.busy = true;
        if (!queue.push(job)) l.failJob(job);
    }

    fn fail(l: *Loop, cl: *Client, status: u16, message: []const u8) void {
        l.body.clearRetainingCapacity();
        put(&l.body, "{\"error\":\"");
        jsonEscape(&l.body, message);
        put(&l.body, "\"}");
        l.respond(cl, status, l.body.items);
    }

    fn respond(l: *Loop, cl: *Client, status: u16, body: []const u8) void {
        var payload = body;
        var compressed = false;
        if (cl.gzip and body.len >= 1024 and body.len < gzip_buffer.len - 32) {
            c.isal_deflate_stateless_init(&gzip_stream);
            gzip_stream.next_in = @constCast(body.ptr);
            gzip_stream.avail_in = @intCast(body.len);
            gzip_stream.next_out = &gzip_buffer;
            gzip_stream.avail_out = gzip_buffer.len;
            gzip_stream.gzip_flag = c.IGZIP_GZIP;
            gzip_stream.level = 0;
            if (c.isal_deflate_stateless(&gzip_stream) == c.COMP_OK) {
                compressed = true;
                payload = gzip_buffer[0 .. gzip_buffer.len - gzip_stream.avail_out];
            }
        }
        const out = &l.wire;
        out.clearRetainingCapacity();
        // HTTP/1.1 keeps the connection open unless a side says otherwise, so
        // the header goes out only to close it or for HTTP/1.0 keep-alive.
        out.writer().print("HTTP/1.1 {d} {s}\r\nContent-Type: application/json\r\n{s}Content-Length: {d}\r\n{s}\r\n", .{
            status,
            statusReason(status),
            if (compressed) "Content-Encoding: gzip\r\n" else "",
            payload.len,
            if (cl.close_after) "Connection: close\r\n" else if (cl.keep_alive_1_0) "Connection: keep-alive\r\n" else "",
        }) catch fatal("out of memory");
        put(out, payload);
        l.send(cl, out.items, false);
    }

    // Writes bytes now; what the socket does not take is kept and sent when
    // it is writable again.
    fn send(l: *Loop, cl: *Client, bytes: []const u8, interim: bool) void {
        var sent: usize = 0;
        while (sent < bytes.len) {
            const n = c.send(cl.fd, @ptrCast(bytes.ptr + sent), bytes.len - sent, c.MSG_NOSIGNAL);
            if (n > 0) {
                sent += @intCast(n);
                continue;
            }
            const code = c.__errno_location().*;
            if (n < 0 and code == c.EINTR) continue;
            if (n < 0 and code == c.EAGAIN) {
                cl.output = l.allocator.dupe(u8, bytes[sent..]) catch return l.closeClient(cl);
                cl.output_pos = 0;
                cl.interim = interim;
                if (!l.watch(c.EPOLL_CTL_MOD, cl.fd, EV_IN | EV_OUT | EV_RDHUP | EV_ET, @intFromPtr(cl))) l.closeClient(cl);
                return;
            }
            return l.closeClient(cl);
        }
        if (!interim) l.finished(cl);
    }

    fn flushClient(l: *Loop, cl: *Client) void {
        const bytes = cl.output orelse return;
        while (cl.output_pos < bytes.len) {
            const n = c.send(cl.fd, @ptrCast(bytes.ptr + cl.output_pos), bytes.len - cl.output_pos, c.MSG_NOSIGNAL);
            if (n > 0) {
                cl.output_pos += @intCast(n);
                continue;
            }
            const code = c.__errno_location().*;
            if (n < 0 and code == c.EINTR) continue;
            if (n < 0 and code == c.EAGAIN) {
                // Ask again: the modification reports the socket at once if it
                // is in fact writable.
                if (!l.watch(c.EPOLL_CTL_MOD, cl.fd, EV_IN | EV_OUT | EV_RDHUP | EV_ET, @intFromPtr(cl))) l.closeClient(cl);
                return;
            }
            return l.closeClient(cl);
        }
        l.allocator.free(bytes);
        cl.output = null;
        cl.output_pos = 0;
        if (cl.interim) {
            cl.interim = false;
            l.resumeClient(cl);
        } else {
            l.finished(cl);
        }
    }

    // A complete response has been written.
    fn finished(l: *Loop, cl: *Client) void {
        if (cl.close_after) return l.closeClient(cl);
        // Inside serve() the caller goes on with the input it holds.
        if (l.serving != cl) l.resumeClient(cl);
    }

    fn resumeClient(l: *Loop, cl: *Client) void {
        if (cl.input_len > 0) l.serve(cl, cl.input.?[0..cl.input_len]);
        l.readClient(cl);
    }

    // ---- answers ----

    fn answer(l: *Loop, job: Job, status: u16, body: []const u8) void {
        const cl = job.client;
        cl.busy = false;
        if (cl.closed) l.bury(cl) else l.respond(cl, status, body);
        if (job.text) |text| l.allocator.free(text);
    }

    fn answerError(l: *Loop, job: Job, status: u16, message: []const u8) void {
        l.body.clearRetainingCapacity();
        put(&l.body, "{\"error\":\"");
        jsonEscape(&l.body, message);
        put(&l.body, "\"}");
        l.answer(job, status, l.body.items);
    }

    fn failJob(l: *Loop, job: Job) void {
        if (job.kind == .health) {
            l.answer(job, 503, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}");
        } else {
            l.answerError(job, 500, "internal server error");
        }
    }

    fn answerWrite(l: *Loop, job: Job, result: WriteResult) void {
        if (!result.done) {
            if (result.not_found) l.answerError(job, 404, "post not found") else l.failJob(job);
            return;
        }
        const out = &l.body;
        out.clearRetainingCapacity();
        if (job.kind == .like) {
            var digits: [20]u8 = undefined;
            put(out, "{\"liked\":true,\"already_liked\":");
            put(out, if (result.inserted) "false" else "true");
            put(out, ",\"post_id\":");
            put(out, decimal(&digits, job.id));
            put(out, "}");
            return l.answer(job, if (result.inserted) 201 else 200, out.items);
        }
        const text = job.text orelse return l.failJob(job);
        if (result.id_len == 0) return l.failJob(job);
        put(out, "{\"post\":{\"id\":");
        put(out, result.id[0..result.id_len]);
        put(out, ",\"body\":\"");
        jsonEscape(out, text[0..job.body_len]);
        put(out, "\",\"created_at\":\"");
        appendTimestamp(out, result.created[0..result.created_len]) catch return l.failJob(job);
        put(out, "\",\"author\":\"");
        jsonEscape(out, text[job.body_len..]);
        put(out, "\",\"like_count\":0}}");
        l.answer(job, 201, out.items);
    }

    // ---- PostgreSQL sessions ----

    fn dial(l: *Loop, s: *Session) void {
        s.dialing = true;
        const thread = std.Thread.spawn(.{}, dialMain, .{ l, s.index }) catch {
            s.dialing = false;
            s.retry_at = l.now + REDIAL_US;
            return;
        };
        thread.detach();
    }

    fn dialMain(l: *Loop, index: usize) void {
        const pg = connectDatabase(l.app);
        l.inbox_mutex.lock();
        l.inbox.appendAssumeCapacity(.{ .index = index, .pg = pg });
        l.inbox_mutex.unlock();
        var one: u64 = 1;
        _ = c.write(l.wake_fd, @ptrCast(&one), @sizeOf(u64));
    }

    fn wakeEvent(l: *Loop) void {
        var count: u64 = 0;
        _ = c.read(l.wake_fd, @ptrCast(&count), @sizeOf(u64));
        while (true) {
            l.inbox_mutex.lock();
            const item = l.inbox.popOrNull();
            l.inbox_mutex.unlock();
            const dialed = item orelse break;
            const s = &l.sessions[dialed.index];
            s.dialing = false;
            const pg = dialed.pg orelse {
                s.retry_at = l.now + REDIAL_US;
                continue;
            };
            const fd = c.PQsocket(pg);
            const tag = (@as(u64, s.index) << 3) | TAG_SESSION;
            if (fd < 0 or !l.watch(c.EPOLL_CTL_ADD, fd, EV_IN | EV_OUT | EV_RDHUP | EV_ET, tag)) {
                c.PQfinish(pg);
                s.retry_at = l.now + REDIAL_US;
                continue;
            }
            setNonBlocking(fd) catch {};
            s.pg = pg;
            s.fd = fd;
            s.busy = false;
            s.flushing = false;
            s.count = 0;
            s.r = 0;
            s.w = 0;
            s.last_send = 0;
        }
    }

    // The session is gone. Reads it had not answered run once more on another
    // session; the outcome of its writes is unknown, so they fail and are
    // never sent again.
    fn lost(l: *Loop, s: *Session) void {
        if (s.fd < 0) return;
        _ = c.epoll_ctl(l.epfd, c.EPOLL_CTL_DEL, s.fd, null);
        c.PQfinish(s.pg);
        s.pg = null;
        s.fd = -1;
        const pending = if (s.busy) s.batch[s.next..s.count] else s.batch[0..0];
        const reads = s.mode == .reads;
        s.busy = false;
        s.flushing = false;
        s.count = 0;
        s.r = 0;
        s.w = 0;
        s.rows = 0;
        s.bad_row = false;
        s.out.clearRetainingCapacity();
        s.body.clearRetainingCapacity();
        // Dial before failing anything, so that work queued meanwhile waits
        // for the new session.
        l.dial(s);
        for (pending) |*job| {
            if (reads and !job.retried) {
                job.retried = true;
                if (l.reads.push(job.*)) continue;
            }
            l.failJob(job.*);
        }
    }

    fn pump(l: *Loop) void {
        if (l.app.plan.writers > 0) {
            _ = l.dispatch(&l.reads, false);
            _ = l.dispatch(&l.writes, true);
        } else if (l.wrote_last) {
            // Shared sessions: reads and writes take turns.
            if (l.dispatch(&l.reads, false)) l.wrote_last = false;
            if (l.dispatch(&l.writes, true)) l.wrote_last = true;
        } else {
            if (l.dispatch(&l.writes, true)) l.wrote_last = true;
            if (l.dispatch(&l.reads, false)) l.wrote_last = false;
        }
    }

    // Hands queued work to the sessions that may send now. A session starts
    // batches no closer together than its gather window; an idle one sends at
    // once.
    fn dispatch(l: *Loop, queue: *JobQueue, writes: bool) bool {
        if (queue.len() == 0) return false;
        const window = if (writes) l.app.plan.write_window else l.app.plan.read_window;
        var ready: usize = 0;
        var alive: usize = 0;
        for (l.sessions) |*s| {
            if (!(if (writes) s.writes else s.reads)) continue;
            if (s.fd >= 0 or s.dialing) alive += 1;
            if (s.fd >= 0 and !s.busy and l.now >= s.last_send + window) ready += 1;
        }
        if (alive == 0) {
            var one: [1]Job = undefined;
            while (queue.take(&one) == 1) l.failJob(one[0]);
            return false;
        }
        if (ready == 0) return false;
        const limit: usize = if (writes) WRITE_GROUP else l.app.plan.batch;
        for (l.sessions) |*s| {
            if (queue.len() == 0 or ready == 0) break;
            if (!(if (writes) s.writes else s.reads)) continue;
            if (s.fd < 0 or s.busy or l.now < s.last_send + window) continue;
            const share = @min(limit, (queue.len() + ready - 1) / ready);
            ready -= 1;
            s.count = queue.take(s.batch[0..share]);
            if (writes) l.sendGroup(s) else l.sendReads(s);
        }
        return true;
    }

    fn sendReads(l: *Loop, s: *Session) void {
        s.out.clearRetainingCapacity();
        for (s.batch[0..s.count]) |job| appendQuery(&s.out, job);
        put(&s.out, SYNC);
        s.mode = .reads;
        l.transmit(s);
    }

    // One Sync for the group: PostgreSQL runs it as one transaction and sends
    // ReadyForQuery only after the commit is durable.
    fn sendGroup(l: *Loop, s: *Session) void {
        const jobs = s.batch[0..s.count];
        std.mem.sort(Job, jobs, {}, writeOrder);
        s.out.clearRetainingCapacity();
        for (jobs) |job| appendQuery(&s.out, job);
        put(&s.out, SYNC);
        s.mode = .group;
        l.transmit(s);
    }

    // A statement of the group failed and nothing was committed: each write
    // runs again as its own transaction.
    fn sendAlone(l: *Loop, s: *Session) void {
        s.out.clearRetainingCapacity();
        for (s.batch[0..s.count]) |job| {
            appendQuery(&s.out, job);
            put(&s.out, SYNC);
        }
        s.mode = .alone;
        l.transmit(s);
    }

    fn transmit(l: *Loop, s: *Session) void {
        s.busy = true;
        s.next = 0;
        s.cursor = 0;
        s.failed = false;
        s.skipping = false;
        s.rows = 0;
        s.bad_row = false;
        s.body.clearRetainingCapacity();
        if (s.mode != .reads) {
            for (s.results[0..s.count]) |*result| result.* = .{};
        }
        s.out_pos = 0;
        s.sent_at = l.now;
        s.last_send = l.now;
        l.flushSession(s);
    }

    fn flushSession(l: *Loop, s: *Session) void {
        const bytes = s.out.items;
        while (s.out_pos < bytes.len) {
            const n = c.send(s.fd, @ptrCast(bytes.ptr + s.out_pos), bytes.len - s.out_pos, c.MSG_NOSIGNAL);
            if (n > 0) {
                s.out_pos += @intCast(n);
                continue;
            }
            const code = c.__errno_location().*;
            if (n < 0 and code == c.EINTR) continue;
            if (n < 0 and code == c.EAGAIN) {
                s.flushing = true;
                const tag = (@as(u64, s.index) << 3) | TAG_SESSION;
                if (!l.watch(c.EPOLL_CTL_MOD, s.fd, EV_IN | EV_OUT | EV_RDHUP | EV_ET, tag)) l.lost(s);
                return;
            }
            return l.lost(s);
        }
        s.flushing = false;
    }

    fn sessionEvent(l: *Loop, s: *Session) void {
        if (s.fd < 0) return;
        if (s.flushing) l.flushSession(s);
        if (s.fd < 0) return;
        while (true) {
            if (s.w == s.incoming.len) {
                // Full: make room, unless one message fills the buffer.
                if (s.r == 0) return l.lost(s);
                std.mem.copyForwards(u8, s.incoming[0 .. s.w - s.r], s.incoming[s.r..s.w]);
                s.w -= s.r;
                s.r = 0;
            }
            const space = s.incoming.len - s.w;
            const n = c.read(s.fd, @ptrCast(s.incoming.ptr + s.w), space);
            if (n < 0) {
                const code = c.__errno_location().*;
                if (code == c.EINTR) continue;
                if (code == c.EAGAIN) return;
                return l.lost(s);
            }
            if (n == 0) return l.lost(s);
            const got: usize = @intCast(n);
            s.w += got;
            if (!l.parse(s)) return;
            // A short read emptied the socket; more raises a new event.
            if (got < space) return;
        }
    }

    // Handles the complete messages in the buffer. False: the session is lost.
    fn parse(l: *Loop, s: *Session) bool {
        while (s.w - s.r >= 5) {
            const kind = s.incoming[s.r];
            const size: usize = std.mem.readInt(u32, s.incoming[s.r + 1 ..][0..4], .big);
            if (size < 4 or size >= s.incoming.len) return l.broken(s);
            if (s.w - s.r < 1 + size) break;
            const body = s.incoming[s.r + 5 .. s.r + 1 + size];
            s.r += 1 + size;
            switch (kind) {
                // Notices, parameter changes and notifications can come at any time.
                'N', 'S', 'A' => continue,
                else => {},
            }
            // The server is closing the session (a shutdown, a terminated
            // backend): no statement failed, the connection did.
            if (kind == 'E' and endsSession(body)) return l.broken(s);
            if (!s.busy) return l.broken(s);
            const ok = if (s.mode == .reads) l.readMessage(s, kind, body) else l.writeMessage(s, kind, body);
            if (!ok) return false;
        }
        if (s.r == s.w) {
            s.r = 0;
            s.w = 0;
        }
        return true;
    }

    fn broken(l: *Loop, s: *Session) bool {
        l.lost(s);
        return false;
    }

    // Replies to a batch of reads. Each read is answered when its own
    // CommandComplete arrives.
    fn readMessage(l: *Loop, s: *Session, kind: u8, body: []const u8) bool {
        if (kind == '2') return true; // BindComplete
        if (kind == 'Z') {
            // ReadyForQuery. After a failed statement the server skipped the
            // rest of the batch: those reads never ran and go back in front.
            const skipped = s.batch[s.next..s.count];
            if (skipped.len > 0 and !s.skipping) return l.broken(s);
            s.busy = false;
            s.count = 0;
            if (skipped.len > 0 and !l.reads.unshift(skipped)) {
                for (skipped) |job| l.failJob(job);
            }
            return true;
        }
        if (s.next >= s.count or s.skipping) return l.broken(s);
        const job = s.batch[s.next];
        switch (kind) {
            'D' => {
                if (job.kind == .feed) put(&s.body, if (s.rows == 0) "{\"posts\":[" else ",");
                if (job.kind == .post) put(&s.body, "{\"post\":");
                if (job.kind != .health and !appendPostRow(&s.body, body)) s.bad_row = true;
                s.rows += 1;
                return true;
            },
            'C' => {
                s.next += 1;
                if (s.bad_row) {
                    l.failJob(job);
                } else switch (job.kind) {
                    .feed => {
                        put(&s.body, if (s.rows == 0) "{\"posts\":[]}" else "]}");
                        l.answer(job, 200, s.body.items);
                    },
                    .post => if (s.rows == 0) l.answerError(job, 404, "post not found") else {
                        put(&s.body, "}");
                        l.answer(job, 200, s.body.items);
                    },
                    else => {
                        const uptime: u64 = @intCast(@divTrunc(std.time.nanoTimestamp() - l.app.started_ns, std.time.ns_per_s));
                        l.body.clearRetainingCapacity();
                        l.body.writer().print("{{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":{d}}}", .{uptime}) catch fatal("out of memory");
                        l.answer(job, 200, l.body.items);
                    },
                }
            },
            'E' => {
                // Only this read failed.
                s.next += 1;
                s.skipping = true;
                l.failJob(job);
            },
            else => return l.broken(s),
        }
        s.rows = 0;
        s.bad_row = false;
        s.body.clearRetainingCapacity();
        return true;
    }

    // Replies to writes. Nothing is answered before ReadyForQuery.
    fn writeMessage(l: *Loop, s: *Session, kind: u8, body: []const u8) bool {
        switch (kind) {
            '2' => return true,
            'D' => {
                if (s.cursor >= s.count or body.len < 2) return l.broken(s);
                const result = &s.results[s.cursor];
                var row = body[2..];
                const id = column(&row) orelse return l.broken(s);
                const created = column(&row) orelse return l.broken(s);
                if (!isDecimal(id) or created.len != 8) return l.broken(s);
                @memcpy(result.id[0..id.len], id);
                result.id_len = @intCast(id.len);
                @memcpy(result.created[0..created.len], created);
                result.created_len = @intCast(created.len);
                return true;
            },
            'C' => {
                if (s.cursor >= s.count) return l.broken(s);
                s.results[s.cursor].inserted = std.mem.startsWith(u8, body, "INSERT 0 1");
                s.results[s.cursor].done = true;
                if (s.mode == .group) s.cursor += 1;
                return true;
            },
            'E' => {
                if (s.mode == .group) {
                    s.failed = true;
                    return true;
                }
                if (s.cursor >= s.count) return l.broken(s);
                s.results[s.cursor].done = false;
                s.results[s.cursor].not_found = missingPost(body);
                return true;
            },
            'Z' => {
                if (s.mode == .group) {
                    if (s.failed) {
                        l.sendAlone(s);
                        return s.fd >= 0;
                    }
                    if (s.cursor != s.count) return l.broken(s);
                    // Committed.
                    const count = s.count;
                    s.busy = false;
                    s.count = 0;
                    for (s.batch[0..count], s.results[0..count]) |job, result| l.answerWrite(job, result);
                    return true;
                }
                if (s.cursor >= s.count) return l.broken(s);
                const job = s.batch[s.cursor];
                const result = s.results[s.cursor];
                s.cursor += 1;
                s.next = s.cursor;
                if (s.cursor == s.count) {
                    s.busy = false;
                    s.count = 0;
                }
                l.answerWrite(job, result);
                return true;
            },
            else => return l.broken(s),
        }
    }
};

fn envOr(name: []const u8, fallback: []const u8) []const u8 {
    return std.posix.getenv(name) orelse fallback;
}

fn setting(name: []const u8, fallback: usize, low: usize, high: usize) !usize {
    const raw = std.posix.getenv(name) orelse return fallback;
    const value = std.fmt.parseInt(usize, raw, 10) catch return error.Configuration;
    if (value < low or value > high) return error.Configuration;
    return value;
}

pub fn main() !void {
    const allocator = std.heap.c_allocator;
    const db_raw = std.posix.getenv("DATABASE_URL") orelse {
        std.debug.print("zig-epoll-kamkm: DATABASE_URL is required\n", .{});
        return error.Configuration;
    };
    const host = envOr("HOST", "127.0.0.1");
    var ports = [2]u16{ std.fmt.parseInt(u16, envOr("PORT", "3000"), 10) catch return error.Configuration, 0 };
    var port_count: usize = 1;
    if (std.posix.getenv("EXTRA_PORT")) |value| if (value.len > 0) {
        ports[1] = std.fmt.parseInt(u16, value, 10) catch return error.Configuration;
        port_count = 2;
    };

    // DB_POOL is the ceiling on PostgreSQL sessions. Every loop has its own:
    // read sessions (READ_SESSIONS is the total over all loops) and write
    // sessions (WRITE_SESSIONS per loop). A loop left with one session uses
    // it for both.
    const pool = try setting("DB_POOL", 64, 1, 512);
    const cpus = std.Thread.getCpuCount() catch 1;
    const loops = try setting("HTTP_LOOPS", @min(cpus, pool, 256), 1, @min(pool, 256));
    const share = pool / loops;
    const writers = @min(try setting("WRITE_SESSIONS", 1, 0, 64), share - 1);
    const read_total = try setting("READ_SESSIONS", 16, 1, 512);
    const readers = @max(1, @min((read_total + loops - 1) / loops, share - writers));

    var app = App{
        .allocator = allocator,
        .db_url = try allocator.dupeZ(u8, db_raw),
        .jwt_secret = try allocator.dupe(u8, envOr("JWT_SECRET", "million-users-challenge")),
        .started_ns = std.time.nanoTimestamp(),
        .plan = .{
            .readers = readers,
            .writers = writers,
            .batch = try setting("READ_BATCH", 64, 1, MAX_BATCH),
            .read_window = @intCast(try setting("READ_WINDOW_US", 5000, 0, 1_000_000)),
            .write_window = @intCast(try setting("WRITE_WINDOW_US", 5000, 0, 1_000_000)),
        },
    };
    const all = try allocator.alloc(*Loop, loops);
    for (all) |*slot| slot.* = try Loop.create(&app, host, ports[0..port_count]);
    std.debug.print("zig-epoll-kamkm: {d} event loops on {s}; per loop {d} read sessions (batch {d}, window {d} us) and {d} write sessions (window {d} us)\n", .{
        loops, host, readers, app.plan.batch, app.plan.read_window, writers, app.plan.write_window,
    });
    for (all[1..]) |l| {
        const thread = try std.Thread.spawn(.{}, Loop.run, .{l});
        thread.detach();
    }
    all[0].run();
}
