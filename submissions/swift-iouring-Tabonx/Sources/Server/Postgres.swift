// A small PostgreSQL wire client: startup and SCRAM-SHA-256 on a blocking socket, then the extended
// protocol with named statements and binary rows through the worker's ring.

#if os(Linux)
import CShim

/// The parts of DATABASE_URL, as C strings.
final class DatabaseURL: Sendable {
    let host: [CChar]
    let port: Int32
    let user: [UInt8]
    let password: [UInt8]
    let database: [UInt8]

    /// Parses `postgres://user:password@host:port/database?options`.
    init?(_ url: UnsafePointer<CChar>) {
        let text = Array(UnsafeBufferPointer(start: UnsafeRawPointer(url).assumingMemoryBound(to: UInt8.self), count: strlen(url)))
        var i = 0
        let scheme: [UInt8] = Array("://".utf8)
        while i &+ 3 <= text.count, Array(text[i..<i &+ 3]) != scheme { i &+= 1 }
        guard i &+ 3 <= text.count else { return nil }
        i &+= 3
        guard let at = text[i...].lastIndex(of: UInt8(ascii: "@")) else { return nil }
        let credentials = text[i..<at]
        let colon = credentials.firstIndex(of: UInt8(ascii: ":")) ?? at
        user = Self.percentDecoded(text[i..<colon])
        password = colon < at ? Self.percentDecoded(text[(colon &+ 1)..<at]) : []
        var rest = text[(at &+ 1)...]
        if let query = rest.firstIndex(of: UInt8(ascii: "?")) { rest = rest[..<query] }
        guard let slash = rest.firstIndex(of: UInt8(ascii: "/")) else { return nil }
        let authority = rest[..<slash]
        database = Self.percentDecoded(rest[(slash &+ 1)...])
        var hostBytes = authority
        var parsedPort: Int32 = 5432
        if let portColon = authority.lastIndex(of: UInt8(ascii: ":")) {
            hostBytes = authority[..<portColon]
            parsedPort = 0
            for b in authority[(portColon &+ 1)...] {
                guard b &- 48 < 10 else { return nil }
                parsedPort = parsedPort &* 10 &+ Int32(b &- 48)
            }
        }
        host = hostBytes.map { CChar(bitPattern: $0) } + [0]
        port = parsedPort
    }

    static func percentDecoded(_ s: ArraySlice<UInt8>) -> [UInt8] {
        var out: [UInt8] = []
        var i = s.startIndex
        while i < s.endIndex {
            if s[i] == UInt8(ascii: "%"), i &+ 2 < s.endIndex {
                let hi = hexValue(s[i &+ 1])
                let lo = hexValue(s[i &+ 2])
                if hi >= 0, lo >= 0 {
                    out.append(UInt8(hi &* 16 &+ lo))
                    i &+= 3
                    continue
                }
            }
            out.append(s[i])
            i &+= 1
        }
        return out
    }
}

/// A blocking connection during startup. Everything after it goes through the ring.
struct PgHandshake {
    let fd: Int32
    var input = [UInt8](repeating: 0, count: 8192)
    var have = 0
    var used = 0

    func send(_ bytes: [UInt8]) -> Bool {
        var sent = 0
        while sent < bytes.count {
            let n = bytes.withUnsafeBytes { write(fd, $0.baseAddress.unsafelyUnwrapped + sent, bytes.count &- sent) }
            if n <= 0 { return false }
            sent &+= n
        }
        return true
    }

    /// Returns the next backend message, or nil when the socket fails.
    mutating func next() -> (type: UInt8, body: [UInt8])? {
        while true {
            if have &- used >= 5 {
                let length = Int(input[used &+ 1]) << 24 | Int(input[used &+ 2]) << 16 | Int(input[used &+ 3]) << 8 | Int(input[used &+ 4])
                if length < 4 { return nil }
                if have &- used >= 1 &+ length {
                    let type = input[used]
                    let body = Array(input[(used &+ 5)..<(used &+ 1 &+ length)])
                    used &+= 1 &+ length
                    return (type, body)
                }
                if 1 &+ length > input.count { input += [UInt8](repeating: 0, count: 1 &+ length) }
            }
            if used > 0 {
                input.withUnsafeMutableBytes { raw in
                    let p = raw.baseAddress.unsafelyUnwrapped
                    memmove(p, p + used, have &- used)
                }
                have &-= used
                used = 0
            }
            let n = input.withUnsafeMutableBytes { read(fd, $0.baseAddress.unsafelyUnwrapped + have, $0.count &- have) }
            if n <= 0 { return nil }
            have &+= n
        }
    }
}

