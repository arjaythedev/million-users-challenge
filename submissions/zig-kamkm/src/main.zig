const std = @import("std");
const c = @cImport({
    @cInclude("arpa/inet.h");
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
    @cInclude("unistd.h");
});

const Allocator = std.mem.Allocator;
const MAX_REQUEST = 16 * 1024;
const IO_BATCH = 256;
const WRITE_BATCH = 64;
const DB_FEED_SQL = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const DB_POST_SQL = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1";
const DB_INSERT_SQL = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
const DB_LIKE_SQL = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";

const ConnState = enum { open, processing, writing, closed };

const Connection = struct {
    app: *App,
    fd: c_int,
    state: ConnState,
    read_buf: ?[]u8,
    read_len: usize,
    output: ?[]u8,
    output_pos: usize,
    close_after: bool,
    want_gzip: bool,
    worker: ?*Worker,
    request_next: ?*Connection,
    completion_next: ?*Connection,
};

const Worker = struct {
    app: *App,
    index: usize,
    request_mutex: std.Thread.Mutex = .{},
    request_cond: std.Thread.Condition = .{},
    request_head: ?*Connection = null,
    request_tail: ?*Connection = null,
    completion_mutex: std.Thread.Mutex = .{},
    completion_head: ?*Connection = null,
    completion_tail: ?*Connection = null,
    db: ?*c.PGconn = null,
    prepared: bool = false,

    fn dequeue(self: *Worker) *Connection {
        self.request_mutex.lock();
        defer self.request_mutex.unlock();
        while (self.request_head == null) self.request_cond.wait(&self.request_mutex);
        const conn = self.request_head.?;
        self.request_head = conn.request_next;
        if (self.request_head == null) self.request_tail = null;
        conn.request_next = null;
        return conn;
    }

    fn tryDequeue(self: *Worker) ?*Connection {
        self.request_mutex.lock();
        defer self.request_mutex.unlock();
        const conn = self.request_head orelse return null;
        self.request_head = conn.request_next;
        if (self.request_head == null) self.request_tail = null;
        conn.request_next = null;
        return conn;
    }

    fn complete(self: *Worker, conn: *Connection, response: ?[]u8, close_after: bool) void {
        conn.output = response;
        conn.output_pos = 0;
        conn.close_after = close_after;
        conn.completion_next = null;
        self.completion_mutex.lock();
        if (self.completion_tail) |tail| tail.completion_next = conn else self.completion_head = conn;
        self.completion_tail = conn;
        self.completion_mutex.unlock();
        self.app.notify();
    }
};

const App = struct {
    allocator: Allocator,
    epfd: c_int,
    notify_fd: c_int,
    db_url: []u8,
    jwt_secret: []u8,
    workers: []Worker,
    threads: []std.Thread,
    write_worker_count: usize,
    next_read_worker: usize = 0,
    next_write_worker: usize = 0,
    started_ns: i128,

    fn enqueue(self: *App, conn: *Connection) void {
        // Keep writes on a dedicated subset of workers. A burst of likes or
        // posts then cannot occupy every connection serving feed/post reads.
        const read_worker_count = self.workers.len - self.write_worker_count;
        const request = parseHttp(conn.read_buf.?[0..conn.read_len]);
        const route = if (request) |req| routeFor(req.method, req.path) else .unknown;
        conn.want_gzip = if (request) |req| req.accept_gzip else false;
        const is_write = route == .create or route == .like;
        const worker: *Worker = if (is_write and self.write_worker_count > 0) blk: {
            const index = read_worker_count + self.next_write_worker;
            self.next_write_worker = (self.next_write_worker + 1) % self.write_worker_count;
            break :blk &self.workers[index];
        } else blk: {
            const index = self.next_read_worker;
            self.next_read_worker = (self.next_read_worker + 1) % read_worker_count;
            break :blk &self.workers[index];
        };
        conn.worker = worker;
        conn.request_next = null;
        worker.request_mutex.lock();
        if (worker.request_tail) |tail| tail.request_next = conn else worker.request_head = conn;
        worker.request_tail = conn;
        worker.request_cond.signal();
        worker.request_mutex.unlock();
    }

    fn drainCompletions(self: *App) void {
        var junk: u64 = 0;
        while (c.read(self.notify_fd, @ptrCast(&junk), @sizeOf(u64)) > 0) {}
        for (self.workers) |*worker| {
            while (true) {
                worker.completion_mutex.lock();
                const conn = worker.completion_head;
                if (conn) |item| {
                    worker.completion_head = item.completion_next;
                    if (worker.completion_head == null) worker.completion_tail = null;
                }
                worker.completion_mutex.unlock();
                const item = conn orelse break;
                item.completion_next = null;
                if (item.state == .closed) {
                    self.freeConnection(item);
                    continue;
                }
                if (item.output != null) {
                    item.output_pos = 0;
                    item.state = .writing;
                    if (writeConnection(self, item)) {
                        self.modifyConnection(item, c.EPOLLOUT | c.EPOLLONESHOT | c.EPOLLERR | c.EPOLLHUP);
                    }
                } else {
                    // The worker has finished without a response. Mark it as
                    // event-loop-owned before closing so the connection record
                    // is reclaimed instead of being left in .closed state.
                    item.state = .open;
                    self.closeConnection(item);
                }
            }
        }
    }

    fn modifyConnection(self: *App, conn: *Connection, events: u32) void {
        var ev: c.struct_epoll_event = undefined;
        ev.events = events;
        ev.data.ptr = @ptrCast(conn);
        _ = c.epoll_ctl(self.epfd, c.EPOLL_CTL_MOD, conn.fd, &ev);
    }

    fn addConnection(self: *App, conn: *Connection) bool {
        var ev: c.struct_epoll_event = undefined;
        ev.events = c.EPOLLIN | c.EPOLLONESHOT | c.EPOLLERR | c.EPOLLHUP;
        ev.data.ptr = @ptrCast(conn);
        return c.epoll_ctl(self.epfd, c.EPOLL_CTL_ADD, conn.fd, &ev) == 0;
    }

    fn closeConnection(self: *App, conn: *Connection) void {
        if (conn.state == .closed) return;
        _ = c.epoll_ctl(self.epfd, c.EPOLL_CTL_DEL, conn.fd, null);
        _ = c.close(conn.fd);
        if (conn.state == .processing) {
            conn.state = .closed;
            return;
        }
        conn.state = .closed;
        self.freeConnection(conn);
    }

    fn freeConnection(self: *App, conn: *Connection) void {
        if (conn.read_buf) |buf| self.allocator.free(buf);
        if (conn.output) |buf| self.allocator.free(buf);
        self.allocator.destroy(conn);
    }

    fn notify(self: *App) void {
        var one: u64 = 1;
        _ = c.write(self.notify_fd, @ptrCast(&one), @sizeOf(u64));
    }
};

