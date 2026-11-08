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

    /// Whether the statement writes: its response waits for the commit at its Sync (rule 8).
    var writes: Bool {
        switch self {
        case .createPost, .like: true
        case .health, .feed, .post: false
        }
    }
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
    /// The writes of the oldest Sync whose statements completed, with their responses (in `heldBodies`): released by
    /// that Sync's ReadyForQuery, once the implicit transaction has committed (rule 8). The writes a turn sends on
    /// one connection share that Sync: one transaction and one commit for all of them.
    private var held = Fifo<Held>()
    private var heldBodies = ByteBuffer(capacity: 4 * 1024)
    /// Held writes being answered after their commit, one per `next()`.
    private var releasing = 0
    /// A write of the oldest Sync failed, so Postgres rolls back the whole group at its Sync.
    private var groupFailed = false
    /// The writes of a rolled-back group to send again, each alone under a Sync of its own.
    private var retries = Fifo<Waiting>()
    /// Statements were queued here while reading replies (writes sent again): the connection has bytes to send.
    var resent = false
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

    /// A write waiting for its commit, and its response's bytes in `heldBodies`.
    struct Held {
        let waiting: Waiting
        let answer: Answer
        let bytes: Range<Int>
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
        if releasing > 0 { return release() }
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
                let w = waiting.removeFirst()
                let answer = complete(w, range)
                guard w.query.writes else { return answer }
                let start = heldBodies.count
                heldBodies.append(body, 0..<body.count)
                held.append(Held(waiting: w, answer: answer, bytes: start..<heldBodies.count))
            case 0x45:
                let error = PgConn.decodeError(conn.received.extracting(range))
                let group = syncs.first ?? 0
                if group > 0 && answered == group {
                    // Every statement of the Sync completed, so this is the commit failing: none of them happened.
                    if group == 1, !held.isEmpty { return failed(dropHeld().waiting, error) }
                    groupFailed = true
                    while !held.isEmpty { retries.append(dropHeld().waiting) }
                    continue
                }
                guard !waiting.isEmpty else { return nil }
                answered += 1
                let w = waiting.removeFirst()
                guard w.query.writes, group > 1 else { return failed(w, error) }
                // A write of a group failed: Postgres rolls the group back, so the writes before it are undone and
                // the ones after it skipped. They run again alone; so does this one, unless its error is its own
                // for certain (a like of a post that doesn't exist).
                groupFailed = true
                while !held.isEmpty { retries.append(dropHeld().waiting) }
                if case .like = w.query, error.code == "23503" { return failed(w, error) }
                retries.append(w)
            case 0x5A:
                guard !syncs.isEmpty else { continue }
                skipped = syncs.removeFirst() - answered
                answered = 0
                if groupFailed {
                    groupFailed = false
                    while skipped > 0, let w = waiting.first, w.query.writes {
                        skipped -= 1
                        retries.append(waiting.removeFirst())
                    }
                    resendRetries()
                }
                if !held.isEmpty {
                    releasing = held.count
                    return release()
                }
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

    /// The next held write of a committed group, its response copied back into `body`.
    private mutating func release() -> Answer {
        releasing -= 1
        let h = held.removeFirst()
        body.removeAll()
        body.append(heldBodies, h.bytes)
        if held.isEmpty { heldBodies.removeAll() }
        return h.answer
    }

    private mutating func dropHeld() -> Held {
        let h = held.removeFirst()
        if held.isEmpty { heldBodies.removeAll() }
        return h
    }

    /// The writes of a rolled-back group, each sent again alone between two Syncs (after closing the group this turn
    /// has open here, so they don't join it).
    private mutating func resendRetries() {
        guard !retries.isEmpty else { return }
        sync()
        while !retries.isEmpty {
            let w = retries.removeFirst()
            send(w.query, slot: w.slot, epoch: w.epoch)
            sync()
        }
        resent = true
    }

    /// After the connection is lost: the next request still waiting, answered with an error.
    mutating func abandonNext() -> Answer? {
        if releasing > 0 { return release() }
        if !held.isEmpty { return fail(dropHeld().waiting, "connection to Postgres lost before the commit") }
        if !retries.isEmpty { return fail(retries.removeFirst(), "connection to Postgres lost") }
        groupFailed = false
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
