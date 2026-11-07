// A reactor: one per core, an actor on its own event loop that owns everything a request touches. It accepts on its own
// SO_REUSEPORT listeners on both ports into a file descriptor table of its own, and drives its own Postgres
// connections from the same epoll set, so nothing crosses between threads on the way from a request to its response.
// Each turn it takes the ready events off its epoll instance: sockets are read, requests parsed and routed, Postgres
// replies turned into responses. At the end of the turn the turn's reads go out under one Sync on one read
// connection, and the writes on the write connections, each under a Sync of its own.
import Glibc
import Gzip
import Syscalls

actor Reactor {
    nonisolated let loop: EventLoop
    nonisolated var unownedExecutor: UnownedSerialExecutor { loop.asUnownedSerialExecutor() }

    private let signer: TokenSigner
    private let db: DbEndpoint
    private let host: String
    private let ports: [UInt16]
    private let readConnections: Int
    private let started: UInt64
    private let pause: Pause

    /// The listening sockets, one per port.
    private var listeners: [Int32] = []
    private var slots = UniqueArray<ClientSlot>(capacity: 4096)
    private var freeSlots: [UInt32] = []
    private var scratch = ByteBuffer(capacity: 256 * 1024)
    /// Error bodies are written here, and sent from here.
    private var errorBody = ByteBuffer(capacity: 256)
    /// Bodies of at least `gzipMinimum` bytes go out compressed to a client that accepts gzip: the feed, 5 KB to about 2.
    private var gzip = Gzip()
    private var gzipped = ByteBuffer(capacity: 16 * 1024)
    private var pipelinedSlots: [UInt32] = []
    private var events: [UInt8]
    private let maxEvents = 1024

    /// The Postgres connections: `readConnections` for reads, then the write connections.
    private var pipes = UniqueArray<Pipeline>()
    /// The read connection this turn's reads go to, chosen at its first read.
    private var turnReads: Int?
    /// Pipelines with messages queued this turn (a bit each).
    private var queued: UInt64 = 0
    /// When to try lost Postgres connections again (monotonic nanoseconds; 0: none lost).
    private var reconnectAt: UInt64 = 0

    init(loop: EventLoop, signer: TokenSigner, db: DbEndpoint, host: String, ports: [UInt16], readConnections: Int,
         writeConnections: Int, started: UInt64, pause: Pause)
    {
        precondition(readConnections + writeConnections <= 64, "at most 64 Postgres connections per reactor")
        self.loop = loop
        self.signer = signer
        self.db = db
        self.host = host
        self.ports = ports
        self.readConnections = readConnections
        self.started = started
        self.pause = pause
        events = [UInt8](repeating: 0, count: maxEvents * Sys.epollEventStride)
        freeSlots.reserveCapacity(4096)
        pipelinedSlots.reserveCapacity(256)
        pipes = UniqueArray(capacity: readConnections + writeConnections)
        for _ in 0..<(readConnections + writeConnections) { pipes.append(Pipeline(started: started)) }
    }

    @inline(__always) private func ctl(_ op: Int32, _ fd: Int32, _ events: UInt32, _ token: UInt64) -> Bool {
        EventLoop.control(loop.epollFD, op, fd, events, token) == 0
    }

    // ---------- the loop ----------

    func run() async {
        // A descriptor table of this thread's own: a million sockets don't share one table's lock.
        Sys.unshareFiles()
        for (i, port) in ports.enumerated() {
            let fd: Int32
            do {
                fd = try Sys.bindListener(host: host, port: port)
            } catch {
                fatalError("failed to bind listener: \(error)")
            }
            guard ctl(Sys.EPOLL_CTL_ADD, fd, Sys.EPOLLIN, Self.tokenListener - UInt64(i)) else { fatalError("epoll_ctl listener: \(errno)") }
            listeners.append(fd)
        }
        reconnect()
        while true {
            // One epoll_wait per turn: it blocks here, on this actor's own thread, until a socket is ready or a job is
            // queued for the actor.
            let (n, jobsWaiting) = loop.waitForEvents(&events)
            for i in 0..<n {
                let base = i * Sys.epollEventStride
                let flags = events.span.bytes.load(fromByteOffset: base, as: UInt32.self)
                let token = events.span.bytes.load(fromByteOffset: base + Sys.epollDataOffset, as: UInt64.self)
                if token >= Self.tokenListener - UInt64(ports.count - 1) {
                    acceptReady(listeners[Int(Self.tokenListener - token)])
                } else if token & Self.tokenPipe != 0 {
                    pipeReady(Int(token & 0xFFFF), flags)
                } else {
                    let slot = UInt32(truncatingIfNeeded: token)
                    if flags & Sys.EPOLLOUT != 0 { flushTx(slot) }
                    if flags & (Sys.EPOLLIN | Sys.EPOLLHUP | Sys.EPOLLERR | Sys.EPOLLRDHUP) != 0 { readReady(slot, flags) }
                }
            }
            endTurn()
            // After a light turn, a pause before the next epoll_wait: what arrives meanwhile is served in one turn, and
            // its reads share one Sync. A busy turn never pauses, so the pause can't cap throughput.
            if n < pause.busyEvents && pause.nanos > 0 { Sys.sleep(nanos: pause.nanos) }
            if jobsWaiting { await Task.yield() }
        }
    }

    /// The turn's reads under one Sync, then every queued message out; lost connections retried once a second.
    private func endTurn() {
        if let p = turnReads {
            pipes[p].sync()
            queued |= 1 << UInt64(p)
            turnReads = nil
        }
        while queued != 0 {
            let p = queued.trailingZeroBitCount
            queued &= queued - 1
            flushPipe(p)
        }
        if reconnectAt != 0 && Sys.monotonicNanos() >= reconnectAt { reconnect() }
    }

    // ---------- Postgres ----------

    /// Connects every pipeline that has no connection (blocking: at start, and after a loss).
    private func reconnect() {
        reconnectAt = 0
        for p in 0..<pipes.count where !pipes[p].conn.isOpen {
            do {
                var conn = try PgConn.connect(db)
                try conn.prepare(Statement.allCases)
                conn.makeNonBlocking()
                let fd = conn.fd
                pipes[p].conn = conn
                pipes[p].waitingToSend = false
                guard ctl(Sys.EPOLL_CTL_ADD, fd, Sys.EPOLLIN | Sys.EPOLLRDHUP, Self.tokenPipe | UInt64(p)) else {
                    pipes[p].conn.close()
                    throw PgError.io("epoll_ctl: \(errno)")
                }
            } catch {
                Sys.logLine("postgres connect failed: \(error); retrying in 1 s")
                reconnectAt = Sys.monotonicNanos() + 1_000_000_000
                return
            }
        }
    }

    /// The read connection this turn's reads go to: the open one with the fewest requests waiting, until it has
    /// `readsPerSync` of them; then they go under a Sync, and the next ones to the next connection, so one backend
    /// doesn't answer a whole busy turn alone.
    private func readPipe() -> Int? {
        if let p = turnReads {
            if pipes[p].unsynced < Self.readsPerSync { return p }
            pipes[p].sync()
            queued |= 1 << UInt64(p)
        }
        turnReads = leastLoaded(0..<readConnections)
        return turnReads
    }

    private func leastLoaded(_ range: Range<Int>) -> Int? {
        var best: Int?
        var load = Int.max
        for p in range where pipes[p].conn.isOpen && pipes[p].pending < load {
            best = p
            load = pipes[p].pending
        }
        return best
    }

    private func flushPipe(_ p: Int) {
        guard pipes[p].conn.isOpen else { return }
        switch pipes[p].conn.send() {
        case .done:
            if pipes[p].waitingToSend {
                pipes[p].waitingToSend = false
                _ = ctl(Sys.EPOLL_CTL_MOD, pipes[p].conn.fd, Sys.EPOLLIN | Sys.EPOLLRDHUP, Self.tokenPipe | UInt64(p))
            }
        case .pending:
            if !pipes[p].waitingToSend {
                pipes[p].waitingToSend = true
                _ = ctl(Sys.EPOLL_CTL_MOD, pipes[p].conn.fd, Sys.EPOLLIN | Sys.EPOLLOUT | Sys.EPOLLRDHUP, Self.tokenPipe | UInt64(p))
            }
        case .failed:
            pipeLost(p)
        }
    }

    private func pipeReady(_ p: Int, _ flags: UInt32) {
        guard pipes[p].conn.isOpen else { return }
        if flags & Sys.EPOLLOUT != 0 { flushPipe(p) }
        guard flags & (Sys.EPOLLIN | Sys.EPOLLHUP | Sys.EPOLLERR | Sys.EPOLLRDHUP) != 0, pipes[p].conn.isOpen else { return }
        let alive = pipes[p].conn.receive()
        while let answer = pipes[p].next() { deliver(answer, from: p) }
        if !alive { pipeLost(p) }
        drainPipelined()
    }

    /// A lost connection: everything waiting on it is answered with an error, and it is retried a second later.
    private func pipeLost(_ p: Int) {
        Sys.logLine("postgres connection lost; reconnecting")
        pipes[p].conn.close()
        pipes[p].waitingToSend = false
        while let answer = pipes[p].abandonNext() { deliver(answer, from: p) }
        if turnReads == p { turnReads = nil }
        queued &= ~(1 << UInt64(p))
        if reconnectAt == 0 { reconnectAt = Sys.monotonicNanos() + 1_000_000_000 }
        drainPipelined()
    }

    /// A response from pipeline `p`, for its connection if that is still the one that asked.
    private func deliver(_ answer: Pipeline.Answer, from p: Int) {
        let s = Int(answer.slot)
        guard s < slots.count, slots[s].fd >= 0, slots[s].epoch == answer.epoch, slots[s].inFlight else { return }
        let hasBufferedRx = !slots[s].rx.isEmpty
        pipes[p].body.withBytes { writeResponse(answer.slot, answer.status, $0) }
        if hasBufferedRx { pipelinedSlots.append(answer.slot) }
    }

    /// Requests that were waiting behind the ones just answered.
    private func drainPipelined() {
        guard !pipelinedSlots.isEmpty else { return }
        let ready = pipelinedSlots
        pipelinedSlots.removeAll(keepingCapacity: true)
        for slot in ready { drainRxBuf(slot) }
    }

    // ---------- connections ----------

    private func acceptReady(_ listener: Int32) {
        while true {
            let client = sys_accept4(listener, nil, nil, Sys.SOCK_NONBLOCK | Sys.SOCK_CLOEXEC)
            if client < 0 {
                switch errno {
                case Sys.EINTR, Sys.ECONNABORTED: continue
                default: return
                }
            }
            Sys.setNoDelay(client)
            let slot: UInt32
            if let idx = freeSlots.popLast() {
                let s = Int(idx)
                slots[s].fd = client
                slots[s].inFlight = false
                slots[s].closeOnDone = false
                slots[s].pollingOut = false
                slots[s].continueSent = false
                slots[s].peerClosed = false
                slots[s].txOff = 0
                slot = idx
            } else {
                slots.append(ClientSlot(fd: client))
                slot = UInt32(slots.count - 1)
            }
            if !ctl(Sys.EPOLL_CTL_ADD, client, Sys.EPOLLIN | Sys.EPOLLRDHUP, UInt64(slot)) { dropConn(slot) }
        }
    }

    private func dropConn(_ slot: UInt32) {
        let s = Int(slot)
        if slots[s].fd < 0 { return }
        _ = sys_close(slots[s].fd)
        slots[s].fd = -1
        let nextEpoch = slots[s].epoch &+ 1
        slots[s].epoch = nextEpoch == Self.epochClosed ? 0 : nextEpoch
        slots[s].rx.release()
        slots[s].tx.release()
        slots[s].txOff = 0
        slots[s].inFlight = false
        freeSlots.append(slot)
    }

    private func readReady(_ slot: UInt32, _ flags: UInt32) {
        let s = Int(slot)
        if slots[s].fd < 0 { return }
        if flags & (Sys.EPOLLHUP | Sys.EPOLLERR) != 0 && flags & Sys.EPOLLIN == 0 {
            dropConn(slot)
            return
        }
        var firstRead = true
        while true {
            if slots[s].fd < 0 { return }
            let fd = slots[s].fd
            let canDirectParse = firstRead && !slots[s].inFlight && slots[s].rx.isEmpty
            firstRead = false
            scratch.removeAll()
            let n = scratch.appendSpare(maximum: 256 * 1024) { spare in sys_read(fd, spare.baseAddress, spare.count) }
            if n < 0 {
                switch errno {
                case Sys.EAGAIN:
                    if flags & (Sys.EPOLLHUP | Sys.EPOLLERR | Sys.EPOLLRDHUP) != 0 {
                        dropConn(slot)
                        return
                    }
                case Sys.EINTR:
                    continue
                default:
                    dropConn(slot)
                    return
                }
                break
            }
            if n == 0 {
                peerDone(slot)
                return
            }
            // Parse straight out of the scratch buffer when the connection has nothing left over: an idle connection
            // keeps no buffer.
            if canDirectParse && n < 256 * 1024 {
                let outcome = HTTP.parseRequest(signer, scratch.span)
                switch consume outcome {
                case .complete(let consumed, let closeAfter, let acceptsGzip, let dispatch):
                    slots[s].continueSent = false
                    slots[s].closeOnDone = closeAfter
                    slots[s].acceptsGzip = acceptsGzip
                    slots[s].inFlight = true
                    if consumed < n { slots[s].rx.append(scratch, consumed..<n) }
                    if handle(dispatch, slot) && slots[s].fd >= 0 && !slots[s].rx.isEmpty { drainRxBuf(slot) }
                case .incomplete(let sendContinue):
                    slots[s].rx.append(scratch, 0..<n)
                    if sendContinue && !slots[s].continueSent {
                        slots[s].continueSent = true
                        writeRaw(slot, "HTTP/1.1 100 Continue\r\n\r\n")
                    }
                case .reject(let status):
                    rejectConn(slot, status)
                }
                return
            }
            slots[s].rx.append(scratch, 0..<n)
            if n < 256 * 1024 { break }
        }
        drainRxBuf(slot)
    }

    /// The peer is done sending (it may have only half-closed): what it sent is answered, then the connection closes.
    /// Its socket stops being polled for input, which epoll would otherwise report again and again.
    private func peerDone(_ slot: UInt32) {
        let s = Int(slot)
        let pending = slots[s].inFlight || slots[s].txOff < slots[s].tx.count || !slots[s].rx.isEmpty
        guard pending else {
            dropConn(slot)
            return
        }
        slots[s].peerClosed = true
        _ = ctl(Sys.EPOLL_CTL_MOD, slots[s].fd, slots[s].pollingOut ? Sys.EPOLLOUT : 0, UInt64(slot))
        if !slots[s].inFlight { drainRxBuf(slot) }
    }

    /// Answers an error now, or queues the statement on a pipeline. Whether it was answered.
    private func handle(_ dispatch: consuming Router.Dispatch, _ slot: UInt32) -> Bool {
        let epoch = slots[Int(slot)].epoch
        switch consume dispatch {
        case .error(let status, let message):
            writeError(slot, status, message)
            return true
        case .read(let query):
            guard let p = readPipe() else { return unavailable(query, slot) }
            pipes[p].send(query, slot: slot, epoch: epoch)
            return false
        case .write(let query):
            guard let p = leastLoaded(readConnections..<pipes.count) else { return unavailable(query, slot) }
            pipes[p].send(query, slot: slot, epoch: epoch)
            pipes[p].sync()
            queued |= 1 << UInt64(p)
            return false
        }
    }

    /// No connection to send `query` on.
    private func unavailable(_ query: Query, _ slot: UInt32) -> Bool {
        if case .health = query {
            errorBody.removeAll()
            errorBody.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no connection to Postgres\"}")
            errorBody.withBytes { writeResponse(slot, 503, $0) }
        } else {
            writeError(slot, 500, "internal server error")
        }
        return true
    }

    /// SPEC.md's `{"error":"<message>"}`.
    private func writeError(_ slot: UInt32, _ status: UInt16, _ message: StaticString) {
        errorBody.removeAll()
        JSON.writeError(&errorBody, message)
        errorBody.withBytes { writeResponse(slot, status, $0) }
    }

    private func drainRxBuf(_ slot: UInt32) {
        let s = Int(slot)
        while true {
            if slots[s].fd < 0 || slots[s].inFlight || slots[s].rx.isEmpty {
                if slots[s].fd >= 0 && slots[s].rx.isEmpty {
                    slots[s].rx.release()
                    if slots[s].peerClosed && !slots[s].inFlight && slots[s].txOff >= slots[s].tx.count { dropConn(slot) }
                }
                return
            }
            let outcome = HTTP.parseRequest(signer, slots[s].rx.span)
            switch consume outcome {
            case .complete(let consumed, let closeAfter, let acceptsGzip, let dispatch):
                slots[s].continueSent = false
                slots[s].closeOnDone = closeAfter
                slots[s].acceptsGzip = acceptsGzip
                slots[s].inFlight = true
                if consumed == slots[s].rx.count { slots[s].rx.release() } else { slots[s].rx.removeFirst(consumed) }
                if !handle(dispatch, slot) { return }
            case .incomplete(let sendContinue):
                // A request the peer will never finish.
                if slots[s].peerClosed {
                    if slots[s].txOff >= slots[s].tx.count { dropConn(slot) } else { slots[s].closeOnDone = true }
                    return
                }
                if sendContinue && !slots[s].continueSent {
                    slots[s].continueSent = true
                    writeRaw(slot, "HTTP/1.1 100 Continue\r\n\r\n")
                }
                return
            case .reject(let status):
                rejectConn(slot, status)
                return
            }
        }
    }

    private func rejectConn(_ slot: UInt32, _ status: UInt16) {
        let s = Int(slot)
        slots[s].closeOnDone = true
        slots[s].inFlight = true
        slots[s].rx.release()
        writeError(slot, status, HTTP.errorMessage(status))
    }

    // ---------- writing ----------

    private func writeResponse(_ slot: UInt32, _ status: UInt16, _ body: UnsafeRawBufferPointer) {
        let s = Int(slot)
        slots[s].inFlight = false
        let close = slots[s].closeOnDone
        // The head in an inline array on the stack: no allocation per response.
        var head = InlineArray<192, UInt8>(repeating: 0)
        var headSpan = head.mutableSpan
        if slots[s].acceptsGzip && body.count >= Self.gzipMinimum {
            gzipped.removeAll()
            _ = gzipped.appendSpare(maximum: Gzip.bound(body.count)) { gzip.compress(body, into: $0) }
            if gzipped.count < body.count {
                let n = HTTP.formatHeader(into: &headSpan, status, gzipped.count, close, gzip: true)
                head.span.withUnsafeBytes { h in
                    gzipped.withBytes { send(slot, UnsafeRawBufferPointer(rebasing: h[0..<n]), $0) }
                }
                updateWriteState(slot)
                return
            }
        }
        let n = HTTP.formatHeader(into: &headSpan, status, body.count, close, gzip: false)
        head.span.withUnsafeBytes { send(slot, UnsafeRawBufferPointer(rebasing: $0[0..<n]), body) }
        updateWriteState(slot)
    }

    /// Writes head and body with one writev, keeping whatever the socket didn't take for EPOLLOUT.
    private func send(_ slot: UInt32, _ header: UnsafeRawBufferPointer, _ body: UnsafeRawBufferPointer) {
        let s = Int(slot)
        if slots[s].tx.count > slots[s].txOff {
            slots[s].tx.append(header)
            slots[s].tx.append(body)
            return
        }
        let total = header.count + body.count
        let rc = withUnsafeTemporaryAllocation(of: iovec.self, capacity: 2) { iov in
            iov[0] = iovec(iov_base: UnsafeMutableRawPointer(mutating: header.baseAddress), iov_len: header.count)
            iov[1] = iovec(iov_base: UnsafeMutableRawPointer(mutating: body.baseAddress), iov_len: body.count)
            return sys_writev(slots[s].fd, iov.baseAddress!, body.isEmpty ? 1 : 2)
        }
        var written = 0
        if rc < 0 {
            if errno != Sys.EAGAIN && errno != Sys.EINTR {
                dropConn(slot)
                return
            }
        } else {
            written = rc
        }
        if written < total {
            slots[s].tx.removeAll()
            slots[s].txOff = 0
            if written < header.count {
                slots[s].tx.append(UnsafeRawBufferPointer(rebasing: header[written...]))
                slots[s].tx.append(body)
            } else {
                slots[s].tx.append(UnsafeRawBufferPointer(rebasing: body[(written - header.count)...]))
            }
        }
    }

    private func writeRaw(_ slot: UInt32, _ raw: StaticString) {
        let s = Int(slot)
        let bytes = UnsafeRawBufferPointer(start: raw.utf8Start, count: raw.utf8CodeUnitCount)
        if slots[s].tx.count > slots[s].txOff {
            slots[s].tx.append(bytes)
        } else {
            let rc = sys_write(slots[s].fd, bytes.baseAddress, bytes.count)
            var written = 0
            if rc < 0 {
                if errno != Sys.EAGAIN && errno != Sys.EINTR {
                    dropConn(slot)
                    return
                }
            } else {
                written = rc
            }
            if written < bytes.count {
                slots[s].tx.removeAll()
                slots[s].txOff = 0
                slots[s].tx.append(UnsafeRawBufferPointer(rebasing: bytes[written...]))
            }
        }
        updateWriteState(slot)
    }

    private func flushTx(_ slot: UInt32) {
        let s = Int(slot)
        if slots[s].fd < 0 { return }
        while slots[s].txOff < slots[s].tx.count {
            let fd = slots[s].fd
            let off = slots[s].txOff
            let rc = slots[s].tx.withBytes { sys_write(fd, $0.baseAddress! + off, $0.count - off) }
            if rc < 0 {
                if errno == Sys.EAGAIN { break }
                if errno == Sys.EINTR { continue }
                dropConn(slot)
                return
            }
            slots[s].txOff += rc
        }
        updateWriteState(slot)
    }

    private func updateWriteState(_ slot: UInt32) {
        let s = Int(slot)
        if slots[s].fd < 0 { return }
        let hasUnsent = slots[s].txOff < slots[s].tx.count
        if !hasUnsent {
            // Nothing left to send: the buffer goes, so an idle connection holds no memory.
            slots[s].tx.release()
            slots[s].txOff = 0
        }
        if !hasUnsent && !slots[s].inFlight && slots[s].closeOnDone {
            dropConn(slot)
            return
        }
        if !hasUnsent && !slots[s].inFlight && slots[s].peerClosed && slots[s].rx.isEmpty {
            dropConn(slot)
            return
        }
        if hasUnsent != slots[s].pollingOut {
            slots[s].pollingOut = hasUnsent
            let input = slots[s].peerClosed ? 0 : Sys.EPOLLIN | Sys.EPOLLRDHUP
            _ = ctl(Sys.EPOLL_CTL_MOD, slots[s].fd, hasUnsent ? input | Sys.EPOLLOUT : input, UInt64(slot))
        }
    }
}