const HttpRequest = struct {
    method: []const u8,
    path: []const u8,
    authorization: ?[]const u8,
    body: []const u8,
    accept_gzip: bool,
};

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
        const trimmed = trimSpace(out[0..body_len]);
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
    const line_end = std.mem.indexOf(u8, raw, "\r\n") orelse return null;
    const line = raw[0..line_end];
    const first = std.mem.indexOfScalar(u8, line, ' ') orelse return null;
    const second_rel = std.mem.indexOfScalar(u8, line[first + 1 ..], ' ') orelse return null;
    const second = first + 1 + second_rel;
    if (second <= first + 1) return null;
    const method = line[0..first];
    const path = line[first + 1 .. second];
    const header_end_rel = std.mem.indexOf(u8, raw, "\r\n\r\n") orelse return null;
    const header_end = header_end_rel + 4;
    var auth: ?[]const u8 = null;
    var accept_gzip = false;
    var cursor = line_end + 2;
    while (cursor < header_end_rel + 2) {
        const end = std.mem.indexOf(u8, raw[cursor .. header_end_rel + 2], "\r\n") orelse return null;
        const absolute = cursor + end;
        const header = raw[cursor..absolute];
        if (std.mem.indexOfScalar(u8, header, ':')) |colon| {
            const name = header[0..colon];
            var value = header[colon + 1 ..];
            while (value.len > 0 and (value[0] == ' ' or value[0] == '\t')) value = value[1..];
            if (std.ascii.eqlIgnoreCase(name, "authorization")) auth = value;
            if (std.ascii.eqlIgnoreCase(name, "accept-encoding")) {
                var enc = value;
                while (enc.len > 0) {
                    const comma = std.mem.indexOfScalar(u8, enc, ',') orelse enc.len;
                    if (std.ascii.eqlIgnoreCase(std.mem.trim(u8, enc[0..comma], " \t"), "gzip")) accept_gzip = true;
                    if (comma == enc.len) break;
                    enc = enc[comma + 1 ..];
                }
            }
        }
        cursor = absolute + 2;
    }
    var content_length: usize = 0;
    cursor = line_end + 2;
    while (cursor < header_end_rel + 2) {
        const end = std.mem.indexOf(u8, raw[cursor .. header_end_rel + 2], "\r\n") orelse return null;
        const absolute = cursor + end;
        const header = raw[cursor..absolute];
        if (std.mem.indexOfScalar(u8, header, ':')) |colon| {
            if (std.ascii.eqlIgnoreCase(header[0..colon], "content-length")) {
                var value = header[colon + 1 ..];
                while (value.len > 0 and (value[0] == ' ' or value[0] == '\t')) value = value[1..];
                content_length = std.fmt.parseInt(usize, value, 10) catch return null;
            }
        }
        cursor = absolute + 2;
    }
    if (header_end + content_length > raw.len) return null;
    return .{ .method = method, .path = path, .authorization = auth, .body = raw[header_end .. header_end + content_length], .accept_gzip = accept_gzip };
}

