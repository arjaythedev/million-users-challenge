// HTTP/1.1: parsing the request at the front of a byte buffer, and the response status lines.
namespace http;
import std;

let MAX_REQUEST = 65536;         // the header block, and separately the body, may be this long

// A parsed request. Every slice points into the buffer it was parsed from; nothing is copied.
struct Request {
    method: u8[:], target: u8[:], authorization: u8[:], body: u8[:],
    size: i64,                   // bytes the whole request takes in the buffer
    close: bool,                 // HTTP/1.0, or Connection: close
    gzip: bool,                  // Accept-Encoding allows gzip
}

enum Parsed {
    Complete,                    // a whole request
    Incomplete,                  // wait for more bytes
    ExpectContinue,              // the headers are in and the client waits for "100 Continue" before the body
    Malformed,                   // drop the connection
}

// The carriage return that ends a line. With one byte in the set, find_any is a memchr.
let CR = cr_set();

fn cr_set() -> ByteSet {
    var members: bool[256] = [false; 256];
    members['\r'] = true;
    return byte_set(members);
}

// What the header lines say. A bad line is noted here and judged once the whole head is in, so that a head still
// arriving is Incomplete whatever it contains so far.
struct Head { length: i64, expect: bool, bad: bool }

// Parses the request at the front of buf. The request is meaningful only when Parsed.Complete.
fn parse(buf: u8[:]) -> Request, Parsed {
    var r = Request { .. };
    var h = Head { length: 0, expect: false, bad: false };
    var start = 0;                                    // where the current line begins
    var from = 0;                                     // where the search for its CR goes on (a lone CR is text)
    var first = true;
    var body_at = 0;
    loop {
        let k = find_any(buf[from..], CR);
        if k < 0 || from + k + 1 >= buf.len {         // the head is not all here yet
            return r, if buf.len > MAX_REQUEST { Parsed.Malformed } else { Parsed.Incomplete };
        }
        let cr = from + k;
        if buf[cr + 1] != '\n' { from = cr + 1; continue; }
        if cr == start && !first { body_at = cr + 2; break; }   // the blank line ends the head
        let line = buf[start..cr];
        if first {
            if !request_line(r, line) { h.bad = true; }
            first = false;
        } else { header(r, h, line); }
        start = cr + 2;
        from = start;
    }
    guard !h.bad else { return r, Parsed.Malformed; }

    if buf.len - body_at < h.length {
        return r, if h.expect && body_at == buf.len { Parsed.ExpectContinue } else { Parsed.Incomplete };
    }
    r.body = buf[body_at..body_at + h.length];
    r.size = body_at + h.length;
    return r, Parsed.Complete;
}

// METHOD SP TARGET SP VERSION
fn request_line(r: Request&, line: u8[:]) -> bool {
    let a = position(line, ' ');
    if a < 0 { return false; }
    let b = position(line[a + 1..], ' ');
    if b < 0 { return false; }
    r.method = line[..a];
    r.target = line[a + 1..a + 1 + b];
    r.close = line[a + 2 + b..] == "HTTP/1.0";
    return true;
}

// Name: value
fn header(r: Request&, h: Head&, line: u8[:]) {
    let colon = position(line, ':');
    guard colon >= 0 else { return; }
    let name = line[..colon];
    let value = trim_blanks(line[colon + 1..]);
    if ieq(name, "content-length") {
        h.length = 0;
        for digit in value {
            if digit < '0' || digit > '9' { h.bad = true; return; }
            h.length = h.length * 10 + (digit - '0');
            if h.length > MAX_REQUEST { h.bad = true; return; }
        }
    } else if ieq(name, "authorization") { r.authorization = value; }
    else if ieq(name, "connection") {
        if ieq(value, "close") { r.close = true; } else if ieq(value, "keep-alive") { r.close = false; }
    } else if ieq(name, "accept-encoding") { r.gzip = accepts_gzip(value); }
    else if ieq(name, "expect") { h.expect = ieq(value, "100-continue"); }
    else if ieq(name, "transfer-encoding") { h.bad = true; }    // chunked uploads: not supported
}

// s without leading and trailing spaces and tabs.
fn trim_blanks(s: u8[:]) -> u8[:] {
    var a = 0;
    var e = s.len;
    while a < e && (s[a] == ' ' || s[a] == '\t') { a++; }
    while e > a && (s[e - 1] == ' ' || s[e - 1] == '\t') { e--; }
    return s[a..e];
}

fn lower(c: u8) -> u8 { if c >= 'A' && c <= 'Z' { c + 32 } else { c } }

// Case-insensitive equality with an all-lowercase literal.
fn ieq(a: u8[:], lit: const u8[:]) -> bool {
    if a.len != lit.len { return false; }
    for i in a.len { if lower(a[i]) != lit[i] { return false; } }
    return true;
}

// "gzip" listed in an Accept-Encoding value, and not with q=0.
fn accepts_gzip(v: u8[:]) -> bool {
    let g = find(v, "gzip");
    guard g >= 0 else { return false; }
    var i = g + 4;
    while i < v.len && v[i] == ' ' { i++; }
    if i + 2 < v.len && v[i] == ';' {
        let q = find(v[i..], "q=");
        if q >= 0 {
            let start = i + q + 2;
            let weight, ok = parse_flt(v[start..min(v.len, start + 7)]);
            return ok && weight > 0.0;
        }
    }
    return true;
}

// A positive decimal integer (a path segment), else -1. Huge ids map to i64 max: not found.
fn parse_id(s: u8[:]) -> i64 {
    if s.len == 0 { return -1; }
    var v: i64 = 0;
    for c in s {
        if c < '0' || c > '9' { return -1; }
        if v < 100000000000000000 { v = v * 10 + (c - '0'); } else { v = 0x7fffffffffffffff; }
    }
    return if v > 0 { v } else { -1 };
}

fn status_line(code: i64) -> const u8[:] {
    match code {
        200 => "HTTP/1.1 200 OK\r\n",
        201 => "HTTP/1.1 201 Created\r\n",
        202 => "HTTP/1.1 202 Accepted\r\n",
        400 => "HTTP/1.1 400 Bad Request\r\n",
        401 => "HTTP/1.1 401 Unauthorized\r\n",
        403 => "HTTP/1.1 403 Forbidden\r\n",
        404 => "HTTP/1.1 404 Not Found\r\n",
        405 => "HTTP/1.1 405 Method Not Allowed\r\n",
        409 => "HTTP/1.1 409 Conflict\r\n",
        410 => "HTTP/1.1 410 Gone\r\n",
        422 => "HTTP/1.1 422 Unprocessable Entity\r\n",
        429 => "HTTP/1.1 429 Too Many Requests\r\n",
        500 => "HTTP/1.1 500 Internal Server Error\r\n",
        502 => "HTTP/1.1 502 Bad Gateway\r\n",
        503 => "HTTP/1.1 503 Service Unavailable\r\n",
        504 => "HTTP/1.1 504 Gateway Timeout\r\n",
        _ => "HTTP/1.1 500 Internal Server Error\r\n",
    }
}
