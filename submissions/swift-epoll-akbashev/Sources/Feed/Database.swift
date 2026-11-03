// A Postgres connection as its reactor drives it: the requests waiting for its replies, oldest first, and the
// response each reply becomes. Replies are turned into JSON as they are read, straight out of the connection's receive
// buffer: a DataRow's columns are written into the response, nothing is copied out first.

/// What a request asks Postgres: one statement of SPEC.md each (rule 3).
enum Query {
    case health
    case feed
    case post(id: Int64)
    /// `body` trimmed and checked; `author` is the token's username, which the response echoes.
    case createPost(userID: Int64, body: [UInt8], author: [UInt8])
    case like(userID: Int64, postID: Int64)
}

struct Pipeline: ~Copyable {
    var conn = PgConn.closed()
    /// The connection is waiting to be writable (EPOLLOUT is on).
    var waitingToSend = false
    /// The requests sent and not answered yet, oldest first.
    private var waiting = Fifo<Waiting>()
    /// How many statements each Sync sent covers, oldest first: the first is the one Postgres is answering.
    private var syncs = Fifo<Int>()
    /// Statements queued since the last Sync.
    private(set) var unsynced = 0
    /// Statements of the oldest Sync answered so far.
    private var answered = 0
    /// Statements the last ReadyForQuery left unanswered: Postgres skips the rest of a Sync after an error.
    private var skipped = 0
    /// The response `next()` built (valid until its next call).
    private(set) var body = ByteBuffer(capacity: 16 * 1024)
    private var rows = 0
    /// Process start, for /health's `uptime_s`.
    private let started: UInt64

    struct Waiting {
        let query: Query
        let slot: UInt32
        let epoch: UInt32
    }

    /// A finished response for a connection slot; its body is `body`.
    struct Answer {
        let slot: UInt32
        let epoch: UInt32
        let status: UInt16
    }

    init(started: UInt64) {
        self.started = started
    }

    var pending: Int { waiting.count }

    /// Queues `query`'s Bind and Execute.
    mutating func send(_ query: Query, slot: UInt32, epoch: UInt32) {
        switch query {
        case .health:
            conn.tx.bind(Statement.health.name, parameters: 0)
        case .feed:
            conn.tx.bind(Statement.feed.name, parameters: 0)
        case .post(let id):
            conn.tx.bind(Statement.post.name, parameters: 1)
            conn.tx.int8(id)
        case .createPost(let userID, let text, _):
            conn.tx.bind(Statement.insertPost.name, parameters: 2)
            conn.tx.int8(userID)
            conn.tx.text(text.span)
        case .like(let userID, let postID):
            conn.tx.bind(Statement.insertLike.name, parameters: 2)
            conn.tx.int8(userID)
            conn.tx.int8(postID)
        }
        conn.tx.execute()
        waiting.append(Waiting(query: query, slot: slot, epoch: epoch))
        unsynced += 1
    }

    /// Ends the statements queued since the last Sync with one: one round trip and one implicit transaction for them.
    mutating func sync() {
        guard unsynced > 0 else { return }
        conn.tx.writeSync()
        syncs.append(unsynced)
        unsynced = 0
    }

    /// The next finished response among the messages received, if any.
    mutating func next() -> Answer? {
        if skipped > 0 {
            skipped -= 1
            return fail(waiting.removeFirst(), "statement skipped after an error")
        }
        while let (tag, range) = conn.nextMessage() {
            switch tag {
            case 0x44:
                guard let head = waiting.first else { return nil }
                if rows == 0 { body.removeAll() }
                rows += 1
                writeRow(head.query, range)
            case 0x43:
                guard !waiting.isEmpty else { return nil }
                answered += 1
                return complete(waiting.removeFirst(), range)
            case 0x45:
                guard !waiting.isEmpty else { return nil }
                answered += 1
                let error = PgConn.decodeError(conn.received.extracting(range))
                return failed(waiting.removeFirst(), error)
            case 0x5A:
                guard !syncs.isEmpty else { continue }
                skipped = syncs.removeFirst() - answered
                answered = 0
                if skipped > 0 {
                    skipped -= 1
                    return fail(waiting.removeFirst(), "statement skipped after an error")
                }
            default:
                // ParseComplete, BindComplete, NoData, notices, parameter status.
                continue
            }
        }
        return nil
    }

    /// After the connection is lost: the next request still waiting, answered with an error.
    mutating func abandonNext() -> Answer? {
        guard !waiting.isEmpty else {
            syncs = Fifo()
            unsynced = 0
            answered = 0
            skipped = 0
            rows = 0
            return nil
        }
        return fail(waiting.removeFirst(), "connection to Postgres lost")
    }

    // ---------- responses ----------

    /// A DataRow of the head request's statement, written into `body`.
    private mutating func writeRow(_ query: Query, _ range: Range<Int>) {
        switch query {
        case .feed:
            body.append(rows == 1 ? "{\"posts\":[" : ",")
            writePost(range)
        case .post:
            body.append("{\"post\":")
            writePost(range)
            body.append("}")
        case .createPost(_, let text, let author):
            // RETURNING id, created_at.
            let data = conn.received
            guard let cols = Self.columns(data, range, InlineArray<2, Range<Int>>(repeating: 0..<0)),
                  cols[0].count == 8, cols[1].count == 8 else { return }
            body.append("{\"post\":{\"id\":")
            JSON.writeInt(&body, data.bytes.load(fromByteOffset: cols[0].lowerBound, as: Int64.self, .bigEndian))
            body.append(",\"body\":")
            JSON.writeString(&body, text)
            body.append(",\"created_at\":")
            JSON.writeTimestamp(&body, data.bytes.load(fromByteOffset: cols[1].lowerBound, as: Int64.self, .bigEndian))
            body.append(",\"author\":")
            JSON.writeString(&body, author)
            body.append(",\"like_count\":0}}")
        case .health, .like:
            break
        }
    }

