//! HTTP framing shared by the event loop and request workers.
const std = @import("std");
pub const max_request = 16 * 1024;
pub const ParseError = error{ MalformedRequest, RequestTooLarge };

pub const Head = struct {
    method: []const u8,
    path: []const u8,
    authorization: ?[]const u8 = null,
    accept_gzip: bool = false,
    header_len: usize,
    content_length: usize = 0,
    chunked: bool = false,
    expect_continue: bool = false,
    close_after: bool = false,
};

pub const Request = struct {
    method: []const u8,
    path: []const u8,
    authorization: ?[]const u8,
    accept_gzip: bool,
    body: []const u8,
};

fn tokenByte(ch: u8) bool {
    return std.ascii.isAlphanumeric(ch) or ch == 0x60 or
        std.mem.indexOfScalar(u8, "!#$%&'*+-.^_|~", ch) != null;
}

fn validValue(value: []const u8) bool {
    for (value) |ch| if ((ch < 0x20 and ch != '\t') or ch == 0x7f) return false;
    return true;
}

fn gzipAccepted(value: []const u8) bool {
    var items = std.mem.splitScalar(u8, value, ',');
    while (items.next()) |item| {
        var parameters = std.mem.splitScalar(u8, item, ';');
        if (!std.ascii.eqlIgnoreCase(std.mem.trim(u8, parameters.next().?, " \t"), "gzip")) continue;
        while (parameters.next()) |parameter| {
            const equal = std.mem.indexOfScalar(u8, parameter, '=') orelse continue;
            if (std.ascii.eqlIgnoreCase(std.mem.trim(u8, parameter[0..equal], " \t"), "q")) {
                const quality = std.fmt.parseFloat(f64, std.mem.trim(u8, parameter[equal + 1 ..], " \t")) catch return false;
                if (!(quality > 0)) return false;
            }
        }
        return true;
    }
    return false;
}

pub fn parseHead(raw: []const u8) ParseError!?Head {
    const header_end = std.mem.indexOf(u8, raw, "\r\n\r\n") orelse return null;
    if (header_end + 4 > max_request) return error.RequestTooLarge;
    const line_end = std.mem.indexOf(u8, raw[0 .. header_end + 2], "\r\n") orelse return error.MalformedRequest;
    const line = raw[0..line_end];
    const first = std.mem.indexOfScalar(u8, line, ' ') orelse return error.MalformedRequest;
    const second_relative = std.mem.indexOfScalar(u8, line[first + 1 ..], ' ') orelse return error.MalformedRequest;
    const second = first + 1 + second_relative;
    if (first == 0 or second == first + 1) return error.MalformedRequest;
    for (line[0..first]) |ch| if (!tokenByte(ch)) return error.MalformedRequest;
    const version = line[second + 1 ..];
    const old_http = std.mem.eql(u8, version, "HTTP/1.0");
    if (!old_http and !std.mem.eql(u8, version, "HTTP/1.1")) return error.MalformedRequest;
    const target = line[first + 1 .. second];
    if (target[0] != '/') return error.MalformedRequest;
    for (target) |ch| if (ch <= 0x20 or ch == 0x7f or ch == '#') return error.MalformedRequest;
    const query = std.mem.indexOfScalar(u8, target, '?') orelse target.len;
    var head = Head{ .method = line[0..first], .path = target[0..query], .header_len = header_end + 4 };
    var host_seen = false;
    var length_seen = false;
    var encoding_seen = false;
    var connection_close = false;
    var keep_alive = false;
    var cursor = line_end + 2;
    while (cursor < header_end + 2) {
        const relative = std.mem.indexOf(u8, raw[cursor .. header_end + 2], "\r\n") orelse return error.MalformedRequest;
        const end = cursor + relative;
        const header = raw[cursor..end];
        const colon = std.mem.indexOfScalar(u8, header, ':') orelse return error.MalformedRequest;
        if (colon == 0) return error.MalformedRequest;
        const name = header[0..colon];
        for (name) |ch| if (!tokenByte(ch)) return error.MalformedRequest;
        const value = std.mem.trim(u8, header[colon + 1 ..], " \t");
        if (!validValue(value)) return error.MalformedRequest;
        if (std.ascii.eqlIgnoreCase(name, "host")) {
            if (host_seen) return error.MalformedRequest;
            host_seen = true;
        } else if (std.ascii.eqlIgnoreCase(name, "content-length")) {
            if (value.len == 0) return error.MalformedRequest;
            for (value) |ch| if (!std.ascii.isDigit(ch)) return error.MalformedRequest;
            const length = std.fmt.parseInt(usize, value, 10) catch return error.MalformedRequest;
            if (length_seen and length != head.content_length) return error.MalformedRequest;
            head.content_length = length;
            length_seen = true;
        } else if (std.ascii.eqlIgnoreCase(name, "transfer-encoding")) {
            if (head.chunked or !std.ascii.eqlIgnoreCase(value, "chunked")) return error.MalformedRequest;
            head.chunked = true;
        } else if (std.ascii.eqlIgnoreCase(name, "authorization")) {
            if (head.authorization == null) head.authorization = value;
        } else if (std.ascii.eqlIgnoreCase(name, "accept-encoding")) {
            if (!encoding_seen) head.accept_gzip = gzipAccepted(value);
            encoding_seen = true;
        } else if (std.ascii.eqlIgnoreCase(name, "expect")) {
            head.expect_continue = std.ascii.eqlIgnoreCase(value, "100-continue");
        } else if (std.ascii.eqlIgnoreCase(name, "connection")) {
            var tokens = std.mem.splitScalar(u8, value, ',');
            while (tokens.next()) |token| {
                const trimmed = std.mem.trim(u8, token, " \t");
                if (std.ascii.eqlIgnoreCase(trimmed, "close")) connection_close = true;
                if (std.ascii.eqlIgnoreCase(trimmed, "keep-alive")) keep_alive = true;
            }
        }
        cursor = end + 2;
    }
    if ((!old_http and !host_seen) or (head.chunked and length_seen)) return error.MalformedRequest;
    if (head.content_length > max_request - head.header_len) return error.RequestTooLarge;
    head.close_after = connection_close or (old_http and !keep_alive);
    return head;
}