extension Reactor {
    /// The pause after a light turn: `nanos` after a turn of fewer than `busyEvents` events.
    struct Pause: Sendable {
        let nanos: Int
        let busyEvents: Int
    }

    /// Event tokens: a client is its slot number; a pipeline is `tokenPipe | index`; listeners count down from the top.
    private static let tokenListener = UInt64.max - 1
    private static let tokenPipe: UInt64 = 1 << 48
    private static let epochClosed = UInt32.max
    private static let readsPerSync = 64
    /// Smaller bodies (a post, an error) don't shrink enough to be worth it, and some would grow.
    private static let gzipMinimum = 1024

    /// A connection: its socket and buffers (owned, so the slot is noncopyable), and its state.
    struct ClientSlot: ~Copyable {
        var fd: Int32
        var epoch: UInt32 = 0
        /// What arrived past a complete request (a pipelined one, or a partial one), kept between turns.
        var rx = ByteBuffer()
        /// What the socket didn't take yet.
        var tx = ByteBuffer()
        var txOff = 0
        /// A request is with Postgres: further requests wait in `rx`, so responses stay in order.
        var inFlight = false
        var closeOnDone = false
        var pollingOut = false
        var continueSent = false
        /// The peer is done sending: once what it sent is answered, the connection closes.
        var peerClosed = false
        /// The request being answered accepts gzip.
        var acceptsGzip = false
    }
}