func pgMessage(_ type: UInt8?, _ body: [UInt8]) -> [UInt8] {
    let length = UInt32(body.count &+ 4)
    var out: [UInt8] = []
    if let type { out.append(type) }
    out += [UInt8(length >> 24), UInt8(truncatingIfNeeded: length >> 16), UInt8(truncatingIfNeeded: length >> 8), UInt8(truncatingIfNeeded: length)]
    return out + body
}

func hmac(_ key: [UInt8], _ message: [UInt8]) -> [UInt8] {
    var out = [UInt8](repeating: 0, count: 32)
    key.withUnsafeBufferPointer { k in
        message.withUnsafeBufferPointer { m in
            let h = HMACSHA256(key: ByteView(base: k.baseAddress, count: k.count))
            out.withUnsafeMutableBufferPointer { h.mac(ByteView(base: m.baseAddress, count: m.count), into: $0.baseAddress.unsafelyUnwrapped) }
        }
    }
    return out
}

func sha256(_ message: [UInt8]) -> [UInt8] {
    var out = [UInt8](repeating: 0, count: 32)
    let empty: [UInt8] = [0]
    empty.withUnsafeBufferPointer { k in
        message.withUnsafeBufferPointer { m in
            let h = HMACSHA256(key: ByteView(base: k.baseAddress, count: 0))
            out.withUnsafeMutableBufferPointer { h.sha256(ByteView(base: m.baseAddress, count: m.count), into: $0.baseAddress.unsafelyUnwrapped) }
        }
    }
    return out
}

let base64Alphabet = Array("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".utf8)

func base64Encoded(_ bytes: [UInt8]) -> [UInt8] {
    var out: [UInt8] = []
    var i = 0
    while i < bytes.count {
        let a = UInt32(bytes[i])
        let b = i &+ 1 < bytes.count ? UInt32(bytes[i &+ 1]) : 0
        let c = i &+ 2 < bytes.count ? UInt32(bytes[i &+ 2]) : 0
        let v = a << 16 | b << 8 | c
        out.append(base64Alphabet[Int(v >> 18)])
        out.append(base64Alphabet[Int(v >> 12 & 63)])
        out.append(i &+ 1 < bytes.count ? base64Alphabet[Int(v >> 6 & 63)] : UInt8(ascii: "="))
        out.append(i &+ 2 < bytes.count ? base64Alphabet[Int(v & 63)] : UInt8(ascii: "="))
        i &+= 3
    }
    return out
}

func base64Decoded(_ text: ArraySlice<UInt8>) -> [UInt8] {
    var out: [UInt8] = []
    var bits: UInt32 = 0
    var count = 0
    for c in text {
        guard let v = base64Alphabet.firstIndex(of: c) else { continue }
        bits = bits << 6 | UInt32(v)
        count &+= 1
        if count == 4 {
            out += [UInt8(truncatingIfNeeded: bits >> 16), UInt8(truncatingIfNeeded: bits >> 8), UInt8(truncatingIfNeeded: bits)]
            bits = 0
            count = 0
        }
    }
    if count == 3 {
        out += [UInt8(truncatingIfNeeded: bits >> 10), UInt8(truncatingIfNeeded: bits >> 2)]
    } else if count == 2 {
        out.append(UInt8(truncatingIfNeeded: bits >> 4))
    }
    return out
}

/// The value of `name=` in a comma-separated SCRAM message.
func scramField(_ message: [UInt8], _ name: UInt8) -> ArraySlice<UInt8>? {
    for part in message.split(separator: UInt8(ascii: ",")) where part.count >= 2 {
        if part[part.startIndex] == name, part[part.startIndex &+ 1] == UInt8(ascii: "=") {
            return part[(part.startIndex &+ 2)...]
        }
    }
    return nil
}

