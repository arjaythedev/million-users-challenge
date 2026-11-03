// A Postgres v3 client. A connection logs in (SCRAM-SHA-256 or cleartext) and prepares SPEC.md's statements on a
// blocking socket; then the socket turns non-blocking and joins its reactor's epoll set, and the connection is a
// pipeline: Bind/Execute pairs and Syncs queue up and go out in one write, and replies are read as they arrive and
// taken message by message out of the receive buffer (Database.swift reads them).
import Glibc
import Syscalls

/// The Postgres type OIDs the statements' parameters are declared with.
enum OID {
    static let int8: UInt32 = 20
    static let text: UInt32 = 25
}

struct SqlStateErr: Error, CustomStringConvertible, Sendable {
    let code: String
    let message: String
    var description: String { "\(code): \(message)" }
}

enum PgError: Error, CustomStringConvertible {
    case io(String)
    case server(SqlStateErr)
    case wire(String)

    var description: String {
        switch self {
        case .io(let e): "io: \(e)"
        case .server(let e): "db: \(e)"
        case .wire(let s): "wire: \(s)"
        }
    }
}

struct DbEndpoint: Sendable {
    let host: String
    let port: UInt16
    let user: String
    let password: String
    let database: String

    private static func decodeURLPart(_ s: Substring) -> String {
        let raw = Array(s.utf8)
        var out: [UInt8] = []
        var i = 0
        while i < raw.count {
            if raw[i] == 0x25, i + 2 < raw.count, let b = UInt8(String(decoding: raw[(i + 1)...(i + 2)], as: UTF8.self), radix: 16) {
                out.append(b)
                i += 3
                continue
            }
            out.append(raw[i])
            i += 1
        }
        return String(decoding: out, as: UTF8.self)
    }

    init?(url: String) {
        var rest = Substring(url)
        if rest.hasPrefix("postgres://") {
            rest = rest.dropFirst("postgres://".count)
        } else if rest.hasPrefix("postgresql://") {
            rest = rest.dropFirst("postgresql://".count)
        } else {
            return nil
        }
        guard let at = rest.lastIndex(of: "@") else { return nil }
        let creds = rest[..<at]
        let target = rest[rest.index(after: at)...]
        let (user, pass) = creds.firstIndex(of: ":").map { (creds[..<$0], creds[creds.index(after: $0)...]) } ?? (creds, "")
        let (addr, dbPart) = target.firstIndex(of: "/").map { (target[..<$0], target[target.index(after: $0)...]) } ?? (target, "")
        let db = dbPart.split(separator: "?", omittingEmptySubsequences: false).first ?? ""
        if let colon = addr.lastIndex(of: ":") {
            guard let p = UInt16(addr[addr.index(after: colon)...]) else { return nil }
            host = String(addr[..<colon])
            port = p
        } else {
            host = String(addr)
            port = 5432
        }
        self.user = Self.decodeURLPart(user)
        password = Self.decodeURLPart(pass)
        database = Self.decodeURLPart(db.isEmpty ? user : db)
    }
}

/// SPEC.md's statements, exactly as written there (rule 3), and `SELECT 1` for /health; every connection prepares
/// them once.
enum Statement: CaseIterable {
    case health
    case feed
    case post
    case insertPost
    case insertLike