fn headerComplete(raw: []const u8) ?usize {
    const h = std.mem.indexOf(u8, raw, "\r\n\r\n") orelse return null;
    const header_end = h + 4;
    var content_length: usize = 0;
    var cursor: usize = (std.mem.indexOf(u8, raw, "\r\n") orelse return null) + 2;
    while (cursor < h + 2) {
        const end_rel = std.mem.indexOf(u8, raw[cursor .. h + 2], "\r\n") orelse return null;
        const end = cursor + end_rel;
        const line = raw[cursor..end];
        if (std.mem.indexOfScalar(u8, line, ':')) |colon| {
            if (std.ascii.eqlIgnoreCase(line[0..colon], "content-length")) {
                var value = line[colon + 1 ..];
                while (value.len > 0 and (value[0] == ' ' or value[0] == '\t')) value = value[1..];
                content_length = std.fmt.parseInt(usize, value, 10) catch return null;
            }
        }
        cursor = end + 2;
    }
    return header_end + content_length;
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

fn jsonEscape(out: *std.ArrayList(u8), value: []const u8) !void {
    for (value) |ch| {
        if (ch < 0x20) {
            switch (ch) {
                8 => try out.appendSlice("\\b"),
                9 => try out.appendSlice("\\t"),
                10 => try out.appendSlice("\\n"),
                12 => try out.appendSlice("\\f"),
                13 => try out.appendSlice("\\r"),
                else => {
                    const digits = "0123456789abcdef";
                    try out.appendSlice("\\u00");
                    try out.append(digits[ch >> 4]);
                    try out.append(digits[ch & 0x0f]);
                },
            }
            continue;
        }
        switch (ch) {
            '"' => try out.appendSlice("\\\""),
            '\\' => try out.appendSlice("\\\\"),
            else => try out.append(ch),
        }
    }
}

threadlocal var gzip_buffer: [256 * 1024]u8 = undefined;
threadlocal var gzip_stream: c.struct_isal_zstream = undefined;

fn responseFor(allocator: Allocator, status: u16, reason: []const u8, body: []const u8, close_after: bool, want_gzip: bool) ![]u8 {
    var compressed = false;
    var response_body = body;
    if (want_gzip and body.len >= 1024 and body.len < gzip_buffer.len - 32) {
        c.isal_deflate_stateless_init(&gzip_stream);
        gzip_stream.next_in = @constCast(body.ptr);
        gzip_stream.avail_in = @intCast(body.len);
        gzip_stream.next_out = &gzip_buffer;
        gzip_stream.avail_out = gzip_buffer.len;
        gzip_stream.gzip_flag = c.IGZIP_GZIP;
        gzip_stream.level = 0;
        if (c.isal_deflate_stateless(&gzip_stream) == c.COMP_OK) {
            compressed = true;
            response_body = gzip_buffer[0 .. gzip_buffer.len - gzip_stream.avail_out];
        }
    }
    var out = std.ArrayList(u8).init(allocator);
    errdefer out.deinit();
    try out.writer().print("HTTP/1.1 {d} {s}\r\nContent-Type: application/json\r\n{s}Content-Length: {d}\r\nConnection: {s}\r\n\r\n", .{ status, reason, if (compressed) "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n" else "", response_body.len, if (close_after) "close" else "keep-alive" });
    try out.appendSlice(response_body);
    return out.toOwnedSlice();
}

fn errorResponse(allocator: Allocator, status: u16, message: []const u8, want_gzip: bool) ![]u8 {
    var body = std.ArrayList(u8).init(allocator);
    defer body.deinit();
    try body.appendSlice("{\"error\":\"");
    try jsonEscape(&body, message);
    try body.appendSlice("\"}");
    return responseFor(allocator, status, statusReason(status), body.items, false, want_gzip);
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

fn pgSlice(res: *c.PGresult, row: c_int, col: c_int) []const u8 {
    const ptr = c.PQgetvalue(res, row, col);
    const len = c.PQgetlength(res, row, col);
    return @as([*]const u8, @ptrCast(ptr))[0..@intCast(len)];
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

fn appendPost(out: *std.ArrayList(u8), res: *c.PGresult, row: c_int) !void {
    const id = pgSlice(res, row, 0);
    const body = pgSlice(res, row, 1);
    const created = pgSlice(res, row, 2);
    const author = pgSlice(res, row, 3);
    const likes = pgSlice(res, row, 4);
    try out.appendSlice("{\"id\":");
    try out.appendSlice(id);
    try out.appendSlice(",\"body\":\"");
    try jsonEscape(out, body);
    try out.appendSlice("\",\"created_at\":\"");
    try appendTimestamp(out, created);
    try out.appendSlice("\",\"author\":\"");
    try jsonEscape(out, author);
    try out.appendSlice("\",\"like_count\":");
    try out.appendSlice(likes);
    try out.append('}');
}

fn preparedExec(db: *c.PGconn, name: []const u8, values: []const []const u8, lengths: []const c_int) ?*c.PGresult {
    // libpq treats text parameters as NUL-terminated strings; paramLengths is
    // only meaningful for binary parameters. Copy each value into a bounded
    // scratch buffer and terminate it explicitly so IDs and post bodies never
    // depend on incidental zero bytes in stack memory.
    if (values.len > 2 or lengths.len < values.len) return null;
    var scratch: [2][MAX_REQUEST + 1]u8 = undefined;
    var ptrs: [2][*c]const u8 = undefined;
    var lens: [2]c_int = undefined;
    for (values, 0..) |value, i| {
        if (value.len > MAX_REQUEST) return null;
        std.mem.copyForwards(u8, scratch[i][0..value.len], value);
        scratch[i][value.len] = 0;
        ptrs[i] = @ptrCast(&scratch[i]);
        lens[i] = lengths[i];
    }
    return c.PQexecPrepared(db, @ptrCast(name.ptr), @intCast(values.len), @ptrCast(&ptrs), @ptrCast(&lens), null, 0);
}

fn dbConnection(worker: *Worker) ?*c.PGconn {
    if (worker.db) |conn| {
        if (c.PQstatus(conn) == c.CONNECTION_OK) return conn;
        c.PQfinish(conn);
        worker.db = null;
        worker.prepared = false;
    }
    const conn = c.PQconnectdb(worker.app.db_url.ptr);
    if (conn == null or c.PQstatus(conn) != c.CONNECTION_OK) {
        if (conn) |bad| c.PQfinish(bad);
        worker.db = null;
        return null;
    }
    const set_result = c.PQexec(conn, "SET TIME ZONE 'UTC'; SET jit = off");
    if (set_result == null or c.PQresultStatus(set_result) != c.PGRES_COMMAND_OK) {
        if (set_result) |r| c.PQclear(r);
        c.PQfinish(conn);
        worker.db = null;
        return null;
    }
    c.PQclear(set_result);
    const statements = [_]struct { name: []const u8, sql: []const u8, params: c_int }{
        .{ .name = "feed", .sql = DB_FEED_SQL, .params = 0 },
        .{ .name = "post", .sql = DB_POST_SQL, .params = 1 },
        .{ .name = "insert_post", .sql = DB_INSERT_SQL, .params = 2 },
        .{ .name = "like_post", .sql = DB_LIKE_SQL, .params = 2 },
    };
    for (statements) |statement| {
        const result = c.PQprepare(conn, @ptrCast(statement.name.ptr), @ptrCast(statement.sql.ptr), statement.params, null);
        if (result == null or c.PQresultStatus(result) != c.PGRES_COMMAND_OK) {
            if (result) |r| c.PQclear(r);
            c.PQfinish(conn);
            worker.db = null;
            worker.prepared = false;
            return null;
        }
        c.PQclear(result);
    }
    worker.prepared = true;
    worker.db = conn;
    return conn;
}

fn processRequest(app: *App, worker: *Worker, conn: *Connection, loaded: ?*c.PGresult) ![]u8 {
    defer if (loaded) |result| c.PQclear(result);
    const raw = conn.read_buf.?[0..conn.read_len];
    const req = parseHttp(raw) orelse return error.MalformedRequest;
    const route = routeFor(req.method, req.path);
    if (route == .unknown) return error.NotFound;
    var auth_username: [4096]u8 = undefined;
    var auth: ?Auth = null;
    if (route == .create or route == .like) {
        auth = authenticate(app, req.authorization, &auth_username) catch |e| switch (e) {
            error.MissingBearer => return error.MissingBearer,
            error.InvalidPayload => return error.InvalidPayload,
            else => return error.InvalidToken,
        };
    }
    if (route == .health) {
        const connection = dbConnection(worker) orelse {
            var msg = std.ArrayList(u8).init(app.allocator);
            defer msg.deinit();
            try msg.appendSlice("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
            if (worker.db) |bad| try jsonEscape(&msg, std.mem.span(c.PQerrorMessage(bad))) else try msg.appendSlice("database unavailable");
            try msg.appendSlice("\"}");
            return responseFor(app.allocator, 503, "Service Unavailable", msg.items, false, req.accept_gzip);
        };
        const res = c.PQexec(connection, "SELECT 1");
        if (res == null or c.PQresultStatus(res) != c.PGRES_TUPLES_OK) {
            var msg = std.ArrayList(u8).init(app.allocator);
            defer msg.deinit();
            try msg.appendSlice("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
            if (res) |r| {
                try jsonEscape(&msg, std.mem.span(c.PQresultErrorMessage(r)));
                c.PQclear(r);
            } else try msg.appendSlice("database unavailable");
            try msg.appendSlice("\"}");
            return responseFor(app.allocator, 503, "Service Unavailable", msg.items, false, req.accept_gzip);
        }
        c.PQclear(res);
        const uptime: u64 = @intCast(@divTrunc(std.time.nanoTimestamp() - app.started_ns, std.time.ns_per_s));
        var body = std.ArrayList(u8).init(app.allocator);
        defer body.deinit();
        try body.writer().print("{{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":{d}}}", .{uptime});
        return responseFor(app.allocator, 200, "OK", body.items, false, req.accept_gzip);
    }
    switch (route) {
        .feed => {
            const connection = dbConnection(worker) orelse return error.Internal;
            const res = loaded orelse preparedExec(connection, "feed", &[_][]const u8{}, &[_]c_int{}) orelse return error.Internal;
            if (c.PQresultStatus(res) != c.PGRES_TUPLES_OK) {
                if (loaded == null) c.PQclear(res);
                return error.Internal;
            }
            const result = res;
            defer if (loaded == null) c.PQclear(result);
            var body = std.ArrayList(u8).init(app.allocator);
            defer body.deinit();
            try body.appendSlice("{\"posts\":[");
            const rows = c.PQntuples(result);
            var i: c_int = 0;
            while (i < rows) : (i += 1) {
                if (i > 0) try body.append(',');
                try appendPost(&body, result, i);
            }
            try body.appendSlice("]}");
            return responseFor(app.allocator, 200, "OK", body.items, false, req.accept_gzip);
        },
        .post => {
            const id = postIdFromPath(req.path, false) orelse return error.InvalidId;
            var id_buf: [32]u8 = undefined;
            const id_text = std.fmt.bufPrint(&id_buf, "{d}", .{id}) catch return error.Internal;
            const values = [_][]const u8{id_text};
            const lengths = [_]c_int{@intCast(id_text.len)};
            const connection = dbConnection(worker) orelse return error.Internal;
            const res = loaded orelse preparedExec(connection, "post", &values, &lengths) orelse return error.Internal;
            const result = res;
            defer if (loaded == null) c.PQclear(result);
            if (c.PQresultStatus(result) != c.PGRES_TUPLES_OK) return error.Internal;
            if (c.PQntuples(result) == 0) return error.PostNotFound;
            var body = std.ArrayList(u8).init(app.allocator);
            defer body.deinit();
            try body.appendSlice("{\"post\":");
            try appendPost(&body, result, 0);
            try body.append('}');
            return responseFor(app.allocator, 200, "OK", body.items, false, req.accept_gzip);
        },
        .create => {
            var parsed_body = app.allocator.alloc(u8, req.body.len + 1) catch return error.Internal;
            defer app.allocator.free(parsed_body);
            var parser = JsonParser{ .data = req.body };
            const body_status = parser.parseBody(parsed_body);
            if (body_status == .malformed) return error.MalformedJson;
            if (body_status == .required) return error.BodyRequired;
            if (body_status == .too_long) return error.BodyTooLong;
            const body_slice = parsed_body[0..parser.parsed_len];
            var user_buf: [32]u8 = undefined;
            const user_text = std.fmt.bufPrint(&user_buf, "{d}", .{auth.?.user_id}) catch return error.Internal;
            const values = [_][]const u8{ user_text, body_slice };
            const lengths = [_]c_int{ @intCast(user_text.len), @intCast(body_slice.len) };
            const connection = dbConnection(worker) orelse return error.Internal;
            const res = loaded orelse preparedExec(connection, "insert_post", &values, &lengths) orelse return error.Internal;
            defer if (loaded == null) c.PQclear(res);
            if (c.PQresultStatus(res) != c.PGRES_TUPLES_OK or c.PQntuples(res) == 0) return error.Internal;
            const new_id = pgSlice(res, 0, 0);
            const created = pgSlice(res, 0, 1);
            var body = std.ArrayList(u8).init(app.allocator);
            defer body.deinit();
            try body.appendSlice("{\"post\":{\"id\":");
            try body.appendSlice(new_id);
            try body.appendSlice(",\"body\":\"");
            try jsonEscape(&body, body_slice);
            try body.appendSlice("\",\"created_at\":\"");
            try appendTimestamp(&body, created);
            try body.appendSlice("\",\"author\":\"");
            try jsonEscape(&body, auth.?.username);
            try body.appendSlice("\",\"like_count\":0}}");
            return responseFor(app.allocator, 201, "Created", body.items, false, req.accept_gzip);
        },
        .like => {
            const id = postIdFromPath(req.path, true) orelse return error.InvalidId;
            var user_buf: [32]u8 = undefined;
            var post_buf: [32]u8 = undefined;
            const user_text = std.fmt.bufPrint(&user_buf, "{d}", .{auth.?.user_id}) catch return error.Internal;
            const post_text = std.fmt.bufPrint(&post_buf, "{d}", .{id}) catch return error.Internal;
            const values = [_][]const u8{ user_text, post_text };
            const lengths = [_]c_int{ @intCast(user_text.len), @intCast(post_text.len) };
            const connection = dbConnection(worker) orelse return error.Internal;
            const res = loaded orelse preparedExec(connection, "like_post", &values, &lengths) orelse return error.Internal;
            defer if (loaded == null) c.PQclear(res);
            if (c.PQresultStatus(res) != c.PGRES_COMMAND_OK) {
                const state_ptr = c.PQresultErrorField(res, c.PG_DIAG_SQLSTATE);
                const constraint_ptr = c.PQresultErrorField(res, c.PG_DIAG_CONSTRAINT_NAME);
                if (state_ptr != null and constraint_ptr != null and std.mem.eql(u8, std.mem.span(state_ptr), "23503") and std.mem.endsWith(u8, std.mem.span(constraint_ptr), "likes_post_id_fkey")) return error.PostNotFound;
                return error.Internal;
            }
            const inserted = std.mem.eql(u8, std.mem.span(c.PQcmdTuples(res)), "1");
            var body = std.ArrayList(u8).init(app.allocator);
            defer body.deinit();
            try body.writer().print("{{\"liked\":true,\"already_liked\":{s},\"post_id\":{d}}}", .{ if (inserted) "false" else "true", id });
            return responseFor(app.allocator, if (inserted) 201 else 200, if (inserted) "Created" else "OK", body.items, false, req.accept_gzip);
        },
        else => return error.Internal,
    }
}

fn makeErrorFor(app: *App, err: anyerror, want_gzip: bool) ![]u8 {
    return switch (err) {
        error.NotFound => errorResponse(app.allocator, 404, "not found", want_gzip),
        error.InvalidId => errorResponse(app.allocator, 400, "invalid post id", want_gzip),
        error.PostNotFound => errorResponse(app.allocator, 404, "post not found", want_gzip),
        error.MissingBearer => errorResponse(app.allocator, 401, "missing bearer token", want_gzip),
        error.InvalidToken, error.InvalidPayload => errorResponse(app.allocator, 401, if (err == error.InvalidPayload) "invalid token payload" else "invalid or expired token", want_gzip),
        error.MalformedJson => errorResponse(app.allocator, 400, "malformed JSON body", want_gzip),
        error.BodyRequired => errorResponse(app.allocator, 400, "body is required", want_gzip),
        error.BodyTooLong => errorResponse(app.allocator, 400, "body must be at most 500 characters", want_gzip),
        error.MalformedRequest => errorResponse(app.allocator, 400, "malformed JSON body", want_gzip),
        else => errorResponse(app.allocator, 500, "internal server error", want_gzip),
    };
}

fn requestRoute(conn: *Connection) Route {
    const raw = conn.read_buf orelse return .unknown;
    const req = parseHttp(raw[0..conn.read_len]) orelse return .unknown;
    return routeFor(req.method, req.path);
}

fn completeSingle(worker: *Worker, conn: *Connection) void {
    const response = processRequest(worker.app, worker, conn, null) catch |err| makeErrorFor(worker.app, err, conn.want_gzip) catch null;
    worker.complete(conn, response, false);
}

fn runTransactionCommand(db: *c.PGconn, command: []const u8) bool {
    const result = c.PQexec(db, @ptrCast(command.ptr));
    if (result == null) return false;
    const ok = c.PQresultStatus(result) == c.PGRES_COMMAND_OK;
    c.PQclear(result);
    return ok;
}

// libpq copies text parameters into its output buffer before returning.
fn sendWrite(worker: *Worker, db: *c.PGconn, conn: *Connection) !bool {
    const req = parseHttp(conn.read_buf.?[0..conn.read_len]) orelse return false;
    var username: [4096]u8 = undefined;
    const auth = authenticate(worker.app, req.authorization, &username) catch return false;
    var user_buf: [32]u8 = undefined;
    const user = try std.fmt.bufPrintZ(&user_buf, "{d}", .{auth.user_id});
    var second: [MAX_REQUEST + 1]u8 = undefined;
    const route = routeFor(req.method, req.path);
    if (route == .create) {
        var parser = JsonParser{ .data = req.body };
        if (parser.parseBody(&second) != .ok) return false;
        second[parser.parsed_len] = 0;
    } else if (route == .like) {
        const id = postIdFromPath(req.path, true) orelse return false;
        _ = try std.fmt.bufPrintZ(&second, "{d}", .{id});
    } else return false;
    const values = [_][*c]const u8{ user.ptr, &second };
    const name: [*:0]const u8 = if (route == .create) "insert_post" else "like_post";
    if (c.PQsendQueryPrepared(db, name, 2, &values, null, null, 0) != 1) return error.Internal;
    return true;
}

fn processWriteBatch(worker: *Worker, first: *Connection) void {
    var batch: [WRITE_BATCH]*Connection = undefined;
    var responses = [_]?[]u8{null} ** WRITE_BATCH;
    var results = [_]?*c.PGresult{null} ** WRITE_BATCH;
    var queued = [_]bool{false} ** WRITE_BATCH;
    batch[0] = first;
    var count: usize = 1;
    while (count < WRITE_BATCH) {
        batch[count] = worker.tryDequeue() orelse break;
        count += 1;
    }
    if (count == 1) {
        completeSingle(worker, first);
        return;
    }
    const db = dbConnection(worker) orelse {
        for (batch[0..count]) |conn| completeSingle(worker, conn);
        return;
    };
    if (!runTransactionCommand(db, "BEGIN")) {
        for (batch[0..count]) |conn| completeSingle(worker, conn);
        return;
    }
    var ok = c.PQenterPipelineMode(db) == 1;
    if (ok) for (batch[0..count], 0..) |conn, i| {
        queued[i] = sendWrite(worker, db, conn) catch {
            ok = false;
            break;
        };
    };
    if (ok) ok = c.PQpipelineSync(db) == 1;
    if (ok) for (queued[0..count], 0..) |sent, i| {
        if (!sent) continue;
        results[i] = c.PQgetResult(db);
        if (results[i] == null) {
            ok = false;
            break;
        }
        if (c.PQgetResult(db)) |extra| {
            c.PQclear(extra);
            ok = false;
            break;
        }
    };
    if (ok) {
        const sync = c.PQgetResult(db);
        ok = sync != null and c.PQresultStatus(sync) == c.PGRES_PIPELINE_SYNC;
        if (sync) |result| c.PQclear(result);
        if (c.PQgetResult(db)) |extra| {
            c.PQclear(extra);
            ok = false;
        }
        if (ok) ok = c.PQexitPipelineMode(db) == 1;
    }
    if (!ok) {
        // COMMIT was never sent. Closing the connection rolls the group back.
        for (results[0..count]) |result| if (result) |r| c.PQclear(r);
        c.PQfinish(db);
        worker.db = null;
        worker.prepared = false;
        for (batch[0..count]) |conn| completeSingle(worker, conn);
        return;
    }
    var failed = false;
    for (batch[0..count], 0..) |conn, i| {
        if (results[i]) |result| {
            results[i] = null;
            responses[i] = processRequest(worker.app, worker, conn, result) catch {
                failed = true;
                break;
            };
        }
    }
    for (results[0..count]) |result| if (result) |r| c.PQclear(r);
    if (failed) {
        for (responses[0..count]) |response| if (response) |buf| worker.app.allocator.free(buf);
        if (!runTransactionCommand(db, "ROLLBACK")) {
            c.PQfinish(db);
            worker.db = null;
            worker.prepared = false;
        }
        for (batch[0..count]) |conn| completeSingle(worker, conn);
        return;
    }
    const committed = runTransactionCommand(db, "COMMIT");
    for (batch[0..count], 0..) |conn, i| {
        if (!committed and queued[i]) {
            if (responses[i]) |buf| worker.app.allocator.free(buf);
            // A lost COMMIT acknowledgement is ambiguous. Never replay it.
            worker.complete(conn, makeErrorFor(worker.app, error.Internal, conn.want_gzip) catch null, false);
        } else if (responses[i]) |response| worker.complete(conn, response, false) else completeSingle(worker, conn);
    }
}

// Every queued request sends its own prepared statement. One Sync terminates
// the read-only group, amortizing protocol flushes without caching any data.
fn processReadBatch(worker: *Worker, first: *Connection) void {
    var batch: [64]*Connection = undefined;
    var results = [_]?*c.PGresult{null} ** 64;
    var queued = [_]bool{false} ** 64;
    batch[0] = first;
    var count: usize = 1;
    while (count < batch.len) {
        batch[count] = worker.tryDequeue() orelse break;
        count += 1;
    }
    if (count == 1) {
        completeSingle(worker, first);
        return;
    }
    const db = dbConnection(worker) orelse {
        for (batch[0..count]) |conn| completeSingle(worker, conn);
        return;
    };
    var ok = c.PQenterPipelineMode(db) == 1;
    if (ok) {
        for (batch[0..count], 0..) |conn, i| {
            const req = parseHttp(conn.read_buf.?[0..conn.read_len]) orelse continue;
            switch (routeFor(req.method, req.path)) {
                .feed => {
                    queued[i] = true;
                    if (c.PQsendQueryPrepared(db, "feed", 0, null, null, null, 0) != 1) {
                        ok = false;
                        break;
                    }
                },
                .post => {
                    const id = postIdFromPath(req.path, false) orelse continue;
                    var text: [32]u8 = undefined;
                    const value = std.fmt.bufPrintZ(&text, "{d}", .{id}) catch unreachable;
                    const values = [_][*c]const u8{value.ptr};
                    queued[i] = true;
                    if (c.PQsendQueryPrepared(db, "post", 1, &values, null, null, 0) != 1) {
                        ok = false;
                        break;
                    }
                },
                else => {},
            }
        }
    }
    if (ok) ok = c.PQpipelineSync(db) == 1;
    if (ok) {
        for (queued[0..count], 0..) |sent, i| {
            if (!sent) continue;
            results[i] = c.PQgetResult(db);
            if (results[i] == null) {
                ok = false;
                break;
            }
            if (c.PQgetResult(db)) |extra| {
                c.PQclear(extra);
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        const sync = c.PQgetResult(db);
        ok = sync != null and c.PQresultStatus(sync) == c.PGRES_PIPELINE_SYNC;
        if (sync) |result| c.PQclear(result);
        if (c.PQgetResult(db)) |extra| {
            c.PQclear(extra);
            ok = false;
        }
        if (ok) ok = c.PQexitPipelineMode(db) == 1;
    }
    if (!ok) {
        for (results[0..count]) |result| if (result) |r| c.PQclear(r);
        c.PQfinish(db);
        worker.db = null;
        worker.prepared = false;
        // Retry only reads; none of these statements can duplicate a write.
        for (batch[0..count]) |conn| completeSingle(worker, conn);
        return;
    }
    for (batch[0..count], 0..) |conn, i| {
        if (results[i]) |result| {
            const response = processRequest(worker.app, worker, conn, result) catch |err| makeErrorFor(worker.app, err, conn.want_gzip) catch null;
            worker.complete(conn, response, false);
        } else completeSingle(worker, conn);
    }
}

fn workerMain(worker: *Worker) void {
    while (true) {
        const conn = worker.dequeue();
        const write_start = worker.app.workers.len - worker.app.write_worker_count;
        const is_write_worker = worker.app.write_worker_count > 0 and worker.index >= write_start;
        const route = requestRoute(conn);
        if (is_write_worker and (route == .create or route == .like)) {
            processWriteBatch(worker, conn);
        } else if (!is_write_worker and (route == .feed or route == .post)) {
            processReadBatch(worker, conn);
        } else {
            completeSingle(worker, conn);
        }
    }
}

fn setNonBlocking(fd: c_int) !void {
    const flags = c.fcntl(fd, c.F_GETFL, 0);
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
        // Keep the accept queue useful during the million-connection ramp.
        // The kernel may clamp this to net.core.somaxconn.
        const backlog: c_int = 262144;
        if (c.bind(fd, entry.ai_addr, entry.ai_addrlen) == 0 and c.listen(fd, backlog) == 0) return fd;
        _ = c.close(fd);
    }
    return error.Bind;
}

fn acceptConnections(app: *App, listener: c_int) void {
    while (true) {
        const fd = c.accept(listener, null, null);
        if (fd < 0) {
            if (c.__errno_location().* == c.EAGAIN or c.__errno_location().* == c.EWOULDBLOCK) return;
            return;
        }
        if (fd >= 0) {
            _ = c.fcntl(fd, c.F_SETFL, c.fcntl(fd, c.F_GETFL, @as(c_int, 0)) | c.O_NONBLOCK);
            _ = c.fcntl(fd, c.F_SETFD, c.fcntl(fd, c.F_GETFD, @as(c_int, 0)) | c.FD_CLOEXEC);
            var yes: c_int = 1;
            _ = c.setsockopt(fd, c.IPPROTO_TCP, c.TCP_NODELAY, @ptrCast(&yes), @sizeOf(c_int));
            _ = c.setsockopt(fd, c.SOL_SOCKET, c.SO_KEEPALIVE, @ptrCast(&yes), @sizeOf(c_int));
        }
        const conn = app.allocator.create(Connection) catch {
            _ = c.close(fd);
            continue;
        };
        conn.* = .{ .app = app, .fd = fd, .state = .open, .read_buf = null, .read_len = 0, .output = null, .output_pos = 0, .close_after = false, .want_gzip = false, .worker = null, .request_next = null, .completion_next = null };
        if (!app.addConnection(conn)) {
            _ = c.close(fd);
            app.allocator.destroy(conn);
            continue;
        }
    }
}

fn dispatchIfComplete(app: *App, conn: *Connection) void {
    const raw = conn.read_buf.?[0..conn.read_len];
    if (headerComplete(raw)) |needed| {
        if (needed <= raw.len) {
            conn.state = .processing;
            app.enqueue(conn);
        }
    }
}

fn readConnection(app: *App, conn: *Connection) void {
    if (conn.state != .open) return;
    if (conn.read_buf == null) conn.read_buf = app.allocator.alloc(u8, 512) catch {
        app.closeConnection(conn);
        return;
    };
    while (true) {
        if (conn.read_len == conn.read_buf.?.len) {
            const old = conn.read_buf.?;
            if (old.len == MAX_REQUEST) {
                app.closeConnection(conn);
                return;
            }
            conn.read_buf = app.allocator.realloc(old, @min(old.len * 2, MAX_REQUEST)) catch {
                app.closeConnection(conn);
                return;
            };
        }
        const buf = conn.read_buf.?;
        const n = c.read(conn.fd, @ptrCast(buf.ptr + conn.read_len), buf.len - conn.read_len);
        if (n > 0) {
            conn.read_len += @intCast(n);
            dispatchIfComplete(app, conn);
            if (conn.state != .open) return;
            continue;
        }
        if (n == 0) {
            app.closeConnection(conn);
            return;
        }
        if (c.__errno_location().* == c.EAGAIN or c.__errno_location().* == c.EWOULDBLOCK) break;
        app.closeConnection(conn);
        return;
    }
    app.modifyConnection(conn, c.EPOLLIN | c.EPOLLONESHOT | c.EPOLLERR | c.EPOLLHUP);
}

// Return true only when a partial send needs another writable event. A false
// result may have freed conn, so callers must not inspect it afterward.
fn writeConnection(app: *App, conn: *Connection) bool {
    if (conn.state != .writing) return false;
    const buf = conn.output orelse {
        app.closeConnection(conn);
        return false;
    };
    while (conn.output_pos < buf.len) {
        const n = c.send(conn.fd, @ptrCast(buf.ptr + conn.output_pos), buf.len - conn.output_pos, c.MSG_NOSIGNAL);
        if (n > 0) {
            conn.output_pos += @intCast(n);
            continue;
        }
        if (n < 0 and (c.__errno_location().* == c.EAGAIN or c.__errno_location().* == c.EWOULDBLOCK)) return true;
        app.closeConnection(conn);
        return false;
    }
    app.allocator.free(buf);
    conn.output = null;
    conn.output_pos = 0;
    if (conn.read_buf) |inbuf| {
        app.allocator.free(inbuf);
        conn.read_buf = null;
    }
    conn.read_len = 0;
    if (conn.close_after) {
        app.closeConnection(conn);
        return false;
    }
    conn.state = .open;
    app.modifyConnection(conn, c.EPOLLIN | c.EPOLLONESHOT | c.EPOLLERR | c.EPOLLHUP);
    return false;
}

fn runLoop(app: *App, listeners: []const c_int) !void {
    var events: [IO_BATCH]c.struct_epoll_event = undefined;
    while (true) {
        const n = c.epoll_wait(app.epfd, &events, IO_BATCH, 1000);
        if (n < 0) {
            if (c.__errno_location().* == c.EINTR) continue;
            return error.Epoll;
        }
        var i: c_int = 0;
        var completions_ready = false;
        while (i < n) : (i += 1) {
            const ev = events[@intCast(i)];
            const tag = ev.data.u64;
            if (tag == @as(u64, @intCast(app.notify_fd))) {
                completions_ready = true;
                continue;
            }
            var is_listener = false;
            for (listeners) |listener| if (tag == @as(u64, @intCast(listener))) {
                acceptConnections(app, listener);
                is_listener = true;
                break;
            };
            if (is_listener) continue;
            const conn: *Connection = @ptrCast(@alignCast(ev.data.ptr));
            if ((ev.events & (c.EPOLLERR | c.EPOLLHUP)) != 0) {
                app.closeConnection(conn);
                continue;
            }
            if ((ev.events & c.EPOLLIN) != 0) readConnection(app, conn);
            if ((ev.events & c.EPOLLOUT) != 0) {
                if (writeConnection(app, conn)) {
                    app.modifyConnection(conn, c.EPOLLOUT | c.EPOLLONESHOT | c.EPOLLERR | c.EPOLLHUP);
                }
            }
        }
        // Process socket hangups before completions can send and free their
        // connection records. The current event batch may still reference them.
        if (completions_ready) app.drainCompletions();
    }
}

fn envOr(name: []const u8, fallback: []const u8) []const u8 {
    return std.posix.getenv(name) orelse fallback;
}

pub fn main() !void {
    const allocator = std.heap.c_allocator;
    const db_raw = std.posix.getenv("DATABASE_URL") orelse {
        std.log.err("DATABASE_URL is required", .{});
        return error.Configuration;
    };
    const secret_raw = envOr("JWT_SECRET", "million-users-challenge");
    const db_url = try allocator.alloc(u8, db_raw.len + 1);
    std.mem.copyForwards(u8, db_url[0..db_raw.len], db_raw);
    db_url[db_raw.len] = 0;
    const secret = try allocator.alloc(u8, secret_raw.len);
    std.mem.copyForwards(u8, secret, secret_raw);
    const host = envOr("HOST", "127.0.0.1");
    const port = std.fmt.parseInt(u16, envOr("PORT", "3000"), 10) catch return error.Configuration;
    const extra_raw = std.posix.getenv("EXTRA_PORT");
    const extra_port: ?u16 = if (extra_raw) |value| if (value.len > 0) std.fmt.parseInt(u16, value, 10) catch return error.Configuration else null else null;
    // Split the total connection budget across the HTTP loops. DB_POOL can
    // override the default when the database has a different connection limit.
    var workers: usize = 16;
    if (std.posix.getenv("DB_POOL")) |value| workers = std.fmt.parseInt(usize, value, 10) catch return error.Configuration;
    if (workers == 0 or workers > 512) return error.Configuration;

    var loops: usize = @min(@as(usize, 2), @max(@as(usize, 1), workers / 2));
    if (std.posix.getenv("HTTP_LOOPS")) |value| loops = std.fmt.parseInt(usize, value, 10) catch return error.Configuration;
    if (loops == 0 or loops > @max(@as(usize, 1), workers / 2)) return error.Configuration;
    var writes: usize = if (workers > 1) @max(loops, workers / 8) else 0;
    if (std.posix.getenv("WRITE_WORKERS")) |value| writes = std.fmt.parseInt(usize, value, 10) catch return error.Configuration;
    if (writes >= workers or (writes != 0 and writes < loops)) return error.Configuration;
    for (0..loops) |i| {
        const count = workers / loops + @intFromBool(i < workers % loops);
        const write_count = writes / loops + @intFromBool(i < writes % loops);
        if (write_count >= count) return error.Configuration;
        const config = ServerConfig{ .allocator = allocator, .db_url = db_url, .secret = secret, .host = host, .port = port, .extra_port = extra_port, .workers = count, .writes = write_count };
        if (i + 1 == loops) try runServer(config) else {
            const thread = try std.Thread.spawn(.{}, serverThread, .{config});
            thread.detach();
        }
    }
}

const ServerConfig = struct {
    allocator: Allocator,
    db_url: []u8,
    secret: []u8,
    host: []const u8,
    port: u16,
    extra_port: ?u16,
    workers: usize,
    writes: usize,
};

fn serverThread(config: ServerConfig) void {
    runServer(config) catch |err| {
        std.log.err("HTTP loop failed: {}", .{err});
        std.process.exit(1);
    };
}

fn runServer(config: ServerConfig) !void {
    const allocator = config.allocator;
    const db_url = config.db_url;
    const secret = config.secret;
    const host = config.host;
    const port = config.port;
    const extra_port = config.extra_port;
    const workers = config.workers;
    const write_worker_count = config.writes;
    const epfd = c.epoll_create1(c.EPOLL_CLOEXEC);
    if (epfd < 0) return error.Epoll;
    const notify = c.eventfd(0, c.EFD_NONBLOCK | c.EFD_CLOEXEC);
    if (notify < 0) return error.Event;
    var app = App{ .allocator = allocator, .epfd = epfd, .notify_fd = notify, .db_url = db_url, .jwt_secret = secret, .workers = &[_]Worker{}, .threads = &[_]std.Thread{}, .write_worker_count = write_worker_count, .started_ns = std.time.nanoTimestamp() };
    app.workers = try allocator.alloc(Worker, workers);
    app.threads = try allocator.alloc(std.Thread, workers);
    for (app.workers, 0..) |*worker, index| worker.* = .{ .app = &app, .index = index };
    for (app.workers, 0..) |*worker, index| app.threads[index] = std.Thread.spawn(.{}, workerMain, .{worker}) catch return error.Thread;
    var listeners: [2]c_int = undefined;
    var listener_count: usize = 1;
    listeners[0] = try makeListener(host, port, allocator);
    if (extra_port) |p| {
        listeners[1] = try makeListener(host, p, allocator);
        listener_count = 2;
    }
    var listener_events: [2]c.struct_epoll_event = undefined;
    for (listeners[0..listener_count], 0..) |fd, idx| {
        listener_events[idx].events = c.EPOLLIN | c.EPOLLERR;
        listener_events[idx].data.u64 = @intCast(fd);
        if (c.epoll_ctl(epfd, c.EPOLL_CTL_ADD, fd, &listener_events[idx]) != 0) return error.Epoll;
    }
    var notify_event: c.struct_epoll_event = undefined;
    notify_event.events = c.EPOLLIN;
    notify_event.data.u64 = @intCast(notify);
    if (c.epoll_ctl(epfd, c.EPOLL_CTL_ADD, notify, &notify_event) != 0) return error.Epoll;
    std.log.info("zig feed server listening on {s}:{d}{s}", .{ host, port, if (extra_port != null) " and extra port" else "" });
    try runLoop(&app, listeners[0..listener_count]);
}