/// Answers SCRAM-SHA-256 (RFC 5802, RFC 7677). True once the server accepts and its signature checks.
func scramAuthenticate(_ hs: inout PgHandshake, password: [UInt8]) -> Bool {
    var random = [UInt8](repeating: 0, count: 18)
    random.withUnsafeMutableBytes { sv_random($0.baseAddress, 18) }
    let nonce = base64Encoded(random)
    let clientFirstBare = Array("n=,r=".utf8) + nonce
    let clientFirst = Array("n,,".utf8) + clientFirstBare
    var initial = Array("SCRAM-SHA-256".utf8) + [0]
    let length = UInt32(clientFirst.count)
    initial += [UInt8(length >> 24), UInt8(truncatingIfNeeded: length >> 16), UInt8(truncatingIfNeeded: length >> 8), UInt8(truncatingIfNeeded: length)]
    initial += clientFirst
    guard hs.send(pgMessage(UInt8(ascii: "p"), initial)) else { return false }

    guard let continuing = hs.next(), continuing.type == UInt8(ascii: "R"), continuing.body.count > 4 else { return false }
    let serverFirst = Array(continuing.body[4...])
    guard let combined = scramField(serverFirst, UInt8(ascii: "r")), combined.starts(with: nonce),
          let saltText = scramField(serverFirst, UInt8(ascii: "s")),
          let iterationsText = scramField(serverFirst, UInt8(ascii: "i")) else { return false }
    var iterations = 0
    for b in iterationsText { iterations = iterations &* 10 &+ Int(b &- 48) }

    // PBKDF2-HMAC-SHA256, one 32-byte block.
    var u = hmac(password, base64Decoded(saltText) + [0, 0, 0, 1])
    var salted = u
    for _ in 1..<max(iterations, 1) {
        u = hmac(password, u)
        for k in 0..<32 { salted[k] ^= u[k] }
    }
    let clientKey = hmac(salted, Array("Client Key".utf8))
    let storedKey = sha256(clientKey)
    let clientFinalBare = Array("c=biws,r=".utf8) + Array(combined)
    let authMessage = clientFirstBare + [UInt8(ascii: ",")] + serverFirst + [UInt8(ascii: ",")] + clientFinalBare
    let signature = hmac(storedKey, authMessage)
    var proof = clientKey
    for k in 0..<32 { proof[k] ^= signature[k] }
    let clientFinal = clientFinalBare + Array(",p=".utf8) + base64Encoded(proof)
    guard hs.send(pgMessage(UInt8(ascii: "p"), clientFinal)) else { return false }

    guard let final = hs.next(), final.type == UInt8(ascii: "R"), final.body.count > 4 else { return false }
    guard let verifier = scramField(Array(final.body[4...]), UInt8(ascii: "v")) else { return false }
    return base64Decoded(verifier) == hmac(hmac(salted, Array("Server Key".utf8)), authMessage)
}

let postSelect = """
    SELECT p.id, p.body, p.created_at, u.username, \
    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
    FROM posts p JOIN users u ON u.id = p.user_id
    """
