// Routing, the five endpoints, and turning Postgres messages into responses.

#if os(Linux)
import CShim

public enum Method: UInt8 {
    case get, post, other
}

/// A parsed request. Every view points into the receive buffer.
public struct Request {
    public var method: Method
    /// Path without the query string.
    public var path: ByteView
    public var authorization: ByteView?
    public var body: ByteView
    public var keepAlive: Bool
    public var acceptsGzip: Bool

    public init(method: Method, path: ByteView, authorization: ByteView?, body: ByteView, keepAlive: Bool = true, acceptsGzip: Bool = false) {
        self.method = method
        self.path = path
        self.authorization = authorization
        self.body = body
        self.keepAlive = keepAlive
        self.acceptsGzip = acceptsGzip
    }
}

/// Who waits for a query's result.
struct Waiter {
    var ring: Int32
    var slot: UInt32
    var generation: UInt16
    var close: Bool
    var gzip: Bool
}

/// The status of an answered request, or `waiting` when it went to Postgres.
let waiting = 0

/// Reads per Sync: past this a pack closes and the next one takes the shortest queue.
let packSize = 64

extension Server {
    /// Writes the JSON body into `body` and returns the status, or `waiting`.
    func respond(_ r: Request, _ waiter: Waiter) -> Int {
        let path = r.path
        if path.equals("/feed") {
            return r.method == .get ? read(.feed, waiter, 0) : notFound()
        }
        if path.hasPrefix("/posts/") {
            let rest = path.slice(7, path.count)
            if r.method == .post, rest.count > 5, rest.slice(rest.count &- 5, rest.count).equals("/like") {
                return like(r, rest.slice(0, rest.count &- 5), waiter)
            }
            if r.method == .get, rest.count > 0, !contains(rest, UInt8(ascii: "/")) {
                guard let id = parsePositiveInt(rest) else { return error(400, "invalid post id") }
                return read(.post, waiter, id)
            }
            return notFound()
        }
        if path.equals("/posts") {
            return r.method == .post ? createPost(r, waiter) : notFound()
        }
        if path.equals("/health") {
            return r.method == .get ? read(.health, waiter, 0) : notFound()
        }
        return notFound()
    }

    @inline(__always)
    func error(_ status: Int, _ message: StaticString) -> Int {
        body.truncate(0)
        body.append("{\"error\":\"")
        body.append(message)
        body.append("\"}")
        return status
    }

    func notFound() -> Int { error(404, "not found") }

    func unavailable(_ kind: PendingKind) -> Int {
        guard kind == .health else { return error(500, "internal server error") }
        writeDegraded("no database connection")
        return 503
    }