/// HTTP/1.1: the response head, and (below) request parsing.
enum HTTP {
    private static let limitHead = 64 * 1024
    private static let limitBody = 1024 * 1024

    private static func reasonPhrase(_ status: UInt16) -> StaticString {
        switch status {
        case 100: "100 Continue"
        case 200: "200 OK"
        case 201: "201 Created"
        case 400: "400 Bad Request"
        case 401: "401 Unauthorized"
        case 404: "404 Not Found"
        case 413: "413 Payload Too Large"
        case 431: "431 Request Header Fields Too Large"
        case 503: "503 Service Unavailable"
        default: "500 Internal Server Error"
        }
    }

    /// The error message of a request rejected before routing.
    static func errorMessage(_ status: UInt16) -> StaticString {
        switch status {
        case 413: "payload too large"
        case 431: "request header fields too large"
        default: "bad request"
        }
    }

    /// The response head (status line, Content-Type, Content-Encoding, Content-Length, Connection) into `out`; returns
    /// its length.
    fileprivate static func formatHeader(into out: inout MutableSpan<UInt8>, _ status: UInt16, _ length: Int, _ close: Bool,
                                         gzip: Bool) -> Int {
        var n = 0
        if status == 200 && !close {
            n = put(&out, n, gzip
                ? "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: "
                : "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
            n = putDecimal(&out, n, length)
            n = put(&out, n, "\r\n\r\n")
        } else {
            n = put(&out, n, "HTTP/1.1 ")
            n = put(&out, n, reasonPhrase(status))
            n = put(&out, n, gzip
                ? "\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: "
                : "\r\nContent-Type: application/json\r\nContent-Length: ")
            n = putDecimal(&out, n, length)
            n = put(&out, n, close ? "\r\nConnection: close\r\n\r\n" : "\r\n\r\n")
        }
        return n
    }

    @inline(__always) private static func put(_ out: inout MutableSpan<UInt8>, _ at: Int, _ text: StaticString) -> Int {
        let bytes = text.utf8Start
        for i in 0..<text.utf8CodeUnitCount { out[at + i] = bytes[i] }
        return at + text.utf8CodeUnitCount
    }

    @inline(__always) private static func putDecimal(_ out: inout MutableSpan<UInt8>, _ at: Int, _ value: Int) -> Int {
        var digits = 1
        var v = value
        while v >= 10 {
            v /= 10
            digits += 1
        }
        v = value
        for i in stride(from: at + digits - 1, through: at, by: -1) {
            out[i] = 0x30 + UInt8(v % 10)
            v /= 10
        }
        return at + digits
    }

    enum ParseOutcome: ~Copyable {
        case complete(consumed: Int, closeAfter: Bool, acceptsGzip: Bool, dispatch: Router.Dispatch)
        case incomplete(sendContinue: Bool)
        case reject(UInt16)
    }
}

// ---------- HTTP/1.1 parsing ----------

extension HTTP {
    @inline(__always) private static func trimOWS(_ s: Span<UInt8>) -> Span<UInt8> {
        var a = 0
        var b = s.count
        while a < b && (s[a] == 0x20 || s[a] == 0x09) { a += 1 }
        while b > a && (s[b - 1] == 0x20 || s[b - 1] == 0x09) { b -= 1 }
        return s.extracting(a..<b)
    }

