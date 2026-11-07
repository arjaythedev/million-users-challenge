import CShim

// HTTP/1.1 framing: request parsing in place and response headers written in front of the body.

public enum ParseResult {
    /// Need more bytes.
    case incomplete
    /// Unparseable or unsupported (e.g. chunked bodies): answer 400 and close.
    case invalid
    /// A full request occupying the first `length` bytes.
    case request(Request, length: Int)
}

/// Upper bound for request line plus headers. Larger requests are rejected.
public let maxHeaderBytes = 16 * 1024
/// Upper bound for a body. SPEC.md leaves anything over 16 KB out of scope.
public let maxBodyBytes = 1024 * 1024

/// Parses one request from the start of `v`.
public func parseRequest(_ v: ByteView) -> ParseResult {
    let n = v.count
    guard n > 0, let p = v.base else { return .incomplete }

    // Find the end of the header block.
    var headerEnd = -1
    var i = 0
    let limit = min(n, maxHeaderBytes)
    while i &+ 3 < limit {
        if p[i] == 0x0D && p[i &+ 1] == 0x0A && p[i &+ 2] == 0x0D && p[i &+ 3] == 0x0A {
            headerEnd = i &+ 4
            break
        }
        i &+= 1
    }
    if headerEnd < 0 { return n >= maxHeaderBytes ? .invalid : .incomplete }

    // Request line: METHOD SP target SP HTTP/1.x CRLF
    var lineEnd = 0
    while lineEnd < headerEnd && p[lineEnd] != 0x0D { lineEnd &+= 1 }
    var sp1 = 0
    while sp1 < lineEnd && p[sp1] != 0x20 { sp1 &+= 1 }
    guard sp1 > 0, sp1 < lineEnd else { return .invalid }
    var sp2 = sp1 &+ 1
    while sp2 < lineEnd && p[sp2] != 0x20 { sp2 &+= 1 }
    guard sp2 < lineEnd, lineEnd &- sp2 == 9 else { return .invalid }
    let version = v.slice(sp2 &+ 1, lineEnd)
    guard version.hasPrefix("HTTP/1.") else { return .invalid }
    let http10 = version[7] == UInt8(ascii: "0")

    let methodView = v.slice(0, sp1)
    let method: Method = methodView.equals("GET") ? .get : (methodView.equals("POST") ? .post : .other)
    var pathEnd = sp1 &+ 1
    while pathEnd < sp2 && p[pathEnd] != UInt8(ascii: "?") { pathEnd &+= 1 }
    let path = v.slice(sp1 &+ 1, pathEnd)

    var authorization: ByteView? = nil
    var contentLength = 0
    var keepAlive = !http10
    var acceptsGzip = false
    var lineStart = lineEnd &+ 2
    while lineStart < headerEnd &- 2 {
        var e = lineStart
        while p[e] != 0x0D { e &+= 1 }
        var colon = lineStart
        while colon < e && p[colon] != UInt8(ascii: ":") { colon &+= 1 }
        guard colon < e else { return .invalid }
        let name = v.slice(lineStart, colon)
        var vs = colon &+ 1
        while vs < e && (p[vs] == 0x20 || p[vs] == 0x09) { vs &+= 1 }
        var ve = e
        while ve > vs && (p[ve &- 1] == 0x20 || p[ve &- 1] == 0x09) { ve &-= 1 }
        let value = v.slice(vs, ve)
        switch name.count {
        case 13 where equalsIgnoringCase(name, "authorization"):
            authorization = value
        case 14 where equalsIgnoringCase(name, "content-length"):
            guard value.count > 0, value.count <= 9 else { return .invalid }
            var len = 0
            for k in 0..<value.count {
                let d = value[k] &- 48
                guard d < 10 else { return .invalid }
                len = len &* 10 &+ Int(d)
            }
            contentLength = len
        case 10 where equalsIgnoringCase(name, "connection"):
            if equalsIgnoringCase(value, "close") { keepAlive = false }
            else if equalsIgnoringCase(value, "keep-alive") { keepAlive = true }
        case 15 where equalsIgnoringCase(name, "accept-encoding"):
            acceptsGzip = listsGzip(value)
        case 17 where equalsIgnoringCase(name, "transfer-encoding"):
            return .invalid
        default:
            break
        }
        lineStart = e &+ 2
    }

    guard contentLength <= maxBodyBytes else { return .invalid }
    let total = headerEnd &+ contentLength
    if n < total { return .incomplete }
    let body = contentLength > 0 ? v.slice(headerEnd, total) : ByteView.empty
    return .request(
        Request(method: method, path: path, authorization: authorization, body: body, keepAlive: keepAlive, acceptsGzip: acceptsGzip),
        length: total)
}