/// Name and text of every statement the server runs, from SPEC.md.
let statements: [(name: String, sql: String)] = [
    ("f", postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"),
    ("p", postSelect + " WHERE p.id = $1"),
    ("c", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"),
    ("l", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"),
    ("h", "SELECT 1"),
]

/// Opens, authenticates and prepares a connection. Returns the socket, or -1.
func pgConnect(_ url: DatabaseURL) -> Int32 {
    let fd = url.host.withUnsafeBufferPointer { sv_connect($0.baseAddress, url.port, 2000) }
    guard fd >= 0 else { return -1 }
    var hs = PgHandshake(fd: fd)
    guard pgHandshake(&hs, url) else {
        _ = close(fd)
        return -1
    }
    sv_set_nonblocking(fd)
    return fd
}

func pgHandshake(_ hs: inout PgHandshake, _ url: DatabaseURL) -> Bool {
    var startup: [UInt8] = [0, 3, 0, 0]
    startup += Array("user".utf8) + [0] + url.user + [0]
    startup += Array("database".utf8) + [0] + url.database + [0]
    startup += Array("client_encoding".utf8) + [0] + Array("UTF8".utf8) + [0, 0]
    guard hs.send(pgMessage(nil, startup)) else { return false }

    while true {
        guard let m = hs.next() else { return false }
        if m.type == UInt8(ascii: "Z") { break }
        if m.type == UInt8(ascii: "E") { return false }
        guard m.type == UInt8(ascii: "R") else { continue } // ParameterStatus, BackendKeyData
        guard m.body.count >= 4 else { return false }
        switch m.body[3] {
        case 0:
            break
        case 3:
            guard hs.send(pgMessage(UInt8(ascii: "p"), url.password + [0])) else { return false }
        case 10:
            guard scramAuthenticate(&hs, password: url.password) else { return false }
        default:
            return false // md5 and the rest: the box uses scram-sha-256
        }
    }

    var prepare: [UInt8] = []
    for s in statements {
        prepare += pgMessage(UInt8(ascii: "P"), Array(s.name.utf8) + [0] + Array(s.sql.utf8) + [0, 0, 0])
    }
    prepare += pgMessage(UInt8(ascii: "S"), [])
    guard hs.send(prepare) else { return false }
    while let m = hs.next() {
        if m.type == UInt8(ascii: "E") { return false }
        if m.type == UInt8(ascii: "Z") { return true }
    }
    return false
}

enum PendingKind: UInt8 {
    /// Marks a Sync on a read connection: everything before it belongs to one pack.
    case sync
    case feed, post, health, createPost, like
}

/// A query on its way, and the HTTP request waiting for it.
struct Pending {
    var kind: PendingKind
    var close = false
    var gzip = false
    var generation: UInt16 = 0
    var ring: Int32 = 0
    var slot: UInt32 = 0
    var postID: Int64 = 0
    /// createPost: the body as JSON, then the author, for the 201.
    var text: UnsafeMutablePointer<UInt8>? = nil
    var bodyCount: Int32 = 0
    var authorCount: Int32 = 0
    /// Writes: set by CommandComplete or ErrorResponse, answered at ReadyForQuery.
    var inserted = false
    var finished = false
    var foreignKeyViolation = false
}

let pgInputCapacity = 1 << 19

final class PgConnection {
    let index: UInt32
    let reader: Bool
    var fd: Int32 = -1
    var generation: UInt16 = 0
    /// Bytes received and not parsed yet.
    let input: UnsafeMutablePointer<UInt8>
    var inputCount = 0
    /// Queued for Postgres, and the buffer in flight.
    var out = OutBuffer(capacity: 1 << 16)
    var sending = OutBuffer(capacity: 1 << 16)
    var sent = 0
    var sendActive = false
    /// Reads in the pack that is still open.
    var packCount = 0
    /// An error aborted the pipeline: Postgres skips everything up to the next Sync.
    var aborted = false
    /// The row being written, for the query at the head of the queue.
    var rows = 0
    var scratch = OutBuffer(capacity: 8192)

    var queue: UnsafeMutablePointer<Pending>
    var queueCapacity = 1024
    var queueHead = 0
    var queueCount = 0

    init(index: UInt32, reader: Bool) {
        self.index = index
        self.reader = reader
        input = .allocate(capacity: pgInputCapacity)
        queue = .allocate(capacity: queueCapacity)
    }

    var alive: Bool { fd >= 0 }

    @inline(__always)
    func push(_ p: Pending) {
        if queueCount == queueCapacity { growQueue() }
        queue[(queueHead &+ queueCount) & (queueCapacity &- 1)] = p
        queueCount &+= 1
    }

    @inline(never)
    func growQueue() {
        let bigger = UnsafeMutablePointer<Pending>.allocate(capacity: queueCapacity &* 2)
        for i in 0..<queueCount { bigger[i] = queue[(queueHead &+ i) & (queueCapacity &- 1)] }
        queue.deallocate()
        queue = bigger
        queueHead = 0
        queueCapacity &*= 2
    }

    @inline(__always)
    var head: UnsafeMutablePointer<Pending> { queue + queueHead }

    @inline(__always)
    func pop() {
        queueHead = (queueHead &+ 1) & (queueCapacity &- 1)
        queueCount &-= 1
    }
}

extension OutBuffer {
    @inline(__always)
    mutating func appendBE32(_ v: Int32) {
        reserve(4)
        UnsafeMutableRawPointer(base + count).storeBytes(of: v.bigEndian, as: Int32.self)
        count &+= 4
    }

    @inline(__always)
    mutating func appendBE64(_ v: Int64) {
        reserve(8)
        UnsafeMutableRawPointer(base + count).storeBytes(of: v.bigEndian, as: Int64.self)
        count &+= 8
    }

    /// Bind for statement `name` with binary parameters and binary results, then Execute.
    /// `parameters` writes each parameter as length and value.
    @inline(__always)
    mutating func appendBindExecute(_ name: StaticString, parameterCount: Int, parameters: (inout OutBuffer) -> Void) {
        append(UInt8(ascii: "B"))
        let lengthAt = count
        appendBE32(0)
        append(0) // unnamed portal
        append(name)
        append(0)
        append(0); append(1); append(0); append(1) // one format code: binary, for all parameters
        append(0); append(UInt8(truncatingIfNeeded: parameterCount))
        parameters(&self)
        append(0); append(1); append(0); append(1) // one format code: binary, for all columns
        let length = Int32(truncatingIfNeeded: count &- lengthAt)
        UnsafeMutableRawPointer(base + lengthAt).storeBytes(of: length.bigEndian, as: Int32.self)
        append("E\0\0\0\u{9}\0\0\0\0\0")
    }

    @inline(__always)
    mutating func appendSync() {
        append("S\0\0\0\u{4}")
    }
}

@inline(__always)
func readBE32(_ p: UnsafePointer<UInt8>) -> Int32 {
    Int32(bigEndian: UnsafeRawPointer(p).loadUnaligned(as: Int32.self))
}

@inline(__always)
func readBE64(_ p: UnsafePointer<UInt8>) -> Int64 {
    Int64(bigEndian: UnsafeRawPointer(p).loadUnaligned(as: Int64.self))
}

extension OutBuffer {
    /// A TIMESTAMPTZ (microseconds since 2000-01-01 UTC) as `2025-12-31T23:59:19.409Z`, truncated
    /// to milliseconds.
    mutating func appendTimestamp(_ microseconds: Int64) {
        reserve(26)
        var ms = microseconds / 1000
        if microseconds % 1000 < 0 { ms &-= 1 }
        var days = ms / 86_400_000
        var msOfDay = ms % 86_400_000
        if msOfDay < 0 {
            msOfDay &+= 86_400_000
            days &-= 1
        }
        // Days since 1970, civil calendar (Howard Hinnant's civil_from_days).
        let z = days &+ 10957 &+ 719_468
        let era = (z >= 0 ? z : z &- 146_096) / 146_097
        let doe = z &- era &* 146_097
        let yoe = (doe &- doe / 1460 &+ doe / 36524 &- doe / 146_096) / 365
        let doy = doe &- (365 &* yoe &+ yoe / 4 &- yoe / 100)
        let mp = (5 &* doy &+ 2) / 153
        let day = doy &- (153 &* mp &+ 2) / 5 &+ 1
        let month = mp < 10 ? mp &+ 3 : mp &- 9
        let year = yoe &+ era &* 400 &+ (month <= 2 ? 1 : 0)
        let hour = msOfDay / 3_600_000
        let minute = msOfDay / 60000 % 60
        let second = msOfDay / 1000 % 60
        let milli = msOfDay % 1000
        let o = base + count
        @inline(__always) func two(_ at: Int, _ v: Int64) {
            o[at] = UInt8(truncatingIfNeeded: v / 10) &+ 48
            o[at &+ 1] = UInt8(truncatingIfNeeded: v % 10) &+ 48
        }
        o[0] = 0x22
        two(1, year / 100)
        two(3, year % 100)
        o[5] = 0x2D
        two(6, month)
        o[8] = 0x2D
        two(9, day)
        o[11] = 0x54
        two(12, hour)
        o[14] = 0x3A
        two(15, minute)
        o[17] = 0x3A
        two(18, second)
        o[20] = 0x2E
        o[21] = UInt8(truncatingIfNeeded: milli / 100) &+ 48
        two(22, milli % 100)
        o[24] = 0x5A
        o[25] = 0x22
        count &+= 26
    }
}
#endif