    @inline(__always) private static func parseUsize(_ s: Span<UInt8>) -> Int? {
        guard s.count > 0 else { return nil }
        var value = 0
        for i in 0..<s.count {
            let b = s[i]
            guard (0x30...0x39).contains(b) else { return nil }
            let (m, o1) = value.multipliedReportingOverflow(by: 10)
            let (v, o2) = m.addingReportingOverflow(Int(b - 0x30))
            if o1 || o2 { return nil }
            value = v
        }
        return value
    }

    @inline(__always) private static func isTokenName(_ s: Span<UInt8>) -> Bool {
        guard s.count > 0 else { return false }
        for i in 0..<s.count {
            let b = s[i]
            if b <= 0x20 || b >= 0x7F { return false }
            switch b {
            case 0x28, 0x29, 0x3C, 0x3E, 0x40, 0x2C, 0x3B, 0x3A, 0x5C, 0x22, 0x2F, 0x5B, 0x5D, 0x3F, 0x3D, 0x7B, 0x7D: return false
            default: continue
            }
        }
        return true
    }

    @inline(__always) private static func equalsIgnoringCase(_ s: Span<UInt8>, _ lower: StaticString) -> Bool {
        guard s.count == lower.utf8CodeUnitCount else { return false }
        let l = lower.utf8Start
        for i in 0..<s.count {
            var c = s[i]
            if (0x41...0x5A).contains(c) { c |= 0x20 }
            if c != l[i] { return false }
        }
        return true
    }

