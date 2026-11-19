//! One io_uring loop per core. A loop owns its ring, its listening sockets,
//! the clients it accepted and its PostgreSQL sessions, so a request is
//! received, parsed, queued, answered and sent on one thread. Accepts,
//! receives and sends are operations in the ring: a pass of the loop hands
//! the kernel everything it queued and collects everything that completed
//! with one system call. libpq connects, authenticates and prepares the
//! statements; the loop then speaks the wire protocol on the session's
//! socket itself (see "PostgreSQL sessions" below).
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
        @cInclude("sys/eventfd.h");
    @cInclude("sys/socket.h");
    @cInclude("sys/types.h");
    @cInclude("time.h");
    @cInclude("unistd.h");
});
const linux = std.os.linux;
const IoUring = linux.IoUring;

const Allocator = std.mem.Allocator;
const MAX_REQUEST = http.max_request;
const DB_FEED_SQL = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const DB_POST_SQL = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1";
const DB_INSERT_SQL = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
const DB_LIKE_SQL = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";

const EVENT_BATCH = 1024;
const READ_CHUNK = 64 * 1024;
const WRITE_GROUP = 64;
const MAX_BATCH = 256;
const WIRE_BUFFER = 256 * 1024;
const DATABASE_TIMEOUT_US: i64 = 5_000_000;
const REDIAL_US: i64 = 250_000;

// The ring: queue sizes, and the buffers that receives on client sockets
// take their memory from.
const SQ_ENTRIES = 4096;
const CQ_ENTRIES = 32768;
const BUFFER_GROUP = 0;
const BUFFER_SIZE = 4096;
const BUFFER_COUNT = 4096;
// Input a client may send ahead while it waits for a response.
const KEEP_LIMIT = 1024 * 1024;

// What a completion refers to, in the low bits of its user data. A client
// is a pointer (aligned, so its low bits are zero); the others carry a
// descriptor or an index above the tag.
const OP_RECV: u64 = 0;
const OP_SEND: u64 = 1;
const OP_ACCEPT: u64 = 2;
const OP_SESSION_RECV: u64 = 3;
const OP_SESSION_SEND: u64 = 4;
const OP_WAKE: u64 = 5;
const OP_NONE: u64 = 6;

const Plan = struct {
    readers: usize,
    writers: usize,
    batch: usize,
    read_window: i64,
    write_window: i64,
    // The length of a pass, and the number of completions that ends one early.
    pass: i64,
    wake: u32,
};

const App = struct {
    allocator: Allocator,
    db_url: [:0]const u8,
    jwt_secret: []const u8,
    started_ns: i128,
    plan: Plan,
    loops: usize = 1,
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
    if (raw.len >= 19) {
        try out.appendSlice(raw[0..10]);
        try out.append('T');
        try out.appendSlice(raw[11..19]);
    } else return error.InvalidTimestamp;
    var frac_start: ?usize = null;
    if (raw.len > 19 and raw[19] == '.') frac_start = 20;
    try out.append('.');
    var wrote: usize = 0;
    if (frac_start) |start| {
        var i = start;
        while (i < raw.len and raw[i] >= '0' and raw[i] <= '9' and wrote < 3) : (i += 1) {
            try out.append(raw[i]);
            wrote += 1;
        }
    }
    while (wrote < 3) : (wrote += 1) try out.append('0');
    try out.append('Z');
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
    std.debug.print("zig-iouring-kamkm: " ++ message ++ "\n", .{});
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
    // An escaped byte takes at most six.
    out.ensureUnusedCapacity(value.len * 6) catch fatal("out of memory");
    escapeAssumeCapacity(out, value);
}

fn needsEscape(ch: u8) bool {
    return ch < 0x20 or ch == '"' or ch == '\\';
}

// Index of the first byte at or after from that JSON must escape, or value.len.
// Sixteen bytes are checked at a time.
fn nextEscape(value: []const u8, from: usize) usize {
    const V = @Vector(16, u8);
    var i = from;
    while (i + 16 <= value.len) : (i += 16) {
        const v: V = value[i..][0..16].*;
        const low: u16 = @bitCast(v < @as(V, @splat(0x20)));
        const quote: u16 = @bitCast(v == @as(V, @splat('"')));
        const slash: u16 = @bitCast(v == @as(V, @splat('\\')));
        const hits = low | quote | slash;
        if (hits != 0) return i + @ctz(hits);
    }
    while (i < value.len and !needsEscape(value[i])) i += 1;
    return i;
}

