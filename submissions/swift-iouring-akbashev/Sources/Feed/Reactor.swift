// A reactor: one per core, an actor on its own event loop that owns everything a request touches. It accepts on its own
// SO_REUSEPORT listeners on both ports into a file descriptor table of its own, and drives its own Postgres
// connections from the same io_uring, so nothing crosses between threads on the way from a request to its response.
// Each turn it takes the completions off its ring: a multishot accept per listener, a multishot receive per connection
// into the ring's provided buffers (an idle connection holds none), polls for the Postgres sockets; requests are
// parsed and routed, Postgres replies turned into responses, and responses written with writev. At the end of the turn
// the turn's reads go out under one Sync on one read connection, and its writes under one Sync on one write
// connection: one transaction and one commit for them.
import Glibc
import Gzip
import IORing
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
    /// Error bodies are written here, and sent from here.
    private var errorBody = ByteBuffer(capacity: 256)
    /// Bodies of at least `gzipMinimum` bytes go out compressed to a client that accepts gzip: the feed, 5 KB to about 2.
    private var gzip = Gzip()
    private var gzipped = ByteBuffer(capacity: 16 * 1024)
    private var pipelinedSlots: [UInt32] = []
    /// A turn that took this many completions goes straight on to the next (see `run`).
    private let maxEvents = 1024
    /// This reactor's ring, made on its own thread in `run`: one issuer, and completions posted only when it asks
    /// (SINGLE_ISSUER, DEFER_TASKRUN).
    private var ring: IORing?
    /// The provided buffers multishot receives fill: a buffer is taken when bytes arrive and given back once parsed.
    private var bufferGroup: UInt16 = 0
    private let bufferCount: Int
    private let bufferSize: Int
    /// The eventfd's 8 bytes, where the wake read lands.
    private let wakeBytes = UnsafeMutableRawBufferPointer.allocate(byteCount: 8, alignment: 8)
    /// Connections whose multishot receive ended for want of a buffer: armed again once the turn gave its back.
    private var starved: [(slot: UInt32, epoch: UInt32)] = []
    /// Each pipeline's connection, counted, in its requests' tokens: a lost connection's late completions are told
    /// apart.
    private var pipeGenerations: [UInt32]

    /// The Postgres connections: `readConnections` for reads, then the write connections.
    private var pipes = UniqueArray<Pipeline>()
    /// The read connection this turn's reads go to, chosen at its first read.
    private var turnReads: Int?
    /// The write connection this turn's writes go to, chosen at its first write.
    private var turnWrites: Int?
    /// Pipelines with messages queued this turn (a bit each).
    private var queued: UInt64 = 0
    /// When to try lost Postgres connections again (monotonic nanoseconds; 0: none lost).
    private var reconnectAt: UInt64 = 0

    init(loop: EventLoop, signer: TokenSigner, db: DbEndpoint, host: String, ports: [UInt16], readConnections: Int,
         writeConnections: Int, started: UInt64, pause: Pause, bufferCount: Int, bufferSize: Int)
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
        self.bufferCount = bufferCount
        self.bufferSize = bufferSize
        pipeGenerations = Array(repeating: 0, count: readConnections + writeConnections)
        freeSlots.reserveCapacity(4096)
        pipelinedSlots.reserveCapacity(256)
        pipes = UniqueArray(capacity: readConnections + writeConnections)
        for _ in 0..<(readConnections + writeConnections) { pipes.append(Pipeline(started: started)) }
    }

    // ---------- the loop ----------

    func run() async {
        // A descriptor table of this thread's own: a million sockets don't share one table's lock.
        Sys.unshareFiles()
        // The ring is made here, on the thread that submits to it.
        do {
            ring = try IORing(entries: 4096, completions: 65536, options: .singleThread)
            bufferGroup = try ring!.registerBuffers(count: UInt32(bufferCount), size: UInt32(bufferSize))
        } catch {
            fatalError("io_uring: \(error)")
        }
        for (i, port) in ports.enumerated() {
            let fd: Int32
            do {
                fd = try Sys.bindListener(host: host, port: port)
            } catch {
                fatalError("failed to bind listener: \(error)")
            }
            listeners.append(fd)
            armAccept(i)
        }
        armWake()
        reconnect()
        while true {
            // One wait per turn, on this actor's own thread: until a completion, a job queued for the actor (its
            // eventfd read), or a second while a lost Postgres connection waits to be retried.
            let jobsQueued = loop.willWait()
            ring!.submit(waitingFor: jobsQueued ? 0 : 1, timeout: reconnectAt != 0 ? .seconds(1) : nil)
            let jobsWaiting = loop.didWait()
            let turnStarted = Sys.monotonicNanos()
            var n = 0
            while let c = ring!.nextCompletion() {
                n += 1
                complete(c)
            }
            // The turn's buffers go back to the kernel here; receives that found none are armed again.
            ring!.finishCompletions()
            if !starved.isEmpty { rearmStarved() }
            endTurn()
            // A fixed cadence: a turn that didn't fill `maxEvents` lasts `pause.nanos` from its wait, the rest slept,
            // so what arrives meanwhile is served in the next turn: its reads share one Sync and its writes one commit.
            // A full turn goes straight on, so the cadence can't cap throughput.
            if n > 0 && n < maxEvents && pause.nanos > 0 {
                let left = pause.nanos - Int(Sys.monotonicNanos() - turnStarted)
                if left > 0 { Sys.sleep(nanos: left) }
            }
            if jobsWaiting { await Task.yield() }
        }
    }

    // ---------- completions ----------

    private func complete(_ c: IORing.Completion) {
        let index = UInt32(truncatingIfNeeded: c.userData)
        let epoch = UInt32(truncatingIfNeeded: c.userData >> 32) & Self.epochMask
        switch Op(rawValue: c.userData >> 56) {
        case .accept:
            accepted(c, listener: Int(index))
        case .receive:
            received(c, slot: index, epoch: epoch)
        case .writable:
            guard live(index, epoch) else { return }
            slots[Int(index)].pollingOut = false
            flushTx(index)
        case .pipeIn:
            if pipeGenerations[Int(index)] & Self.epochMask == epoch { pipeReadable(Int(index), c) }
        case .pipeOut:
            guard pipeGenerations[Int(index)] & Self.epochMask == epoch else { return }
            pipes[Int(index)].waitingToSend = false
            flushPipe(Int(index))
        case .wake:
            armWake()
        case .none:
            // A cancel's own completion (token 0).
            break
        }
    }

    /// Whether `slot` is still the connection a request with `epoch` was made for.
    @inline(__always) private func live(_ slot: UInt32, _ epoch: UInt32) -> Bool {
        let s = Int(slot)
        return s < slots.count && slots[s].fd >= 0 && slots[s].epoch & Self.epochMask == epoch
    }

    private func armAccept(_ l: Int) {
        ring!.prepare(.accept(.descriptor(listeners[l]), multishot: true, nonblocking: true),
                      userData: Self.token(.accept, UInt32(l)))
    }

    private func armWake() {
        ring!.prepare(.read(.descriptor(loop.wakeFD), into: wakeBytes, offset: 0), userData: Self.token(.wake, 0))
    }

    private func armReceive(_ slot: UInt32) {
        let s = Int(slot)
        ring!.prepare(.receiveProvided(.descriptor(slots[s].fd), group: bufferGroup, multishot: true),
                      userData: Self.token(.receive, slot, slots[s].epoch))
    }

    /// A one-shot poll, armed again after each read: arming checks the socket at once, so replies already waiting
    /// complete it (a multishot poll reports new arrivals only, which the reads here, written for level-triggered
    /// epoll, could miss).
    private func armPipe(_ p: Int) {
        ring!.prepare(.poll(.descriptor(pipes[p].conn.fd), events: Sys.EPOLLIN | Sys.EPOLLRDHUP),
                      userData: Self.token(.pipeIn, UInt32(p), pipeGenerations[p]))
    }

    private func rearmStarved() {
        for (slot, epoch) in starved where live(slot, epoch) && !slots[Int(slot)].peerClosed { armReceive(slot) }
        starved.removeAll(keepingCapacity: true)
    }

    /// The turn's reads under one Sync and its writes under another, then every queued message out; lost connections
    /// retried once a second.
    private func endTurn() {
        if let p = turnReads {
            pipes[p].sync()
            queued |= 1 << UInt64(p)
            turnReads = nil
        }
        if let p = turnWrites {
            pipes[p].sync()
            queued |= 1 << UInt64(p)
            turnWrites = nil
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
                pipes[p].conn = conn
                pipes[p].waitingToSend = false
                pipeGenerations[p] &+= 1
                armPipe(p)
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

    /// The write connection this turn's writes go to, chosen the same way: up to `writesPerSync` of them share a Sync,
    /// so they commit together, and each response waits for that commit.
    private func writePipe() -> Int? {
        if let p = turnWrites {
            if pipes[p].unsynced < Self.writesPerSync { return p }
            pipes[p].sync()
            queued |= 1 << UInt64(p)
        }
        turnWrites = leastLoaded(readConnections..<pipes.count)
        return turnWrites
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
            break
        case .pending:
            // The rest goes once the socket takes more: a one-shot poll, which `waitingToSend` keeps to one.
            if !pipes[p].waitingToSend {
                pipes[p].waitingToSend = true
                ring!.prepare(.poll(.descriptor(pipes[p].conn.fd), events: Sys.EPOLLOUT),
                              userData: Self.token(.pipeOut, UInt32(p), pipeGenerations[p]))
            }
        case .failed:
            pipeLost(p)
        }
    }

    /// Pipeline `p`'s socket has something to read (its poll), or failed.
    private func pipeReadable(_ p: Int, _ c: IORing.Completion) {
        guard pipes[p].conn.isOpen else { return }
        if c.result < 0 {
            pipeLost(p)
            return
        }
        let alive = pipes[p].conn.receive()
        while let answer = pipes[p].next() { deliver(answer, from: p) }
        if pipes[p].resent {
            // Writes of a rolled-back group, queued again: they go out at the end of the turn.
            pipes[p].resent = false
            queued |= 1 << UInt64(p)
        }
        if alive { armPipe(p) } else { pipeLost(p) }
        drainPipelined()
    }

    /// A lost connection: everything waiting on it is answered with an error, and it is retried a second later.
    private func pipeLost(_ p: Int) {
        Sys.logLine("postgres connection lost; reconnecting")
        // Its poll would hold the closed socket: cancelled, and anything late from it is told apart by the generation.
        ring!.prepare(.cancel(userData: Self.token(.pipeIn, UInt32(p), pipeGenerations[p])), userData: 0)
        pipeGenerations[p] &+= 1
        pipes[p].conn.close()
        pipes[p].waitingToSend = false
        while let answer = pipes[p].abandonNext() { deliver(answer, from: p) }
        if turnReads == p { turnReads = nil }
        if turnWrites == p { turnWrites = nil }
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

    /// A connection from a listener's multishot accept (which stays armed until it fails).
    private func accepted(_ c: IORing.Completion, listener l: Int) {
        if c.result >= 0 { open(c.result) }
        if !c.hasMore { armAccept(l) }
    }

    /// A slot for an accepted connection, then its multishot receive.
    private func open(_ client: Int32) {
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
        armReceive(slot)
    }

    /// A multishot receive's completion: bytes in a provided buffer (parsed, then the buffer given back), the end of
    /// the stream, or an error. It stays armed while the completion says so.
    private func received(_ c: IORing.Completion, slot: UInt32, epoch: UInt32) {
        if let id = c.bufferID {
            if c.result > 0 && live(slot, epoch) {
                take(ring!.buffer(group: bufferGroup, id, count: Int(c.result)), slot)
            }
            ring!.recycle(group: bufferGroup, buffer: id)
        }
        guard live(slot, epoch) else { return }
        if c.result == 0 {
            peerDone(slot)
        } else if c.result < 0 {
            // Out of buffers ends the receive: armed again at the end of the turn, once the turn's are back.
            if c.result == -ENOBUFS { starved.append((slot, epoch)) } else { dropConn(slot) }
        } else if !c.hasMore && !slots[Int(slot)].peerClosed {
            armReceive(slot)
        }
    }

    /// Bytes a receive brought: parsed in place when the connection has nothing left over (what follows a complete
    /// request is copied out), else after what it has.
    private func take(_ bytes: UnsafeRawBufferPointer, _ slot: UInt32) {
        let s = Int(slot)
        let n = bytes.count
        guard !slots[s].inFlight && slots[s].rx.isEmpty else {
            slots[s].rx.append(bytes)
            drainRxBuf(slot)
            return
        }
        let outcome = HTTP.parseRequest(signer, Span(_unsafeElements: bytes.assumingMemoryBound(to: UInt8.self)))
        switch consume outcome {
        case .complete(let consumed, let closeAfter, let acceptsGzip, let dispatch):
            slots[s].continueSent = false
            slots[s].closeOnDone = closeAfter
            slots[s].acceptsGzip = acceptsGzip
            slots[s].inFlight = true
            if consumed < n { slots[s].rx.append(UnsafeRawBufferPointer(rebasing: bytes[consumed..<n])) }
            if handle(dispatch, slot) && slots[s].fd >= 0 && !slots[s].rx.isEmpty { drainRxBuf(slot) }
        case .incomplete(let sendContinue):
            slots[s].rx.append(bytes)
            if sendContinue && !slots[s].continueSent {
                slots[s].continueSent = true
                writeRaw(slot, "HTTP/1.1 100 Continue\r\n\r\n")
            }
        case .reject(let status):
            rejectConn(slot, status)
        }
    }

    private func dropConn(_ slot: UInt32) {
        let s = Int(slot)
        if slots[s].fd < 0 { return }
        // Shut down first: the connection's armed receive and poll then complete (their tokens are stale by then).
        _ = sys_shutdown(slots[s].fd, 2)
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

    /// The peer is done sending (it may have only half-closed): what it sent is answered, then the connection closes.
    private func peerDone(_ slot: UInt32) {
        let s = Int(slot)
        let pending = slots[s].inFlight || slots[s].txOff < slots[s].tx.count || !slots[s].rx.isEmpty
        guard pending else {
            dropConn(slot)
            return
        }
        // Its multishot receive ended with the stream.
        slots[s].peerClosed = true
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
            guard let p = writePipe() else { return unavailable(query, slot) }
            pipes[p].send(query, slot: slot, epoch: epoch)
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
        // What the socket didn't take goes once it takes more: a one-shot poll, which `pollingOut` keeps to one.
        if hasUnsent && !slots[s].pollingOut {
            slots[s].pollingOut = true
            ring!.prepare(.poll(.descriptor(slots[s].fd), events: Sys.EPOLLOUT), userData: Self.token(.writable, slot, slots[s].epoch))
        }
    }
}

extension Reactor {
    /// The batching window: a turn that didn't take `maxEvents` completions lasts `nanos`, counted from its wait.
    struct Pause: Sendable {
        let nanos: Int
    }

    /// What a request is for: its token's top byte (see `token`).
    enum Op: UInt64 {
        case accept = 1, receive, writable, pipeIn, pipeOut, wake
    }

    /// A request's token: its kind in the top byte; a client's slot (or a pipeline's or a listener's index) in the low
    /// 32 bits, and the slot's epoch (or the pipeline's generation) in the 24 above, so late completions for a closed
    /// connection are told apart.
    @inline(__always) static func token(_ op: Op, _ index: UInt32, _ epoch: UInt32 = 0) -> UInt64 {
        op.rawValue << 56 | UInt64(epoch & epochMask) << 32 | UInt64(index)
    }

    private static let epochMask: UInt32 = 0xFF_FFFF
    private static let epochClosed = UInt32.max
    private static let readsPerSync = 64
    private static let writesPerSync = 64
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