    func writeDegraded(_ message: StaticString) {
        body.truncate(0)
        body.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"")
        body.append(message)
        body.append("\"}")
    }

    /// Every read of a pass goes onto one connection, so they share one write and one Sync.
    /// `packSize` reads close the pack and the next one takes the shortest queue.
    func read(_ kind: PendingKind, _ waiter: Waiter, _ id: Int64) -> Int {
        if let c = pack, c.alive, c.packCount >= packSize { closePack() }
        if pack == nil || !pack.unsafelyUnwrapped.alive {
            pack = shortest(readers)
        }
        guard let c = pack else { return unavailable(kind) }
        switch kind {
        case .feed:
            c.out.appendBindExecute("f", parameterCount: 0) { _ in }
        case .post:
            c.out.appendBindExecute("p", parameterCount: 1) { $0.appendBE32(8); $0.appendBE64(id) }
        default:
            c.out.appendBindExecute("h", parameterCount: 0) { _ in }
        }
        c.push(Pending(kind: kind, close: waiter.close, gzip: waiter.gzip, generation: waiter.generation, ring: waiter.ring, slot: waiter.slot, postID: id))
        c.packCount &+= 1
        return waiting
    }

    func closePack() {
        guard let c = pack else { return }
        if c.alive && c.packCount > 0 {
            c.out.appendSync()
            c.push(Pending(kind: .sync))
        }
        c.packCount = 0
        pack = nil
    }

    func shortest(_ list: [PgConnection]) -> PgConnection? {
        var best: PgConnection? = nil
        for c in list where c.alive {
            if best == nil || c.queueCount < best.unsafelyUnwrapped.queueCount { best = c }
        }
        return best
    }

    /// Writes the 401 body for a failed check, or returns the user.
    func authenticate(_ r: Request) -> (id: Int64, username: ByteView)? {
        switch jwt.verify(authorization: r.authorization, nowMs: sv_now_ms()) {
        case .missingBearer:
            _ = error(401, "missing bearer token")
        case .invalidToken:
            _ = error(401, "invalid or expired token")
        case .invalidPayload:
            _ = error(401, "invalid token payload")
        case .user(let id, let username):
            return (id, username)
        }
        return nil
    }

    func createPost(_ r: Request, _ waiter: Waiter) -> Int {
        guard let user = authenticate(r) else { return 401 }

        var bodyValue: ByteView? = nil
        var bodyEscaped = false
        let doc = scanJSONObject(r.body) { m in
            if keyIs(m, "body") {
                bodyValue = m.kind == .string ? m.value : nil
                bodyEscaped = m.escaped
            }
        }
        if doc == .malformed { return error(400, "malformed JSON body") }
        guard doc == .object, var text = bodyValue else { return error(400, "body is required") }
        if bodyEscaped {
            bodyScratch.truncate(0)
            unescapeJSONString(text, into: &bodyScratch)
            text = bodyScratch.view
        }
        text = trimWhitespace(text)
        if text.count == 0 { return error(400, "body is required") }
        if scalarCount(text) > 500 { return error(400, "body must be at most 500 characters") }

        guard let c = shortest(writers) else { return error(500, "internal server error") }
        c.out.appendBindExecute("c", parameterCount: 2) { out in
            out.appendBE32(8)
            out.appendBE64(user.id)
            out.appendBE32(Int32(text.count))
            out.append(text)
        }
        c.out.appendSync()

        // The 201 repeats the body and the author, and the request's memory won't outlive this call.
        body.truncate(0)
        body.appendJSONString(text)
        let bodyCount = body.count
        body.append(user.username)
        let saved = UnsafeMutablePointer<UInt8>.allocate(capacity: max(body.count, 1))
        saved.update(from: body.base, count: body.count)
        var p = Pending(kind: .createPost, close: waiter.close, gzip: waiter.gzip, generation: waiter.generation, ring: waiter.ring, slot: waiter.slot)
        p.text = saved
        p.bodyCount = Int32(bodyCount)
        p.authorCount = Int32(body.count &- bodyCount)
        c.push(p)
        return waiting
    }

    func like(_ r: Request, _ idText: ByteView, _ waiter: Waiter) -> Int {
        guard let user = authenticate(r) else { return 401 }
        guard let postID = parsePositiveInt(idText) else { return error(400, "invalid post id") }
        guard let c = shortest(writers) else { return error(500, "internal server error") }
        c.out.appendBindExecute("l", parameterCount: 2) { out in
            out.appendBE32(8)
            out.appendBE64(user.id)
            out.appendBE32(8)
            out.appendBE64(postID)
        }
        c.out.appendSync()
        c.push(Pending(kind: .like, close: waiter.close, gzip: waiter.gzip, generation: waiter.generation, ring: waiter.ring, slot: waiter.slot, postID: postID))
        return waiting
    }

    // MARK: Postgres messages

    /// Parses every complete message in the connection's input.
    func parseMessages(_ c: PgConnection, _ arena: Arena) {
        var offset = 0
        let input = UnsafePointer(c.input)
        while c.inputCount &- offset >= 5 {
            let length = Int(readBE32(input + (offset &+ 1)))
            if c.inputCount &- offset < 1 &+ length { break }
            let message = input + (offset &+ 5)
            let n = length &- 4
            switch input[offset] {
            case UInt8(ascii: "D"): dataRow(c, message)
            case UInt8(ascii: "C"): commandComplete(c, message, n, arena)
            case UInt8(ascii: "Z"): readyForQuery(c, arena)
            case UInt8(ascii: "E"): errorResponse(c, message, n, arena)
            default: break // BindComplete and notices
            }
            offset &+= 1 &+ length
        }
        if offset > 0 {
            let rest = c.inputCount &- offset
            if rest > 0 { memmove(c.input, c.input + offset, rest) }
            c.inputCount = rest
        }
    }

    func dataRow(_ c: PgConnection, _ message: UnsafePointer<UInt8>) {
        guard c.queueCount > 0 else { return }
        var p = message + 2
        @inline(__always) func column() -> ByteView {
            let n = Int(readBE32(p))
            let view = ByteView(base: p + 4, count: n)
            p += 4 &+ n
            return view
        }
        switch c.head.pointee.kind {
        case .feed, .post:
            if c.rows == 0 {
                c.scratch.truncate(0)
                if c.head.pointee.kind == .feed { c.scratch.append("{\"posts\":[") } else { c.scratch.append("{\"post\":") }
            } else {
                c.scratch.append(UInt8(ascii: ","))
            }
            c.rows &+= 1
            let id = column()
            let text = column()
            let created = column()
            let author = column()
            let likes = column()
            c.scratch.reserve(text.count &* 6 &+ author.count &* 6 &+ 128)
            c.scratch.append("{\"id\":")
            c.scratch.appendInt(readBE64(id.base.unsafelyUnwrapped))
            c.scratch.append(",\"body\":")
            c.scratch.appendJSONString(text)
            c.scratch.append(",\"created_at\":")
            c.scratch.appendTimestamp(readBE64(created.base.unsafelyUnwrapped))
            c.scratch.append(",\"author\":")
            c.scratch.appendJSONString(author)
            c.scratch.append(",\"like_count\":")
            c.scratch.appendInt(Int64(readBE32(likes.base.unsafelyUnwrapped)))
            c.scratch.append(UInt8(ascii: "}"))
        case .createPost:
            let pending = c.head
            let id = column()
            let created = column()
            c.scratch.truncate(0)
            c.scratch.reserve(Int(pending.pointee.bodyCount) &+ Int(pending.pointee.authorCount) &+ 128)
            c.scratch.append("{\"post\":{\"id\":")
            c.scratch.appendInt(readBE64(id.base.unsafelyUnwrapped))
            c.scratch.append(",\"body\":")
            let saved = UnsafePointer(pending.pointee.text.unsafelyUnwrapped)
            c.scratch.append(saved, Int(pending.pointee.bodyCount))
            c.scratch.append(",\"created_at\":")
            c.scratch.appendTimestamp(readBE64(created.base.unsafelyUnwrapped))
            c.scratch.append(",\"author\":\"")
            c.scratch.append(saved + Int(pending.pointee.bodyCount), Int(pending.pointee.authorCount))
            c.scratch.append("\",\"like_count\":0}}")
            c.rows &+= 1
        default:
            break // SELECT 1
        }
    }

    func commandComplete(_ c: PgConnection, _ tag: UnsafePointer<UInt8>, _ n: Int, _ arena: Arena) {
        guard c.queueCount > 0 else { return }
        let pending = c.head
        switch pending.pointee.kind {
        case .feed:
            if c.rows == 0 {
                c.scratch.truncate(0)
                c.scratch.append("{\"posts\":[")
            }
            c.scratch.append("]}")
            answer(pending.pointee, 200, c.scratch.view, arena)
            c.rows = 0
            c.pop()
        case .post:
            if c.rows == 0 {
                _ = error(404, "post not found")
                answer(pending.pointee, 404, body.view, arena)
            } else {
                c.scratch.append(UInt8(ascii: "}"))
                answer(pending.pointee, 200, c.scratch.view, arena)
            }
            c.rows = 0
            c.pop()
        case .health:
            body.truncate(0)
            body.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
            body.appendInt(sv_monotonic_s() &- startedAt)
            body.append(UInt8(ascii: "}"))
            answer(pending.pointee, 200, body.view, arena)
            c.pop()
        case .like:
            // The tag ends in the row count, then a NUL: "INSERT 0 1".
            pending.pointee.inserted = n >= 2 && tag[n &- 2] == UInt8(ascii: "1")
            pending.pointee.finished = true
        case .createPost:
            pending.pointee.finished = c.rows > 0
        case .sync:
            break
        }
    }

    func errorResponse(_ c: PgConnection, _ fields: UnsafePointer<UInt8>, _ n: Int, _ arena: Arena) {
        c.rows = 0
        guard c.queueCount > 0 else { return }
        if c.reader {
            // Postgres skips the rest of the pack, and readyForQuery fails it.
            if c.head.pointee.kind != .sync {
                fail(c.head.pointee, arena)
                c.pop()
            }
            c.aborted = true
            return
        }
        // A write has its own Sync, so ReadyForQuery comes next.
        var i = 0
        while i < n, fields[i] != 0 {
            let type = fields[i]
            var end = i &+ 1
            while end < n, fields[end] != 0 { end &+= 1 }
            if type == UInt8(ascii: "C"), end &- i &- 1 == 5 {
                c.head.pointee.foreignKeyViolation = ByteView(base: fields + (i &+ 1), count: 5).equals("23503")
            }
            i = end &+ 1
        }
    }

    func readyForQuery(_ c: PgConnection, _ arena: Arena) {
        guard c.queueCount > 0 else { return }
        if c.reader {
            while c.queueCount > 0, c.head.pointee.kind != .sync {
                fail(c.head.pointee, arena)
                c.pop()
            }
            if c.queueCount > 0 { c.pop() }
            c.aborted = false
            return
        }
        let pending = c.head
        switch pending.pointee.kind {
        case .like where pending.pointee.finished:
            let inserted = pending.pointee.inserted
            body.truncate(0)
            body.append("{\"liked\":true,\"already_liked\":")
            body.append(inserted ? "false" : "true")
            body.append(",\"post_id\":")
            body.appendInt(pending.pointee.postID)
            body.append(UInt8(ascii: "}"))
            answer(pending.pointee, inserted ? 201 : 200, body.view, arena)
        case .like where pending.pointee.foreignKeyViolation:
            _ = error(404, "post not found")
            answer(pending.pointee, 404, body.view, arena)
        case .createPost where pending.pointee.finished:
            answer(pending.pointee, 201, c.scratch.view, arena)
        default:
            fail(pending.pointee, arena)
        }
        pending.pointee.text?.deallocate()
        c.rows = 0
        c.pop()
    }

    func fail(_ p: Pending, _ arena: Arena) {
        if p.kind == .health {
            writeDegraded("query failed")
            answer(p, 503, body.view, arena)
        } else {
            _ = error(500, "internal server error")
            answer(p, 500, body.view, arena)
        }
    }

    /// Fails everything the connection owed and schedules a reconnect.
    func lost(_ c: PgConnection, _ arena: Arena) {
        guard c.alive else { return }
        // The shutdown ends a receive still in flight, and the generation drops its completion.
        _ = shutdown(c.fd, Int32(SHUT_RDWR))
        _ = close(c.fd)
        c.fd = -1
        c.generation &+= 1
        c.inputCount = 0
        c.packCount = 0
        c.out.truncate(0)
        c.sendActive = false
        c.rows = 0
        c.aborted = false
        if pack === c { pack = nil }
        while c.queueCount > 0 {
            let p = c.head.pointee
            if p.kind != .sync { fail(p, arena) }
            p.text?.deallocate()
            c.pop()
        }
    }
}