    static func parseRequest(_ signer: TokenSigner, _ buf: Span<UInt8>) -> ParseOutcome {
        guard let reqLineEnd = Bytes.findCRLF(buf) else {
            return buf.count > limitHead ? .reject(431) : .incomplete(sendContinue: false)
        }
        let reqLine = buf.extracting(0..<reqLineEnd)
        guard let sp1 = Bytes.find(reqLine, 0x20) else { return .reject(400) }
        let method = reqLine.extracting(0..<sp1)
        guard isTokenName(method) else { return .reject(400) }
        guard let sp2 = Bytes.find(reqLine, 0x20, from: sp1 + 1) else { return .reject(400) }
        let rawTarget = reqLine.extracting((sp1 + 1)..<sp2)
        guard rawTarget.count > 0 else { return .reject(400) }
        for i in 0..<rawTarget.count where rawTarget[i] <= 0x20 || rawTarget[i] == 0x7F { return .reject(400) }
        let version = reqLine.extracting((sp2 + 1)..<reqLine.count)
        let isHTTP10: Bool
        if Bytes.equal(version, "HTTP/1.1") {
            isHTTP10 = false
        } else if Bytes.equal(version, "HTTP/1.0") {
            isHTTP10 = true
        } else {
            return .reject(400)
        }

        var contentLength: Int?
        var isChunked = false
        var authRange: Range<Int>?
        var keepAlive = !isHTTP10
        var expect100 = false
        var acceptsGzip = false

        var pos = reqLineEnd + 2
        var headEnd = 0
        while true {
            guard let lineEnd = Bytes.findCRLF(buf, from: pos) else {
                return buf.count > limitHead ? .reject(431) : .incomplete(sendContinue: false)
            }
            if lineEnd + 2 > limitHead { return .reject(431) }
            if lineEnd == pos {
                headEnd = pos + 2
                break
            }
            let line = buf.extracting(pos..<lineEnd)
            let lineStart = pos
            pos = lineEnd + 2
            guard let colon = Bytes.find(line, 0x3A) else { return .reject(400) }
            let name = line.extracting(0..<colon)
            guard isTokenName(name) else { return .reject(400) }
            let value = trimOWS(line.extracting((colon + 1)..<line.count))
            switch name.count {
            case 13 where equalsIgnoringCase(name, "authorization"):
                // The value's place in `buf` (after the leading whitespace trimOWS skipped), so the request borrows it.
                var a = colon + 1
                while a < line.count && (line[a] == 0x20 || line[a] == 0x09) { a += 1 }
                let valueStart = lineStart + a
                authRange = valueStart..<(valueStart + value.count)
            case 14 where equalsIgnoringCase(name, "content-length"):
                guard let n = parseUsize(value) else { return .reject(400) }
                if let prev = contentLength, prev != n { return .reject(400) }
                contentLength = n
            case 10 where equalsIgnoringCase(name, "connection"):
                if equalsIgnoringCase(value, "close") {
                    keepAlive = false
                } else if equalsIgnoringCase(value, "keep-alive") {
                    keepAlive = true
                }
            case 17 where equalsIgnoringCase(name, "transfer-encoding"):
                isChunked = equalsIgnoringCase(value, "chunked")
                if !isChunked { return .reject(400) }
            case 6 where equalsIgnoringCase(name, "expect"):
                expect100 = equalsIgnoringCase(value, "100-continue")
            case 15 where equalsIgnoringCase(name, "accept-encoding"):
                acceptsGzip = acceptsGzipCoding(value)
            default:
                break
            }
        }

        if isChunked && contentLength != nil { return .reject(400) }

        let path = rawTarget.extracting(0..<(Bytes.find(rawTarget, 0x3F) ?? rawTarget.count))
        var auth: Span<UInt8>? = nil
        if let authRange { auth = buf.extracting(authRange) }

        if isChunked {
            switch decodeChunkedBody(buf.extracting(headEnd..<buf.count)) {
            case .complete(let decoded, let used):
                if decoded.count > limitBody { return .reject(413) }
                let req = Request(method: method, path: path, auth: auth, body: decoded.span)
                let dispatch = Router.route(signer, req)
                return .complete(consumed: headEnd + used, closeAfter: !keepAlive, acceptsGzip: acceptsGzip, dispatch: dispatch)
            case .incomplete:
                if buf.count - headEnd > limitBody + 64 * 1024 { return .reject(413) }
                return .incomplete(sendContinue: expect100)
            case .malformed:
                return .reject(400)
            }
        }
        let cl = contentLength ?? 0
        if cl > limitBody { return .reject(413) }
        if buf.count < headEnd + cl { return .incomplete(sendContinue: expect100) }
        let req = Request(method: method, path: path, auth: auth, body: buf.extracting(headEnd..<(headEnd + cl)))
        let dispatch = Router.route(signer, req)
        return .complete(consumed: headEnd + cl, closeAfter: !keepAlive, acceptsGzip: acceptsGzip, dispatch: dispatch)
    }