    /// The post object of a row of the post SELECT: id, body, created_at, username, like_count.
    private mutating func writePost(_ range: Range<Int>) {
        let data = conn.received
        guard let cols = Self.columns(data, range, InlineArray<5, Range<Int>>(repeating: 0..<0)),
              cols[0].count == 8, cols[2].count == 8, cols[4].count == 4 else { return }
        let raw = data.bytes
        body.append("{\"id\":")
        JSON.writeInt(&body, raw.load(fromByteOffset: cols[0].lowerBound, as: Int64.self, .bigEndian))
        body.append(",\"body\":")
        JSON.writeString(&body, data.extracting(cols[1]))
        body.append(",\"created_at\":")
        JSON.writeTimestamp(&body, raw.load(fromByteOffset: cols[2].lowerBound, as: Int64.self, .bigEndian))
        body.append(",\"author\":")
        JSON.writeString(&body, data.extracting(cols[3]))
        body.append(",\"like_count\":")
        JSON.writeInt(&body, Int64(raw.load(fromByteOffset: cols[4].lowerBound, as: Int32.self, .bigEndian)))
        body.append("}")
    }

    /// A statement's CommandComplete: its response.
    private mutating func complete(_ w: Waiting, _ range: Range<Int>) -> Answer {
        let rowCount = rows
        rows = 0
        if rowCount == 0 { body.removeAll() }
        switch w.query {
        case .health:
            body.append("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
            JSON.writeUInt(&body, (Sys.monotonicNanos() - started) / 1_000_000_000)
            body.append("}")
            return Answer(slot: w.slot, epoch: w.epoch, status: 200)
        case .feed:
            body.append(rowCount == 0 ? "{\"posts\":[]}" : "]}")
            return Answer(slot: w.slot, epoch: w.epoch, status: 200)
        case .post:
            if rowCount == 0 { return error(w, 404, "post not found") }
            return Answer(slot: w.slot, epoch: w.epoch, status: 200)
        case .createPost:
            if rowCount != 1 || body.isEmpty { return error(w, 500, "internal server error") }
            return Answer(slot: w.slot, epoch: w.epoch, status: 201)
        case .like(_, let postID):
            // "INSERT 0 1" for a new like, "INSERT 0 0" when ON CONFLICT kept the old one.
            let inserted = Self.insertedOne(conn.received.extracting(range))
            body.append(inserted ? "{\"liked\":true,\"already_liked\":false,\"post_id\":" : "{\"liked\":true,\"already_liked\":true,\"post_id\":")
            JSON.writeInt(&body, postID)
            body.append("}")
            return Answer(slot: w.slot, epoch: w.epoch, status: inserted ? 201 : 200)
        }
    }

    /// A statement's ErrorResponse: a like of a post that doesn't exist fails its foreign key (23503, a 404); anything
    /// else is a 500, or /health's 503.
    private mutating func failed(_ w: Waiting, _ error: SqlStateErr) -> Answer {
        rows = 0
        if case .like = w.query, error.code == "23503" { return self.error(w, 404, "post not found") }
        return fail(w, error.message)
    }

    private mutating func fail(_ w: Waiting, _ reason: String) -> Answer {
        rows = 0
        body.removeAll()
        if case .health = w.query {
            body.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
            JSON.writeString(&body, reason.utf8Span.span)
            body.append("}")
            return Answer(slot: w.slot, epoch: w.epoch, status: 503)
        }
        Sys.logLine("postgres: \(reason)")
        return error(w, 500, "internal server error")
    }

    private mutating func error(_ w: Waiting, _ status: UInt16, _ message: StaticString) -> Answer {
        body.removeAll()
        JSON.writeError(&body, message)
        return Answer(slot: w.slot, epoch: w.epoch, status: status)
    }

    /// Whether a CommandComplete tag (NUL-terminated) says one row was inserted.
    private static func insertedOne(_ tag: Span<UInt8>) -> Bool {
        tag.count >= 3 && tag[tag.count - 2] == 0x31 && tag[tag.count - 3] == 0x20
    }

    /// The byte ranges of a DataRow's first `n` columns in `data`; nil if it has fewer or one is NULL.
    private static func columns<let n: Int>(_ data: Span<UInt8>, _ range: Range<Int>, _ empty: InlineArray<n, Range<Int>>)
        -> InlineArray<n, Range<Int>>?
    {
        let raw = data.bytes
        guard range.count >= 2, Int(raw.load(fromByteOffset: range.lowerBound, as: UInt16.self, .bigEndian)) >= n else {
            return nil
        }
        var cols = empty
        var p = range.lowerBound + 2
        for k in 0..<n {
            guard p + 4 <= range.upperBound else { return nil }
            let len = Int(raw.load(fromByteOffset: p, as: Int32.self, .bigEndian))
            p += 4
            guard len >= 0, p + len <= range.upperBound else { return nil }
            cols[k] = p..<(p + len)
            p += len
        }
        return cols
    }
}