    private static let postSelect = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
          FROM posts p JOIN users u ON u.id = p.user_id
        """

    var name: StaticString {
        switch self {
        case .health: "health"
        case .feed: "feed"
        case .post: "post"
        case .insertPost: "insert_post"
        case .insertLike: "insert_like"
        }
    }

    var sql: String {
        switch self {
        case .health: "SELECT 1"
        case .feed: Self.postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
        case .post: Self.postSelect + " WHERE p.id = $1"
        case .insertPost: "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
        case .insertLike: "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
        }
    }

    var parameterTypes: [UInt32] {
        switch self {
        case .health, .feed: []
        case .post: [OID.int8]
        case .insertPost: [OID.int8, OID.text]
        case .insertLike: [OID.int8, OID.int8]
        }
    }
}

/// The outgoing messages of a connection.
struct TxBuf: ~Copyable {
    var buf = ByteBuffer(capacity: 16 * 1024)
    /// Where the open Bind's length goes.
    private var bindAt = 0

    @inline(__always) private mutating func startMessage(_ tag: UInt8) -> Int {
        buf.append(tag)
        let at = buf.count
        buf.append(repeating: 0, count: 4)
        return at
    }

    @inline(__always) private mutating func finishMessage(_ at: Int) {
        buf.storeBigEndian(UInt32(buf.count - at), at: at)
    }

    @inline(__always) private mutating func cstr(_ s: StaticString) {
        buf.append(s)
        buf.append(0)
    }

    @inline(__always) private mutating func i16(_ v: Int16) {
        withUnsafeBytes(of: v.bigEndian) { buf.append($0) }
    }

    @inline(__always) private mutating func i32(_ v: Int32) {
        withUnsafeBytes(of: v.bigEndian) { buf.append($0) }
    }

    mutating func writeParse(_ statement: Statement) {
        let at = startMessage(0x50)
        cstr(statement.name)
        buf.append(statement.sql.utf8Span.span)
        buf.append(0)
        i16(Int16(statement.parameterTypes.count))
        for oid in statement.parameterTypes { i32(Int32(bitPattern: oid)) }
        finishMessage(at)
    }

    /// Opens a Bind of `statement` on the unnamed portal with binary parameters; its `parameters` follow (`int8`,
    /// `text`), then `execute()`.
    @inline(__always) mutating func bind(_ statement: StaticString, parameters: Int16) {
        bindAt = startMessage(0x42)
        buf.append(0)
        cstr(statement)
        if parameters == 0 {
            i16(0)
        } else {
            i16(1)
            i16(1)
        }
        i16(parameters)
    }

    @inline(__always) mutating func int8(_ v: Int64) {
        i32(8)
        withUnsafeBytes(of: v.bigEndian) { buf.append($0) }
    }

    @inline(__always) mutating func text(_ s: Span<UInt8>) {
        i32(Int32(s.count))
        buf.append(s)
    }

    /// Closes the Bind (all results binary) and executes the portal.
    @inline(__always) mutating func execute() {
        i16(1)
        i16(1)
        finishMessage(bindAt)
        let at = startMessage(0x45)
        buf.append(0)
        i32(0)
        finishMessage(at)
    }

    mutating func writeSync() {
        finishMessage(startMessage(0x53))
    }

    mutating func clear() {
        buf.removeAll(shrinkingAbove: 1024 * 1024, to: 64 * 1024)
    }
}

/// One Postgres connection. It owns its socket: noncopyable, closed in `deinit` or by `close()`, never shared.
struct PgConn: ~Copyable {
    private(set) var fd: Int32
    private var rx = ByteBuffer()
    private var rpos = 0
    var tx = TxBuf()
    private var txOff = 0

    deinit { if fd >= 0 { _ = sys_close(fd) } }

    private init(fd: Int32) {
        self.fd = fd
        if fd >= 0 { rx = ByteBuffer(capacity: 64 * 1024) }
    }

    /// No connection: what a pipeline holds before it connects and after its connection is lost.
    static func closed() -> PgConn { PgConn(fd: -1) }

    var isOpen: Bool { fd >= 0 }

    mutating func close() {
        if fd >= 0 { _ = sys_close(fd) }
        fd = -1
        rx.release()
        rpos = 0
        tx.clear()
        txOff = 0
    }

    /// A logged-in connection on a blocking socket, with `timeoutSeconds` on every send and receive until
    /// `makeNonBlocking()`.
    static func connect(_ ep: DbEndpoint, timeoutSeconds: Int = 5) throws(PgError) -> PgConn {
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = Sys.SOCK_STREAM
        var list: UnsafeMutablePointer<addrinfo>?
        let rc = getaddrinfo(ep.host, String(ep.port), &hints, &list)
        guard rc == 0, let first = list else { throw .io("getaddrinfo \(ep.host): \(String(cString: gai_strerror(rc)))") }
        defer { freeaddrinfo(list) }
        var fd: Int32 = -1
        var candidate: UnsafeMutablePointer<addrinfo>? = first
        while let info = candidate, fd < 0 {
            candidate = info.pointee.ai_next
            let s = sys_socket(info.pointee.ai_family, Sys.SOCK_STREAM | Sys.SOCK_CLOEXEC, 0)
            if s < 0 { continue }
            Sys.setTimeouts(s, seconds: timeoutSeconds)
            if Glibc.connect(s, info.pointee.ai_addr, info.pointee.ai_addrlen) == 0 {
                fd = s
            } else {
                _ = sys_close(s)
            }
        }
        guard fd >= 0 else { throw .io("connect \(ep.host):\(ep.port): \(String(cString: strerror(errno)))") }
        Sys.setNoDelay(fd)
        var conn = PgConn(fd: fd)

        var startup: [UInt8] = [0, 0, 0, 0]
        withUnsafeBytes(of: Int32(196_608).bigEndian) { startup.append(contentsOf: $0) }
        // Session settings on our own connection (rule 2): one generic plan per statement from the first execution.
        let params: [(String, String)] = [
            ("user", ep.user), ("database", ep.database), ("client_encoding", "UTF8"), ("DateStyle", "ISO"),
            ("plan_cache_mode", "force_generic_plan"),
        ]
        for (k, v) in params {
            startup.append(contentsOf: k.utf8)
            startup.append(0)
            startup.append(contentsOf: v.utf8)
            startup.append(0)
        }
        startup.append(0)
        withUnsafeBytes(of: Int32(startup.count).bigEndian) { b in for i in 0..<4 { startup[i] = b[i] } }
        try conn.sendBlocking(startup)
        try conn.authenticate(ep)
        return conn
    }

    /// Prepares `statements` (blocking).
    mutating func prepare(_ statements: [Statement]) throws(PgError) {
        for s in statements { tx.writeParse(s) }
        tx.writeSync()
        let fd = self.fd
        let failure = tx.buf.withBytes { raw -> SystemError? in
            do throws(SystemError) { try Sys.writeAll(fd, raw) } catch { return error }
            return nil
        }
        tx.clear()
        if let failure { throw .io("\(failure)") }
        while true {
            let (tag, range) = try nextFrame()
            switch tag {
            case 0x45:
                let err = Self.decodeError(rx.span.extracting(range))
                while try nextFrame().tag != 0x5A {}
                throw .server(err)
            case 0x5A:
                return
            default:
                break
            }
        }
    }

    /// From here on the reactor drives the socket: `send()`, `receive()`, `nextMessage()`.
    mutating func makeNonBlocking() {
        Sys.setTimeouts(fd, seconds: 0)
        _ = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK)
        if rpos == rx.count {
            rx.removeAll()
            rpos = 0
        }
    }

    // ---------- the pipeline (non-blocking) ----------

    enum SendState {
        case done
        /// The socket is full: the rest goes when it is writable.
        case pending
        case failed
    }

    /// Writes what it can of the queued messages.
    mutating func send() -> SendState {
        while txOff < tx.buf.count {
            let fd = self.fd
            let off = txOff
            let n = tx.buf.withBytes { sys_write(fd, $0.baseAddress! + off, $0.count - off) }
            if n < 0 {
                if errno == Sys.EINTR { continue }
                return errno == Sys.EAGAIN ? .pending : .failed
            }
            txOff += n
        }
        tx.clear()
        txOff = 0
        return .done
    }

    /// Reads what has arrived, after the messages not yet taken; false when the connection is gone.
    mutating func receive() -> Bool {
        if rpos > 0 {
            if rpos == rx.count {
                rx.removeAll(shrinkingAbove: 1024 * 1024, to: 64 * 1024)
            } else {
                rx.removeFirst(rpos)
            }
            rpos = 0
        }
        while true {
            let fd = self.fd
            let n = rx.appendSpare(maximum: 64 * 1024) { spare in sys_read(fd, spare.baseAddress, spare.count) }
            if n > 0 {
                // A short read drained the socket (epoll is level-triggered: anything later wakes us again).
                if n < 64 * 1024 { return true }
                continue
            }
            if n == 0 { return false }
            if errno == Sys.EINTR { continue }
            return errno == Sys.EAGAIN
        }
    }

    /// The next complete message in the receive buffer: its tag and the range of its body in `received`.
    @inline(__always) mutating func nextMessage() -> (tag: UInt8, body: Range<Int>)? {
        guard rx.count - rpos >= 5 else { return nil }
        let len = Int(rx.span.bytes.load(fromByteOffset: rpos + 1, as: Int32.self, .bigEndian))
        guard len >= 4, rx.count - rpos >= 1 + len else { return nil }
        let tag = rx[rpos]
        let start = rpos + 5
        rpos += 1 + len
        return (tag, start..<(start + len - 4))
    }

    /// The receive buffer, which `nextMessage()`'s ranges point into until the next `receive()`.
    var received: Span<UInt8> {
        @_lifetime(borrow self) borrowing get { rx.span }
    }

    // ---------- login (blocking) ----------

    private func sendBlocking(_ bytes: [UInt8]) throws(PgError) {
        let fd = self.fd
        // The error comes back as a value: a typed-throws closure through `rethrows` crashes Swift 6.4's SILGen.
        let failure = bytes.withUnsafeBytes { raw -> SystemError? in
            do throws(SystemError) { try Sys.writeAll(fd, raw) } catch { return error }
            return nil
        }
        if let failure { throw .io("\(failure)") }
    }

    private func sendPacket(_ tag: UInt8, _ payload: [UInt8]) throws(PgError) {
        var pkt: [UInt8] = [tag]
        withUnsafeBytes(of: Int32(payload.count + 4).bigEndian) { pkt.append(contentsOf: $0) }
        pkt.append(contentsOf: payload)
        try sendBlocking(pkt)
    }

    /// The next message, waiting for it: its tag and the range of its payload in `rx` (valid until the next call).
    private mutating func nextFrame() throws(PgError) -> (tag: UInt8, payload: Range<Int>) {
        try ensure(5)
        let tag = rx[rpos]
        let len = Int(rx.span.bytes.load(fromByteOffset: rpos + 1, as: Int32.self, .bigEndian))
        try ensure(1 + len)
        let start = rpos + 5
        rpos += 1 + len
        return (tag, start..<(start + len - 4))
    }

    private mutating func ensure(_ needed: Int) throws(PgError) {
        if rx.count - rpos >= needed { return }
        if rpos == rx.count {
            rx.removeAll()
        } else if rpos > 0 {
            rx.removeFirst(rpos)
        }
        rpos = 0
        while rx.count < needed {
            let fd = self.fd
            let n = rx.appendSpare(maximum: max(64 * 1024, needed - rx.count)) { spare in
                sys_read(fd, spare.baseAddress, spare.count)
            }
            if n == 0 { throw .io("postgres connection closed") }
            if n < 0 {
                if errno == Sys.EINTR { continue }
                throw .io("read: \(String(cString: strerror(errno)))")
            }
        }
    }

    private func payload(_ r: Range<Int>) -> [UInt8] { ByteBuffer(copying: rx.span.extracting(r)).take() }

    private mutating func authenticate(_ ep: DbEndpoint) throws(PgError) {
        var scram: (bare: String, nonce: String)?
        var expectedServerSignature: [UInt8]?
        let password = Array(ep.password.utf8)
        while true {
            let (tag, range) = try nextFrame()
            let body = payload(range)
            switch tag {
            case 0x52:
                let method = body.span.bytes.load(fromByteOffset: 0, as: Int32.self, .bigEndian)
                switch method {
                case 0:
                    break
                case 3:
                    try sendPacket(0x70, password + [0])
                case 10:
                    let mechanisms = String(decoding: body[4...], as: UTF8.self)
                    guard mechanisms.split(separator: "\0").contains("SCRAM-SHA-256") else {
                        throw .wire("unsupported SASL mechanisms: \(mechanisms)")
                    }
                    var raw = [UInt8](repeating: 0, count: 18)
                    guard raw.withUnsafeMutableBytes({ Bytes.fillRandom($0) }) else { throw .wire("getrandom failed") }
                    let nonce = String(decoding: Base64.stdEncode(raw), as: UTF8.self)
                    let bare = "n=,r=\(nonce)"
                    let first = Array("n,,\(bare)".utf8)
                    var msg = Array("SCRAM-SHA-256\0".utf8)
                    withUnsafeBytes(of: Int32(first.count).bigEndian) { msg.append(contentsOf: $0) }
                    msg.append(contentsOf: first)
                    try sendPacket(0x70, msg)
                    scram = (bare, nonce)
                case 11:
                    guard let (bare, nonce) = scram else { throw .wire("SASL state") }
                    let serverFirst = String(decoding: body[4...], as: UTF8.self)
                    var rToken: String?
                    var salt: [UInt8]?
                    var iterations: UInt32?
                    for part in serverFirst.split(separator: ",", omittingEmptySubsequences: false) {
                        if part.hasPrefix("r=") {
                            rToken = String(part.dropFirst(2))
                        } else if part.hasPrefix("s=") {
                            salt = Base64.stdDecode(Array(part.dropFirst(2).utf8).span)
                        } else if part.hasPrefix("i=") {
                            iterations = UInt32(part.dropFirst(2))
                        }
                    }
                    guard let rToken, let salt, let iterations, rToken.hasPrefix(nonce) else {
                        throw .wire("invalid SCRAM server-first")
                    }
                    let firstBlock = salt + [0, 0, 0, 1]
                    var u = SHA256.hmac(key: password, firstBlock.span)
                    var salted = u
                    for _ in 1..<max(iterations, 1) {
                        u = SHA256.hmac(key: password, u.span)
                        for b in 0..<32 { salted[b] ^= u[b] }
                    }
                    let saltedKey = Self.digestBytes(salted)
                    let clientKey = SHA256.hmac(key: saltedKey, Array("Client Key".utf8).span)
                    let storedKey = SHA256.digest(clientKey.span)
                    let noProof = "c=biws,r=\(rToken)"
                    let authMessage = Array("\(bare),\(serverFirst),\(noProof)".utf8)
                    let clientSignature = SHA256.hmac(key: Self.digestBytes(storedKey), authMessage.span)
                    var proof = Self.digestBytes(clientKey)
                    for b in 0..<32 { proof[b] ^= clientSignature[b] }
                    let serverKey = SHA256.hmac(key: saltedKey, Array("Server Key".utf8).span)
                    let serverSignature = SHA256.hmac(key: Self.digestBytes(serverKey), authMessage.span)
                    expectedServerSignature = Self.digestBytes(serverSignature)
                    let finalMessage = noProof + ",p=" + String(decoding: Base64.stdEncode(proof), as: UTF8.self)
                    try sendPacket(0x70, Array(finalMessage.utf8))
                case 12:
                    let serverFinal = String(decoding: body[4...], as: UTF8.self)
                    let verifier = serverFinal.hasPrefix("v=")
                        ? Base64.stdDecode(Array(serverFinal.dropFirst(2).utf8).span)
                        : nil
                    guard let verifier, verifier == expectedServerSignature else {
                        throw .wire("SCRAM server signature mismatch")
                    }
                default:
                    throw .wire("unsupported auth code \(method)")
                }
            case 0x45:
                throw .server(Self.decodeError(body.span))
            case 0x5A:
                return
            default:
                break
            }
        }
    }
}

extension PgConn {
    private static func digestBytes(_ digest: SHA256.Digest) -> [UInt8] {
        (0..<32).map { digest[$0] }
    }

    /// An ErrorResponse's SQLSTATE and message.
    static func decodeError(_ body: Span<UInt8>) -> SqlStateErr {
        var code = ""
        var message = ""
        var start = 0
        while start < body.count {
            let end = Bytes.find(body, 0, from: start) ?? body.count
            if end > start {
                let field = Bytes.copyString(body.extracting((start + 1)..<end)) ?? ""
                switch body[start] {
                case 0x43: code = field
                case 0x4D: message = field
                default: break
                }
            }
            start = end + 1
        }
        return SqlStateErr(code: code, message: message)
    }
}