    /// Whether an Accept-Encoding value lists gzip (or `*`) without `q=0`.
    private static func acceptsGzipCoding(_ value: Span<UInt8>) -> Bool {
        var start = 0
        while start <= value.count {
            let end = Bytes.find(value, 0x2C, from: start) ?? value.count
            let item = trimOWS(value.extracting(start..<end))
            let semicolon = Bytes.find(item, 0x3B) ?? item.count
            let coding = trimOWS(item.extracting(0..<semicolon))
            if equalsIgnoringCase(coding, "gzip") || equalsIgnoringCase(coding, "x-gzip") || equalsIgnoringCase(coding, "*") {
                return !isZeroQuality(item.extracting(semicolon..<item.count))
            }
            start = end + 1
        }
        return false
    }

    /// Parameters `;q=0`, `;q=0.0`, … (the coding refused).
    private static func isZeroQuality(_ params: Span<UInt8>) -> Bool {
        var i = 0
        while i + 1 < params.count {
            if (params[i] == 0x71 || params[i] == 0x51) && params[i + 1] == 0x3D {
                var j = i + 2
                guard j < params.count, params[j] == 0x30 else { return false }
                j += 1
                if j < params.count && params[j] == 0x2E {
                    j += 1
                    while j < params.count && params[j] == 0x30 { j += 1 }
                }
                return j == params.count || params[j] == 0x3B || params[j] == 0x20
            }
            i += 1
        }
        return false
    }