const ChunkResult = struct { used: usize, body_len: usize };

fn chunks(input: []const u8, output: ?[]u8) ParseError!?ChunkResult {
    var offset: usize = 0;
    var body_len: usize = 0;
    while (true) {
        const relative = std.mem.indexOf(u8, input[offset..], "\r\n") orelse return null;
        const line = input[offset .. offset + relative];
        const extension = std.mem.indexOfScalar(u8, line, ';') orelse line.len;
        const digits = line[0..extension];
        if (digits.len == 0) return error.MalformedRequest;
        for (digits) |ch| switch (ch) {
            '0'...'9', 'a'...'f', 'A'...'F' => {},
            else => return error.MalformedRequest,
        };
        const size = std.fmt.parseInt(usize, digits, 16) catch return error.MalformedRequest;
        offset += relative + 2;
        if (size == 0) {
            // Trailers do not change the headers used for routing or framing.
            while (true) {
                const trailer_end = std.mem.indexOf(u8, input[offset..], "\r\n") orelse return null;
                if (trailer_end == 0) return .{ .used = offset + 2, .body_len = body_len };
                const trailer = input[offset .. offset + trailer_end];
                const colon = std.mem.indexOfScalar(u8, trailer, ':') orelse return error.MalformedRequest;
                if (colon == 0 or !validValue(trailer[colon + 1 ..])) return error.MalformedRequest;
                for (trailer[0..colon]) |ch| if (!tokenByte(ch)) return error.MalformedRequest;
                offset += trailer_end + 2;
            }
        }
        if (size > max_request - body_len) return error.RequestTooLarge;
        if (input.len - offset < size + 2) return null;
        if (!std.mem.eql(u8, input[offset + size .. offset + size + 2], "\r\n")) return error.MalformedRequest;
        if (output) |out| {
            if (body_len > out.len or size > out.len - body_len) return error.RequestTooLarge;
            std.mem.copyForwards(u8, out[body_len .. body_len + size], input[offset .. offset + size]);
        }
        body_len += size;
        offset += size + 2;
    }
}

pub fn messageLength(head: Head, raw: []const u8) ParseError!?usize {
    if (!head.chunked) return head.header_len + head.content_length;
    const decoded = try chunks(raw[head.header_len..], null) orelse return null;
    return head.header_len + decoded.used;
}

pub fn parseRequest(raw: []const u8, scratch: []u8) ParseError!?Request {
    const head = try parseHead(raw) orelse return null;
    const used = try messageLength(head, raw) orelse return null;
    if (raw.len < used) return null;
    const body = if (head.chunked) blk: {
        const decoded = (try chunks(raw[head.header_len..], scratch)).?;
        break :blk scratch[0..decoded.body_len];
    } else raw[head.header_len..used];
    return .{ .method = head.method, .path = head.path, .authorization = head.authorization, .accept_gzip = head.accept_gzip, .body = body };
}