fn escapeAssumeCapacity(out: *std.ArrayList(u8), value: []const u8) void {
    var start: usize = 0;
    while (true) {
        const i = nextEscape(value, start);
        out.appendSliceAssumeCapacity(value[start..i]);
        if (i == value.len) return;
        const ch = value[i];
        switch (ch) {
            '"' => out.appendSliceAssumeCapacity("\\\""),
            '\\' => out.appendSliceAssumeCapacity("\\\\"),
            8 => out.appendSliceAssumeCapacity("\\b"),
            9 => out.appendSliceAssumeCapacity("\\t"),
            10 => out.appendSliceAssumeCapacity("\\n"),
            12 => out.appendSliceAssumeCapacity("\\f"),
            13 => out.appendSliceAssumeCapacity("\\r"),
            else => {
                const digits = "0123456789abcdef";
                out.appendSliceAssumeCapacity("\\u00");
                out.appendSliceAssumeCapacity(&[_]u8{ digits[ch >> 4], digits[ch & 0x0f] });
            },
        }
        start = i + 1;
    }
}

// ---------------------------------------------------------------------------
// PostgreSQL wire messages
//
// A batch is one write: a Bind and an Execute per statement, closed by one
// Sync. Nothing asks the server to describe a result, so no RowDescription
// comes back; parameters and results are text, and JSON is written straight
// from the DataRow bytes.

const SYNC = "S\x00\x00\x00\x04";

fn appendBind(out: *std.ArrayList(u8), statement: []const u8, parameters: []const []const u8) void {
    const start = out.items.len;
    put(out, "B\x00\x00\x00\x00\x00"); // type, length (set below), unnamed portal
    put(out, statement);
    put(out, "\x00\x00\x00"); // end of the name; no parameter formats: all text
    putInt(out, u16, @intCast(parameters.len));
    for (parameters) |parameter| {
        putInt(out, u32, @intCast(parameter.len));
        put(out, parameter);
    }
    put(out, "\x00\x00"); // no result formats: all text
    std.mem.writeInt(u32, out.items[start + 1 ..][0..4], @intCast(out.items.len - start - 1), .big);
    put(out, "E\x00\x00\x00\x09\x00\x00\x00\x00\x00"); // Execute the unnamed portal, no row limit
}