@inline(__always)
func contains(_ v: ByteView, _ b: UInt8) -> Bool {
    for i in 0..<v.count where v[i] == b { return true }
    return false
}

// MARK: Text helpers

/// Number of Unicode scalars in valid UTF-8: every byte that isn't a continuation byte.
func scalarCount(_ v: ByteView) -> Int {
    var n = 0
    for i in 0..<v.count where v[i] & 0xC0 != 0x80 { n &+= 1 }
    return n
}

/// JavaScript's String.prototype.trim set: ASCII whitespace, NBSP, the Unicode space separators,
/// line/paragraph separators and the BOM.
@inline(__always)
func isTrimmable(_ cp: UInt32) -> Bool {
    switch cp {
    case 0x09...0x0D, 0x20, 0xA0, 0x1680, 0x2000...0x200A, 0x2028, 0x2029, 0x202F, 0x205F, 0x3000, 0xFEFF:
        return true
    default:
        return false
    }
}

/// Decodes the scalar starting at `i`. Returns (scalar, length); invalid bytes decode as (0xFFFD, 1).
func decodeScalar(_ v: ByteView, _ i: Int) -> (UInt32, Int) {
    let b0 = UInt32(v[i])
    if b0 < 0x80 { return (b0, 1) }
    let n = v.count &- i
    if b0 & 0xE0 == 0xC0, n >= 2 { return ((b0 & 0x1F) << 6 | UInt32(v[i &+ 1] & 0x3F), 2) }
    if b0 & 0xF0 == 0xE0, n >= 3 {
        return ((b0 & 0x0F) << 12 | UInt32(v[i &+ 1] & 0x3F) << 6 | UInt32(v[i &+ 2] & 0x3F), 3)
    }
    if b0 & 0xF8 == 0xF0, n >= 4 {
        return ((b0 & 0x07) << 18 | UInt32(v[i &+ 1] & 0x3F) << 12 | UInt32(v[i &+ 2] & 0x3F) << 6
            | UInt32(v[i &+ 3] & 0x3F), 4)
    }
    return (0xFFFD, 1)
}

func trimWhitespace(_ v: ByteView) -> ByteView {
    var start = 0
    var end = v.count
    while start < end {
        let b = v[start]
        if b < 0x80 {
            guard b == 0x20 || (b >= 0x09 && b <= 0x0D) else { break }
            start &+= 1
        } else {
            let (cp, len) = decodeScalar(v, start)
            guard isTrimmable(cp) else { break }
            start &+= len
        }
    }
    while end > start {
        let b = v[end &- 1]
        if b < 0x80 {
            guard b == 0x20 || (b >= 0x09 && b <= 0x0D) else { break }
            end &-= 1
        } else {
            var s = end &- 1
            while s > start && v[s] & 0xC0 == 0x80 { s &-= 1 }
            let (cp, len) = decodeScalar(v, s)
            guard s &+ len == end, isTrimmable(cp) else { break }
            end = s
        }
    }
    return v.slice(start, end)
}
#endif