    private enum ChunkResult {
        case complete([UInt8], Int)
        case incomplete
        case malformed
    }

    private static func decodeChunkedBody(_ src: Span<UInt8>) -> ChunkResult {
        var out: [UInt8] = []
        var pos = 0
        while true {
            guard let crlf = Bytes.findCRLF(src, from: pos) else { return .incomplete }
            let line = src.extracting(pos..<crlf)
            let hexPart = trimOWS(line.extracting(0..<(Bytes.find(line, 0x3B) ?? line.count)))
            guard hexPart.count > 0, hexPart.count <= 8 else { return .malformed }
            var chunkLen = 0
            for i in 0..<hexPart.count {
                let b = hexPart[i]
                let d: Int
                switch b {
                case 0x30...0x39: d = Int(b - 0x30)
                case 0x61...0x66: d = Int(b - 0x61 + 10)
                case 0x41...0x46: d = Int(b - 0x41 + 10)
                default: return .malformed
                }
                chunkLen = chunkLen << 4 | d
            }
            pos = crlf + 2
            if chunkLen == 0 {
                while true {
                    guard let endLine = Bytes.findCRLF(src, from: pos) else { return .incomplete }
                    let empty = endLine == pos
                    pos = endLine + 2
                    if empty { return .complete(out, pos) }
                }
            }
            if chunkLen > limitBody || out.count + chunkLen > limitBody {
                return .complete([UInt8](repeating: 0, count: limitBody + 1), pos)
            }
            if src.count < pos + chunkLen + 2 { return .incomplete }
            src.extracting(pos..<(pos + chunkLen)).withUnsafeBufferPointer { out.append(contentsOf: $0) }
            if src[pos + chunkLen] != 0x0D || src[pos + chunkLen + 1] != 0x0A { return .malformed }
            pos += chunkLen + 2
        }
    }
}