fn appendQuery(out: *std.ArrayList(u8), job: Job) void {
    var first: [20]u8 = undefined;
    var second: [20]u8 = undefined;
    switch (job.kind) {
        .feed => appendBind(out, "feed", &[_][]const u8{}),
        .health => appendBind(out, "health", &[_][]const u8{}),
        .post => appendBind(out, "post", &[_][]const u8{decimal(&first, job.id)}),
        .create => appendBind(out, "insert_post", &[_][]const u8{ decimal(&first, job.user), job.text.?[0..job.body_len] }),
        .like => appendBind(out, "like_post", &[_][]const u8{ decimal(&first, job.user), decimal(&second, job.id) }),
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
    if (created.len < 19) return false;
    // Room for the whole object, so nothing below grows the list.
    out.ensureUnusedCapacity(128 + id.len + likes.len + (text.len + author.len) * 6) catch fatal("out of memory");
    out.appendSliceAssumeCapacity("{\"id\":");
    out.appendSliceAssumeCapacity(id);
    out.appendSliceAssumeCapacity(",\"body\":\"");
    escapeAssumeCapacity(out, text);
    out.appendSliceAssumeCapacity("\",\"created_at\":\"");
    timestampAssumeCapacity(out, created);
    out.appendSliceAssumeCapacity("\",\"author\":\"");
    escapeAssumeCapacity(out, author);
    out.appendSliceAssumeCapacity("\",\"like_count\":");
    out.appendSliceAssumeCapacity(likes);
    out.appendAssumeCapacity('}');
    return true;
}

// appendTimestamp for a value already checked to be at least 19 bytes long,
// into a list with 24 bytes to spare.
fn timestampAssumeCapacity(out: *std.ArrayList(u8), raw: []const u8) void {
    out.appendSliceAssumeCapacity(raw[0..10]);
    out.appendAssumeCapacity('T');
    out.appendSliceAssumeCapacity(raw[11..19]);
    out.appendAssumeCapacity('.');
    var wrote: usize = 0;
    if (raw.len > 19 and raw[19] == '.') {
        var i: usize = 20;
        while (i < raw.len and raw[i] >= '0' and raw[i] <= '9' and wrote < 3) : (i += 1) {
            out.appendAssumeCapacity(raw[i]);
            wrote += 1;
        }
    }
    while (wrote < 3) : (wrote += 1) out.appendAssumeCapacity('0');
    out.appendAssumeCapacity('Z');
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

// One cache line per client: a completion touches the client's flags and
// its buffers, and with a million clients most of them are cold.
const Client = struct {
    // A direct descriptor (a slot in the ring's file table) when fixed.
    fd: c_int,
    fixed: bool = false,
    // A request of this client is queued or with the database.
    busy: bool = false,
    // The ring holds this client's receive.
    receiving: bool = false,
    // A cancel of that receive is on its way.
    stopping: bool = false,
    // The ring holds a send of this client's output.
    sending: bool = false,
    // The peer has closed its side.
    eof: bool = false,
    closed: bool = false,
    continued: bool = false,
    close_after: bool = false,
    gzip: bool = false,
    interim: bool = false,
    // Input kept across receives: a partial request, or requests sent ahead.
    input: ?[]u8 = null,
    input_len: u32 = 0,
    // The response being sent.
    output: ?[]u8 = null,
    output_pos: u32 = 0,
    next: ?*Client = null,
};

comptime {
    std.debug.assert(@sizeOf(Client) <= 64);
}

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
    out: std.ArrayList(u8),
    out_pos: usize = 0,
    // What the ring holds for this session's socket.
    receiving: bool = false,
    sending: bool = false,
    // A batch was built while the report of the last send was still due.
    restart: bool = false,
    // Connected while operations on the lost socket were still in the ring.
    fresh: ?*c.PGconn = null,
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
    ring: IoUring = undefined,
    // Memory the kernel receives client input into: BUFFER_COUNT buffers of
    // BUFFER_SIZE bytes, lent to the kernel and given back once read.
    pool: []u8 = &.{},
    returned: std.ArrayList(u16),
    listeners: [2]c_int = .{ -1, -1 },
    listener_count: usize = 0,
    // Clients take direct descriptors, slots in the ring's file table, while
    // the table has room: an operation on one skips the process's descriptor
    // table and the file's reference count.
    direct: bool = false,
    files: u32 = 0,
    clients: std.heap.MemoryPoolAligned(Client, 64),
    wake_fd: c_int,
    now: i64 = 0,
    // The client whose buffered input serve() is working through.
    serving: ?*Client = null,
    // Closed clients, freed at the end of the pass so that nothing in the pass
    // can hold a pointer to freed memory.
    graves: ?*Client = null,
    wrote_last: bool = false,
    // When the loop last started a read batch and a write batch.
    reads_sent: i64 = -(1 << 40),
    writes_sent: i64 = -(1 << 40),
    sessions: []Session,
    reads: JobQueue,
    writes: JobQueue,
    wire: std.ArrayList(u8),
    body: std.ArrayList(u8),
    inbox_mutex: std.Thread.Mutex = .{},
    inbox: std.ArrayList(Dialed),

    fn create(app: *App, host: []const u8, ports: []const u16) !*Loop {
        const allocator = app.allocator;
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
            .wake_fd = wake_fd,
            .sessions = sessions,
            .reads = .{ .items = std.ArrayList(Job).init(allocator) },
            .writes = .{ .items = std.ArrayList(Job).init(allocator) },
            .wire = std.ArrayList(u8).init(allocator),
            .body = std.ArrayList(u8).init(allocator),
            .inbox = try std.ArrayList(Dialed).initCapacity(allocator, sessions.len),
            .clients = std.heap.MemoryPoolAligned(Client, 64).init(allocator),
            .returned = try std.ArrayList(u16).initCapacity(allocator, BUFFER_COUNT),
        };
        for (ports) |port| {
            l.listeners[l.listener_count] = try makeListener(host, port, allocator);
            l.listener_count += 1;
        }
        return l;
    }

    // Makes the ring on the thread that will use it: the kernel then runs the
    // ring's completion work only when this thread asks for it.
    fn open(l: *Loop) !void {
        const common: u32 = linux.IORING_SETUP_SUBMIT_ALL | linux.IORING_SETUP_CLAMP | linux.IORING_SETUP_CQSIZE;
        var params = std.mem.zeroInit(linux.io_uring_params, .{
            .flags = common | linux.IORING_SETUP_SINGLE_ISSUER | linux.IORING_SETUP_DEFER_TASKRUN,
            .cq_entries = CQ_ENTRIES,
        });
        l.ring = IoUring.init_params(SQ_ENTRIES, &params) catch older: {
            // Before Linux 6.1: completion work runs when the thread enters
            // the kernel, whatever it entered for.
            params = std.mem.zeroInit(linux.io_uring_params, .{
                .flags = common | linux.IORING_SETUP_COOP_TASKRUN,
                .cq_entries = CQ_ENTRIES,
            });
            break :older try IoUring.init_params(SQ_ENTRIES, &params);
        };
        if (l.ring.features & linux.IORING_FEAT_EXT_ARG == 0) return error.Unsupported;
        l.pool = try l.allocator.alloc(u8, BUFFER_SIZE * BUFFER_COUNT);
        l.registerFiles();
        l.provide(0, BUFFER_COUNT);
        for (l.listeners[0..l.listener_count]) |fd| l.armAccept(fd);
        l.armWake();
    }

    // A sparse file table for the clients: the loop's share of the descriptor
    // limit, twice over, between 64Ki and the kernel's 1Mi slots.
    fn registerFiles(l: *Loop) void {
        const limit = std.posix.getrlimit(.NOFILE) catch return;
        const loops: u64 = @max(1, l.app.loops);
        const share = std.math.ceilPowerOfTwo(u64, @max(1, limit.cur * 2 / loops)) catch 1 << 20;
        const slots: u32 = @intCast(@min(@max(share, 1 << 16), @min(1 << 20, limit.cur)));
        var reg = std.mem.zeroInit(linux.io_uring_rsrc_register, .{
            .nr = slots,
            .flags = linux.IORING_RSRC_REGISTER_SPARSE,
        });
        const rc = linux.io_uring_register(l.ring.fd, .REGISTER_FILES2, &reg, @sizeOf(linux.io_uring_rsrc_register));
        if (linux.E.init(rc) != .SUCCESS) return;
        l.direct = true;
        l.files = slots;
    }

    fn run(l: *Loop) void {
        l.open() catch |err| {
            std.debug.print("zig-iouring-kamkm: cannot set up io_uring ({s}); Linux 6.0 or later is needed\n", .{@errorName(err)});
            std.process.exit(1);
        };
        var cqes: [EVENT_BATCH]linux.io_uring_cqe = undefined;
        l.now = monotonicMicros();
        for (l.sessions) |*s| l.dial(s);
        var quiet = true;
        while (true) {
            l.wait(quiet);
            l.now = monotonicMicros();
            var seen: usize = 0;
            while (seen < 16 * EVENT_BATCH) {
                const n = l.ring.copy_cqes(&cqes, 0) catch 0;
                if (n == 0) break;
                for (cqes[0..n]) |cqe| l.complete(cqe);
                seen += n;
            }
            quiet = seen == 0;
            l.timers();
            l.pump();
            while (l.graves) |cl| {
                l.graves = cl.next;
                l.clients.destroy(@alignCast(cl));
            }
        }
    }

    // Hands what the pass queued to the kernel and sleeps, in one system
    // call. A loop that found nothing sleeps until something completes. A
    // loop that had work sleeps to the end of its pass, and what completes
    // meanwhile waits in the ring without waking it, unless a full batch
    // has gathered.
    fn wait(l: *Loop, quiet: bool) void {
        const plan = l.app.plan;
        const t = monotonicMicros();
        var limit = l.due(t);
        var want: u32 = 1;
        if (!quiet and plan.pass > 0) {
            limit = @min(limit, l.now + plan.pass - t);
            want = plan.wake;
        }
        if (limit <= 0) {
            limit = 0;
            want = 0;
        }
        l.giveBack();
        const queued = l.ring.flush_sq();
        var span = linux.kernel_timespec{
            .tv_sec = @intCast(@divTrunc(limit, 1_000_000)),
            .tv_nsec = @intCast(@rem(limit, 1_000_000) * 1000),
        };
        var arg = linux.io_uring_getevents_arg{
            .sigmask = 0,
            .sigmask_sz = linux.NSIG / 8,
            .pad = 0,
            .ts = if (want > 0) @intFromPtr(&span) else 0,
        };
        const rc = linux.syscall6(
            .io_uring_enter,
            @as(usize, @bitCast(@as(isize, l.ring.fd))),
            queued,
            want,
            linux.IORING_ENTER_GETEVENTS | linux.IORING_ENTER_EXT_ARG,
            @intFromPtr(&arg),
            @sizeOf(linux.io_uring_getevents_arg),
        );
        switch (linux.E.init(rc)) {
            .SUCCESS, .TIME, .INTR, .AGAIN, .BUSY => {},
            else => fatal("io_uring_enter failed"),
        }
    }

    // How long the loop may sleep: until a gather window ends, a redial is
    // due or a batch has been out too long.
    fn due(l: *Loop, t: i64) i64 {
        const plan = l.app.plan;
        var span: i64 = 1_000_000;
        for (l.sessions) |*s| {
            if (s.fd < 0) {
                if (!s.dialing) span = @min(span, s.retry_at - t);
            } else if (s.busy) {
                span = @min(span, s.sent_at + DATABASE_TIMEOUT_US - t);
            } else {
                if (s.reads and l.reads.len() > 0) span = @min(span, l.reads_sent + plan.read_window - t);
                if (s.writes and l.writes.len() > 0) span = @min(span, l.writes_sent + plan.write_window - t);
            }
        }
        return span;
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

    // A free submission entry. A full queue is handed to the kernel first.
    fn sqe(l: *Loop) *linux.io_uring_sqe {
        var tries: usize = 0;
        while (true) : (tries += 1) {
            if (l.ring.get_sqe()) |entry| return entry else |_| {}
            if (tries == 1000) fatal("the io_uring submission queue does not drain");
            _ = l.ring.submit() catch {};
        }
    }

    fn complete(l: *Loop, cqe: linux.io_uring_cqe) void {
        const word = cqe.user_data;
        const subject: usize = @intCast(word & ~@as(u64, 7));
        switch (word & 7) {
            OP_RECV => l.clientRecv(@ptrFromInt(subject), cqe),
            OP_SEND => l.clientSent(@ptrFromInt(subject), cqe.res),
            OP_ACCEPT => l.accepted(@intCast(word >> 4), (word >> 3) & 1 == 1, cqe),
            OP_SESSION_RECV => l.sessionRecv(&l.sessions[@intCast(word >> 3)], cqe.res),
            OP_SESSION_SEND => l.sessionSent(&l.sessions[@intCast(word >> 3)], cqe.res),
            OP_WAKE => {
                l.wakeEvent();
                l.armWake();
            },
            else => {},
        }
    }

    // Lends buffers first..first+count to the kernel. Buffers are lent with
    // IORING_OP_PROVIDE_BUFFERS rather than a registered buffer ring, which
    // some kernels of the 6.8 series refuse.
    fn provide(l: *Loop, first: usize, count: usize) void {
        const entry = l.sqe();
        entry.prep_provide_buffers(l.pool.ptr + first * BUFFER_SIZE, BUFFER_SIZE, count, BUFFER_GROUP, first);
        entry.flags |= linux.IOSQE_CQE_SKIP_SUCCESS;
        entry.user_data = OP_NONE;
    }

    fn bufferAt(l: *Loop, id: u16) []u8 {
        return l.pool[@as(usize, id) * BUFFER_SIZE ..][0..BUFFER_SIZE];
    }

    fn release(l: *Loop, id: u16) void {
        l.returned.append(id) catch l.provide(id, 1);
    }

    // Lends the buffers read in this pass back, a run of neighbours at a time.
    fn giveBack(l: *Loop) void {
        const ids = l.returned.items;
        if (ids.len == 0) return;
        std.mem.sort(u16, ids, {}, std.sort.asc(u16));
        var start: usize = 0;
        for (ids, 0..) |id, i| {
            const last = i + 1 == ids.len or ids[i + 1] != id + 1;
            if (!last) continue;
            l.provide(ids[start], i + 1 - start);
            start = i + 1;
        }
        l.returned.clearRetainingCapacity();
    }

    fn armWake(l: *Loop) void {
        const entry = l.sqe();
        entry.prep_poll_add(l.wake_fd, linux.POLL.IN);
        entry.user_data = OP_WAKE;
    }

    // ---- clients ----

    // One accept per listener stays in the ring and reports every connection.
    fn armAccept(l: *Loop, listener: c_int) void {
        const entry = l.sqe();
        if (l.direct) {
            // A direct descriptor is in no descriptor table: no SOCK_CLOEXEC
            // (the kernel refuses it).
            entry.prep_multishot_accept_direct(listener, null, null, 0);
        } else {
            entry.prep_multishot_accept(listener, null, null, c.SOCK_CLOEXEC);
        }
        entry.user_data = (@as(u64, @intCast(listener)) << 4) | (@as(u64, @intFromBool(l.direct)) << 3) | OP_ACCEPT;
    }

    fn accepted(l: *Loop, listener: c_int, direct: bool, cqe: linux.io_uring_cqe) void {
        // A full file table: from now on clients get ordinary descriptors.
        if (direct and cqe.res == -@as(i32, @intFromEnum(linux.E.NFILE))) l.direct = false;
        if (cqe.flags & linux.IORING_CQE_F_MORE == 0) l.armAccept(listener);
        if (cqe.res < 0) return;
        const fd: c_int = cqe.res;
        const cl = l.clients.create() catch {
            if (direct) l.closeDirect(fd) else _ = c.close(fd);
            return;
        };
        cl.* = .{ .fd = fd, .fixed = direct };
        l.armRecv(cl);
    }

    fn closeDirect(l: *Loop, slot: c_int) void {
        const entry = l.sqe();
        entry.prep_close_direct(@intCast(slot));
        entry.flags |= linux.IOSQE_CQE_SKIP_SUCCESS;
        entry.user_data = OP_NONE;
    }

    // One receive per client stays in the ring. Each time the socket has
    // input the kernel copies it into one of the loop's lent buffers and
    // reports it; an idle client holds no buffer.
    fn armRecv(l: *Loop, cl: *Client) void {
        const entry = l.sqe();
        entry.prep_rw(.RECV, cl.fd, 0, 0, 0);
        entry.flags |= linux.IOSQE_BUFFER_SELECT;
        if (cl.fixed) entry.flags |= linux.IOSQE_FIXED_FILE;
        entry.buf_index = BUFFER_GROUP;
        entry.ioprio |= linux.IORING_RECV_MULTISHOT;
        entry.user_data = @intFromPtr(cl) | OP_RECV;
        cl.receiving = true;
    }

    fn rearm(l: *Loop, cl: *Client) void {
        if (cl.closed or cl.eof or cl.receiving or cl.input_len >= READ_CHUNK) return;
        l.armRecv(cl);
    }

    fn clientRecv(l: *Loop, cl: *Client, cqe: linux.io_uring_cqe) void {
        const more = cqe.flags & linux.IORING_CQE_F_MORE != 0;
        if (!more) {
            cl.receiving = false;
            cl.stopping = false;
        }
        if (cqe.flags & linux.IORING_CQE_F_BUFFER != 0) {
            const id: u16 = @intCast(cqe.flags >> linux.IORING_CQE_BUFFER_SHIFT);
            if (cqe.res > 0 and !cl.closed) l.received(cl, l.bufferAt(id)[0..@intCast(cqe.res)]);
            l.release(id);
        }
        if (cqe.res == 0) {
            cl.eof = true;
        } else if (cqe.res < 0) switch (linux.E.init(@bitCast(@as(isize, cqe.res)))) {
            // No free buffer, or our own cancel: the receive is armed again.
            .NOBUFS, .CANCELED, .INTR, .AGAIN => {},
            else => l.closeClient(cl),
        };
        if (more) return;
        if (cl.closed) return l.settle(cl);
        if (cl.eof) {
            // The peer has finished sending. A request still with the
            // database is answered first.
            if (!cl.busy and cl.output == null) l.closeClient(cl);
            return;
        }
        l.rearm(cl);
    }

    // Input in a lent buffer. An idle client's request is parsed where it
    // lies. What arrives while the client waits for the database or for a
    // send is kept, in order, and served after the response.
    fn received(l: *Loop, cl: *Client, data: []const u8) void {
        const blocked = cl.busy or cl.output != null;
        if (cl.input_len == 0 and !blocked) return l.serve(cl, data);
        if (!l.keep(cl, data)) return;
        if (!blocked) return l.serve(cl, cl.input.?[0..cl.input_len]);
        // Enough is kept: stop receiving until the client is served again.
        if (cl.input_len >= READ_CHUNK and cl.receiving and !cl.stopping) {
            const entry = l.sqe();
            entry.prep_cancel(@intFromPtr(cl) | OP_RECV, 0);
            entry.user_data = OP_NONE;
            cl.stopping = true;
        }
    }

    fn keep(l: *Loop, cl: *Client, data: []const u8) bool {
        const need = cl.input_len + data.len;
        if (need > KEEP_LIMIT) {
            l.closeClient(cl);
            return false;
        }
        var held: []u8 = cl.input orelse &.{};
        if (held.len < need) {
            const size = @max(512, std.math.ceilPowerOfTwo(usize, need) catch need);
            const grown = if (cl.input) |old| l.allocator.realloc(old, size) else l.allocator.alloc(u8, size);
            held = grown catch {
                l.closeClient(cl);
                return false;
            };
            cl.input = held;
        }
        @memcpy(held[cl.input_len..need], data);
        cl.input_len = @intCast(need);
        return true;
    }

    fn closeClient(l: *Loop, cl: *Client) void {
        if (cl.closed) return;
        cl.closed = true;
        if (cl.input) |buffer| l.allocator.free(buffer);
        cl.input = null;
        cl.input_len = 0;
        // The ring may still hold this client's receive and a send. The
        // shutdown completes both; the descriptor and the memory stay until
        // the last of them has been reported.
        if (cl.receiving or cl.sending) {
            if (cl.fixed) {
                const entry = l.sqe();
                entry.prep_shutdown(cl.fd, linux.SHUT.RDWR);
                entry.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_CQE_SKIP_SUCCESS;
                entry.user_data = OP_NONE;
            } else _ = c.shutdown(cl.fd, c.SHUT_RDWR);
        }
        l.settle(cl);
    }

    // Closes the descriptor once the ring holds nothing more for the client.
    fn settle(l: *Loop, cl: *Client) void {
        if (cl.receiving or cl.sending or cl.fd < 0) return;
        if (cl.fixed) l.closeDirect(cl.fd) else _ = c.close(cl.fd);
        cl.fd = -1;
        if (cl.output) |buffer| l.allocator.free(buffer);
        cl.output = null;
        // A busy client is still named by a job; the job's answer buries it.
        if (!cl.busy) l.bury(cl);
    }

    fn bury(l: *Loop, cl: *Client) void {
        cl.next = l.graves;
        l.graves = cl;
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
            cl.gzip = head.accept_gzip;
            l.handle(cl, rest[0..used], head);
            if (cl.closed) return;
            rest = rest[used..];
        }
        l.stash(cl, rest);
    }

    // Keeps unserved input with the client. rest is either part of the
    // client's own buffer or of a buffer of the ring.
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
                cl.input_len = @intCast(rest.len);
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
        cl.input_len = @intCast(rest.len);
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
        out.writer().print("HTTP/1.1 {d} {s}\r\nContent-Type: application/json\r\n{s}Content-Length: {d}\r\nConnection: {s}\r\n\r\n", .{
            status,
            statusReason(status),
            if (compressed) "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n" else "",
            payload.len,
            if (cl.close_after) "close" else "keep-alive",
        }) catch fatal("out of memory");
        put(out, payload);
        l.send(cl, out.items, false);
    }

    // Queues a send. The bytes are copied: they must stay where they are
    // until the kernel reports the send, and the caller's buffer is reused.
    fn send(l: *Loop, cl: *Client, bytes: []const u8, interim: bool) void {
        if (cl.output != null) return l.closeClient(cl);
        cl.output = l.allocator.dupe(u8, bytes) catch return l.closeClient(cl);
        cl.output_pos = 0;
        cl.interim = interim;
        l.armSend(cl);
    }

    fn armSend(l: *Loop, cl: *Client) void {
        const entry = l.sqe();
        entry.prep_send(cl.fd, cl.output.?[cl.output_pos..], c.MSG_NOSIGNAL | c.MSG_WAITALL);
        if (cl.fixed) entry.flags |= linux.IOSQE_FIXED_FILE;
        entry.user_data = @intFromPtr(cl) | OP_SEND;
        cl.sending = true;
    }

    fn clientSent(l: *Loop, cl: *Client, result: i32) void {
        cl.sending = false;
        if (cl.closed) return l.settle(cl);
        const bytes = cl.output orelse return;
        if (result < 0) switch (linux.E.init(@bitCast(@as(isize, result)))) {
            .INTR, .AGAIN => return l.armSend(cl),
            else => return l.closeClient(cl),
        };
        cl.output_pos += @intCast(result);
        if (cl.output_pos < bytes.len) {
            // The kernel took a part; a send of nothing means it will not go on.
            if (result == 0) return l.closeClient(cl);
            return l.armSend(cl);
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

    // A complete response has been sent.
    fn finished(l: *Loop, cl: *Client) void {
        if (cl.close_after) return l.closeClient(cl);
        l.resumeClient(cl);
    }

    // Goes on with what the client sent while it could not be served.
    fn resumeClient(l: *Loop, cl: *Client) void {
        if (cl.input_len > 0) l.serve(cl, cl.input.?[0..cl.input_len]);
        if (cl.closed) return;
        if (cl.eof and !cl.busy and cl.output == null) return l.closeClient(cl);
        l.rearm(cl);
    }

    // ---- answers ----

    fn answer(l: *Loop, job: Job, status: u16, body: []const u8) void {
        const cl = job.client;
        cl.busy = false;
        if (!cl.closed) {
            l.respond(cl, status, body);
        } else if (cl.fd < 0) {
            // Closed and done with the ring; otherwise settle() buries it.
            l.bury(cl);
        }
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
            const pg = dialed.pg orelse {
                s.dialing = false;
                s.retry_at = l.now + REDIAL_US;
                continue;
            };
            // The ring may still hold operations on the lost socket, and they
            // use this session's buffers: the new one starts after them.
            if (s.receiving or s.sending) s.fresh = pg else l.adopt(s, pg);
        }
    }

    fn adopt(l: *Loop, s: *Session, pg: *c.PGconn) void {
        s.dialing = false;
        const fd = c.PQsocket(pg);
        if (fd < 0) {
            c.PQfinish(pg);
            s.retry_at = l.now + REDIAL_US;
            return;
        }
        s.pg = pg;
        s.fd = fd;
        s.busy = false;
        s.restart = false;
        s.count = 0;
        s.r = 0;
        s.w = 0;
        l.armSession(s);
    }

    // The last operation on a lost socket has been reported.
    fn drained(l: *Loop, s: *Session) void {
        if (s.receiving or s.sending) return;
        if (s.fresh) |pg| {
            s.fresh = null;
            l.adopt(s, pg);
        }
    }

    // The session is gone. Reads it had not answered run once more on another
    // session; the outcome of its writes is unknown, so they fail and are
    // never sent again.
    fn lost(l: *Loop, s: *Session) void {
        if (s.fd < 0) return;
        // The shutdown completes what the ring still holds on the socket.
        _ = c.shutdown(s.fd, c.SHUT_RDWR);
        // Queued operations name the descriptor by number: hand them to the
        // kernel before PQfinish closes it and the number can be reused.
        _ = l.ring.submit() catch {};
        c.PQfinish(s.pg);
        s.pg = null;
        s.fd = -1;
        const pending = if (s.busy) s.batch[s.next..s.count] else s.batch[0..0];
        const reads = s.mode == .reads;
        s.busy = false;
        s.restart = false;
        s.count = 0;
        s.r = 0;
        s.w = 0;
        s.rows = 0;
        s.bad_row = false;
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

    // Hands queued work to the sessions that may send now. The loop starts a
    // read batch, and a write batch, no more than once per gather window, and
    // the batch takes everything queued (up to the batch limit) to one free
    // session; only a full batch goes out before the window ends. A window's
    // requests then share one send, one reply stream and its ACKs. An idle
    // loop sends at once.
    fn dispatch(l: *Loop, queue: *JobQueue, writes: bool) bool {
        if (queue.len() == 0) return false;
        const window = if (writes) l.app.plan.write_window else l.app.plan.read_window;
        const sent = if (writes) &l.writes_sent else &l.reads_sent;
        var alive: usize = 0;
        for (l.sessions) |*s| {
            if (!(if (writes) s.writes else s.reads)) continue;
            if (s.fd >= 0 or s.dialing) alive += 1;
        }
        if (alive == 0) {
            var one: [1]Job = undefined;
            while (queue.take(&one) == 1) l.failJob(one[0]);
            return false;
        }
        const limit: usize = if (writes) WRITE_GROUP else l.app.plan.batch;
        var started = false;
        for (l.sessions) |*s| {
            if (queue.len() == 0) break;
            if (queue.len() < limit and l.now < sent.* + window) break;
            if (!(if (writes) s.writes else s.reads)) continue;
            if (s.fd < 0 or s.busy) continue;
            sent.* = l.now;
            s.count = queue.take(s.batch[0..@min(limit, queue.len())]);
            if (writes) l.sendGroup(s) else l.sendReads(s);
            started = true;
        }
        return started;
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
        // The replies to the last batch are complete, so PostgreSQL has all
        // of it; if the report of that send is still due, start after it.
        if (s.sending) s.restart = true else l.sessionSend(s);
    }

    fn sessionSend(l: *Loop, s: *Session) void {
        const entry = l.sqe();
        entry.prep_send(s.fd, s.out.items[s.out_pos..], c.MSG_NOSIGNAL | c.MSG_WAITALL);
        entry.user_data = (@as(u64, s.index) << 3) | OP_SESSION_SEND;
        s.sending = true;
    }

    fn sessionSent(l: *Loop, s: *Session, result: i32) void {
        s.sending = false;
        if (s.fd < 0) return l.drained(s);
        if (result < 0) switch (linux.E.init(@bitCast(@as(isize, result)))) {
            .INTR, .AGAIN => return l.sessionSend(s),
            else => return l.lost(s),
        };
        if (s.restart) {
            s.restart = false;
            return l.sessionSend(s);
        }
        s.out_pos += @intCast(result);
        if (s.out_pos < s.out.items.len) {
            if (result == 0) return l.lost(s);
            l.sessionSend(s);
        }
    }

    // One receive at a time into the session's buffer, queued again after
    // each report.
    fn armSession(l: *Loop, s: *Session) void {
        if (s.r > 0 and s.incoming.len - s.w < READ_CHUNK) {
            std.mem.copyForwards(u8, s.incoming[0 .. s.w - s.r], s.incoming[s.r..s.w]);
            s.w -= s.r;
            s.r = 0;
        }
        // Full with one unfinished message: it does not fit.
        if (s.w == s.incoming.len) return l.lost(s);
        const entry = l.sqe();
        entry.prep_recv(s.fd, s.incoming[s.w..], 0);
        entry.user_data = (@as(u64, s.index) << 3) | OP_SESSION_RECV;
        s.receiving = true;
    }

    fn sessionRecv(l: *Loop, s: *Session, result: i32) void {
        s.receiving = false;
        if (s.fd < 0) return l.drained(s);
        if (result <= 0) {
            if (result < 0) switch (linux.E.init(@bitCast(@as(isize, result)))) {
                .INTR, .AGAIN => return l.armSession(s),
                else => {},
            };
            return l.lost(s);
        }
        s.w += @intCast(result);
        if (!l.parse(s)) return;
        l.armSession(s);
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
                if (!isDecimal(id) or created.len > result.created.len) return l.broken(s);
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
        std.debug.print("zig-iouring-kamkm: DATABASE_URL is required\n", .{});
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
    // it for both. A loop fills one session per gather window and uses the
    // others only while earlier batches are still out, so spare sessions cost
    // nothing until the database is slow, when a request left waiting past
    // 40 ms would cost the network two extra ACKs.
    const pool = try setting("DB_POOL", 128, 1, 512);
    const cpus = std.Thread.getCpuCount() catch 1;
    const loops = try setting("HTTP_LOOPS", @min(cpus, pool, 256), 1, @min(pool, 256));
    const share = pool / loops;
    const writers = @min(try setting("WRITE_SESSIONS", 2, 0, 64), share - 1);
    const read_total = try setting("READ_SESSIONS", 64, 1, 512);
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
            .read_window = @intCast(try setting("READ_WINDOW_US", 2000, 0, 1_000_000)),
            .write_window = @intCast(try setting("WRITE_WINDOW_US", 5000, 0, 1_000_000)),
            .pass = @intCast(try setting("PASS_US", 2000, 0, 100_000)),
            .wake = @intCast(try setting("PASS_EVENTS", 512, 1, 16384)),
        },
    };
    app.loops = loops;
    const all = try allocator.alloc(*Loop, loops);
    for (all) |*slot| slot.* = try Loop.create(&app, host, ports[0..port_count]);
    std.debug.print("zig-iouring-kamkm: {d} io_uring loops on {s}; per loop {d} read sessions (batch {d}, window {d} us) and {d} write sessions (window {d} us)\n", .{
        loops, host, readers, app.plan.batch, app.plan.read_window, writers, app.plan.write_window,
    });
    for (all[1..]) |l| {
        const thread = try std.Thread.spawn(.{}, Loop.run, .{l});
        thread.detach();
    }
    all[0].run();
}