@inline(__always)
func equalsIgnoringCase(_ v: ByteView, _ s: StaticString) -> Bool {
    guard v.count == s.utf8CodeUnitCount else { return false }
    let q = s.utf8Start
    for i in 0..<v.count where v[i] | 0x20 != q[i] | 0x20 { return false }
    return true
}

/// Status line, Content-Type and Content-Length for each status we send.
func statusLine(_ status: Int) -> StaticString {
    switch status {
    case 200: return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
    case 201: return "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: "
    case 400: return "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: "
    case 401: return "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: "
    case 404: return "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: "
    case 503: return "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: "
    default: return "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: "
    }
}

/// Whether an Accept-Encoding value lists gzip and doesn't refuse it with q=0.
func listsGzip(_ v: ByteView) -> Bool {
    var i = 0
    while i &+ 4 <= v.count {
        if v[i] == UInt8(ascii: "g"), v.slice(i, i &+ 4).equals("gzip") {
            var e = i &+ 4
            while e < v.count, v[e] != UInt8(ascii: ",") { e &+= 1 }
            let params = v.slice(i &+ 4, e)
            return !(params.count >= 3 && contains(params, UInt8(ascii: "=")) && params[params.count &- 1] == UInt8(ascii: "0"))
        }
        i &+= 1
    }
    return false
}

/// Bodies below this go out as they are: they fit a segment already.
let gzipMinimum = 1024

/// Appends a full response. A large body goes out compressed if the client takes it.
func appendHTTPResponse(status: Int, body: ByteView, close: Bool, gzip: Bool, into out: inout OutBuffer) {
    let line = statusLine(status)
    out.reserve(line.utf8CodeUnitCount &+ 192 &+ body.count &* 2)
    out.append(line)
    if gzip && body.count >= gzipMinimum {
        // The digits and the rest of the head are at most this many bytes, written after the fact.
        let lengthAt = out.count
        let head = 96
        let room = out.capacity &- (lengthAt &+ head)
        let size = sv_gzip(body.base, body.count, out.base + (lengthAt &+ head), room)
        if size > 0 && size < body.count {
            out.count = lengthAt
            out.moveForHeader(bodyAt: lengthAt &+ head, size: size, close: close)
            return
        }
    }
    out.appendInt(Int64(body.count))
    out.append(close ? "\r\nConnection: close\r\n\r\n" : "\r\n\r\n")
    out.append(body)
}

extension OutBuffer {
    /// Writes the end of the head at `count`, then slides the `size` compressed bytes behind it.
    mutating func moveForHeader(bodyAt: Int, size: Int, close: Bool) {
        appendInt(Int64(size))
        append(close ? "\r\nContent-Encoding: gzip\r\nConnection: close\r\n\r\n" : "\r\nContent-Encoding: gzip\r\n\r\n")
        move(from: bodyAt, to: count, length: size)
        count &+= size
    }
}

/// A complete 400 response for unparseable requests.
public func appendBadRequest(into out: inout OutBuffer) {
    out.append("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 29\r\nConnection: close\r\n\r\n{\"error\":\"malformed request\"}")
}

/// A complete 500 response, used when a batch's commit fails.
public func appendInternalError(close: Bool, into out: inout OutBuffer) {
    out.append("HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33")
    out.append(close ? "\r\nConnection: close\r\n\r\n" : "\r\n\r\n")
    out.append("{\"error\":\"internal server error\"}")
}
