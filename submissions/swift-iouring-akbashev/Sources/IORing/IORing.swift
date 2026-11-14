// io_uring without liburing or C: the raw system calls (io_uring_setup, io_uring_enter, io_uring_register) through
// syscall(2), the rings mapped with mmap, entries and completions written and read at the kernel's offsets
// (linux/io_uring.h, the same on x86-64 and arm64), and the heads and tails moved with the acquire and release orderings
// the ring protocol asks for. The shape follows swift-system's IORing: prepare requests, submit, consume completions.
//
//     var ring = try IORing(entries: 256, options: .singleThread)
//     ring.prepare(.read(.descriptor(fd), into: buffer, offset: 0), userData: 1)
//     ring.submit(waitingFor: 1)
//     while let completion = ring.nextCompletion() { ... }
//     ring.finishCompletions()
//
// What goes in a request (descriptors, buffers, socket addresses) must stay put until its completion arrives.
#if os(Linux)
import Glibc
import Synchronization

/// An io_uring instance. Noncopyable: it owns its mappings, and closes the ring when it goes.
public struct IORing: ~Copyable {
    @usableFromInline let fd: Int32
    // Submission queue: the kernel's head, tail and flags, and the entries. `sqeTail` runs ahead of the kernel's tail
    // until `submit` publishes it.
    @usableFromInline let sqHead: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let sqTail: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let sqFlags: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let sqMask: UInt32
    @usableFromInline let sqEntries: UInt32
    @usableFromInline var sqeTail: UInt32
    @usableFromInline let sqes: UnsafeMutableRawPointer
    // Completion queue: `cqHead` is ours, published by `finishCompletions`.
    @usableFromInline let cqHeadShared: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let cqTail: UnsafeMutablePointer<Atomic<UInt32>>
    @usableFromInline let cqMask: UInt32
    @usableFromInline var cqHead: UInt32
    @usableFromInline let cqes: UnsafeRawPointer
    private let maps: Maps
    /// io_uring_enter's wait argument: an io_uring_getevents_arg pointing at the __kernel_timespec after it.
    @usableFromInline let waitArgument: UnsafeMutableRawPointer
    /// The provided buffer groups, by group id.
    @usableFromInline var groups: [BufferGroup] = []

    /// How the ring is set up (IORING_SETUP_*).
    public struct Options: OptionSet, Sendable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }
        /// Keep submitting a batch after a request in it fails.
        public static let submitAll = Options(rawValue: 1 << 7)
        /// Run completions' kernel work when the thread next enters the kernel, not by interrupting it.
        public static let cooperativeTaskRun = Options(rawValue: 1 << 8)
        /// Flag the rings when completions are waiting to be posted (`needsEnter`); needs cooperativeTaskRun or
        /// deferTaskRun.
        public static let taskRunFlag = Options(rawValue: 1 << 9)
        /// Only the creating thread submits.
        public static let singleIssuer = Options(rawValue: 1 << 12)
        /// Post completions only when the thread asks for them (every `submit` does); needs singleIssuer.
        public static let deferTaskRun = Options(rawValue: 1 << 13)

        /// For a ring that one thread owns, as an event loop's does (kernel 6.1 or later): completions come in batches
        /// when the thread asks, without interrupts, and a ring with nothing to do needs no system call.
        public static let singleThread: Options = [.singleIssuer, .deferTaskRun, .submitAll, .taskRunFlag]
    }

    /// A descriptor a request works on.
    public enum File: Sendable {
        case descriptor(Int32)
        /// A slot in the ring's table of registered descriptors (`registerFiles`).
        case fixed(UInt32)
    }

    /// What a request asks of the kernel. Pointers must stay valid until its completion.
    public enum Request {
        case nop
        /// Reads into `buffer` at `offset` (`.max`: the descriptor's current position).
        case read(File, into: UnsafeMutableRawBufferPointer, offset: UInt64)
        /// Writes `bytes` at `offset` (`.max`: the descriptor's current position).
        case write(File, from: UnsafeRawBufferPointer, offset: UInt64)
        /// Sends `bytes` with send(2) `flags` (MSG_WAITALL: all of them, or an error).
        case send(File, UnsafeRawBufferPointer, flags: UInt32 = 0)
        /// Receives into `buffer` with recv(2) `flags`.
        case receive(File, into: UnsafeMutableRawBufferPointer, flags: UInt32 = 0)
        /// Receives into a buffer the kernel takes from provided buffer `group` (`registerBuffers`); with `multishot`
        /// it stays armed, one completion per chunk, until it fails or the buffers run out.
        case receiveProvided(File, group: UInt16, multishot: Bool = false, flags: UInt32 = 0)
        /// Accepts a connection on a listener: a new descriptor, or with `direct` a free slot of the registered table
        /// (the completion's result). With `multishot` it stays armed, one completion per connection; `nonblocking`
        /// makes the new socket non-blocking (SOCK_NONBLOCK).
        case accept(File, multishot: Bool = false, direct: Bool = false, nonblocking: Bool = false)
        /// Connects to the socket address at `address`, `length` bytes.
        case connect(File, address: UnsafeRawPointer, length: UInt32)
        /// shutdown(2): `how` is SHUT_RD (0), SHUT_WR (1) or SHUT_RDWR (2).
        case shutdown(File, how: UInt32 = 2)
        /// Closes a descriptor, or frees a slot of the registered table.
        case close(File)
        /// Cancels the request submitted with `userData`.
        case cancel(userData: UInt64)
        /// Waits until the descriptor reports any of poll(2)'s `events`; the completion's result is the events it
        /// reported. With `multishot` it stays armed, one completion per wake-up, until it fails or is cancelled.
        case poll(File, events: UInt32, multishot: Bool = false)
    }

    /// How a request is chained to the next one prepared: the next starts when this one completes (`.soft`: and only
    /// if it succeeded). A chain must reach the kernel in one submit: `reserve` its length first.
    public enum Link: Sendable {
        case none, soft, hard
    }

    public struct Completion: Sendable {
        public let userData: UInt64
        /// The request's result: a count, a descriptor or slot, or -errno.
        public let result: Int32
        public let flags: UInt32

        @inlinable init(userData: UInt64, result: Int32, flags: UInt32) {
            self.userData = userData
            self.result = result
            self.flags = flags
        }

        /// A multishot request is still armed.
        @inlinable public var hasMore: Bool { flags & ABI.cqeMore != 0 }

        /// The provided buffer a receive filled, if any.
        @inlinable public var bufferID: UInt16? {
            flags & ABI.cqeBuffer != 0 ? UInt16(truncatingIfNeeded: flags >> ABI.cqeBufferShift) : nil
        }
    }

    /// A ring of `entries` submissions and `completions` completions (twice `entries` by default), rounded up to powers
    /// of two by the kernel.
    public init(entries: UInt32, completions: UInt32? = nil, options: Options = []) throws(IORingError) {
        let flags = options.rawValue | (completions != nil ? ABI.setupCQSize : 0)
        // Without the submission index array where the kernel can (6.6 on); with it, each place maps to its entry.
        let rings: Rings
        do {
            rings = try Rings(entries, completions ?? 0, flags | ABI.setupNoSQArray)
        } catch where error.code == EINVAL {
            rings = try Rings(entries, completions ?? 0, flags)
        }
        fd = rings.fd
        sqHead = rings.sqHead
        sqTail = rings.sqTail
        sqFlags = rings.sqFlags
        sqMask = rings.sqMask
        sqEntries = rings.sqEntries
        sqeTail = rings.sqTail.pointee.load(ordering: .relaxed)
        sqes = rings.sqes
        cqHeadShared = rings.cqHead
        cqTail = rings.cqTail
        cqMask = rings.cqMask
        cqHead = rings.cqHead.pointee.load(ordering: .relaxed)
        cqes = UnsafeRawPointer(rings.cqes)
        maps = rings.maps
        waitArgument = UnsafeMutableRawPointer.allocate(byteCount: ABI.getEventsArgSize + 16, alignment: 8)
        waitArgument.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.getEventsArgSize + 16)
        waitArgument.storeBytes(of: UInt64(UInt(bitPattern: waitArgument + ABI.getEventsArgSize)),
                                toByteOffset: ABI.getEventsArgTimespec, as: UInt64.self)
    }

    deinit {
        // The buffer rings' registrations go first: the kernel may still hold them.
        for group in groups.indices {
            let registration = UnsafeMutableRawPointer.allocate(byteCount: ABI.bufferRegSize, alignment: 8)
            registration.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.bufferRegSize)
            registration.storeBytes(of: UInt16(group), toByteOffset: ABI.bufferRegGroup, as: UInt16.self)
            _ = sys_syscall(ABI.sysRegister, Int(fd), ABI.registerUnregisterBufferRing, Int(bitPattern: registration), 1, 0, 0)
            registration.deallocate()
            munmap(groups[group].ring, Int(groups[group].count) * ABI.bufferEntrySize)
            free(groups[group].base)
        }
        waitArgument.deallocate()
        maps.unmap()
        Glibc.close(fd)
    }

    // ---------- registering ----------

    /// A sparse table of `count` registered descriptors (IORING_REGISTER_FILES2), for `.fixed` files and direct
    /// accepts.
    public mutating func registerFiles(count: UInt32) throws(IORingError) {
        let registration = UnsafeMutableRawPointer.allocate(byteCount: ABI.resourceRegSize, alignment: 8)
        defer { registration.deallocate() }
        registration.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.resourceRegSize)
        registration.storeBytes(of: count, toByteOffset: ABI.resourceRegCount, as: UInt32.self)
        registration.storeBytes(of: ABI.resourceRegisterSparse, toByteOffset: ABI.resourceRegFlags, as: UInt32.self)
        guard sys_syscall(ABI.sysRegister, Int(fd), ABI.registerFiles2, Int(bitPattern: registration), ABI.resourceRegSize, 0, 0) == 0 else {
            throw IORingError("io_uring_register files")
        }
    }

    /// A group of `count` provided buffers (a power of two, up to 32768) of `size` bytes each, for `.receiveProvided`
    /// (IORING_REGISTER_PBUF_RING, kernel 5.19 or later). Returns the group's id; all its buffers start out with the
    /// kernel.
    public mutating func registerBuffers(count: UInt32, size: UInt32) throws(IORingError) -> UInt16 {
        precondition(count > 0 && count <= 32768 && count & (count - 1) == 0, "a power of two of buffers, up to 32768")
        precondition(groups.count < Int(UInt16.max))
        let id = UInt16(groups.count)
        let ringSize = Int(count) * ABI.bufferEntrySize
        guard let ring = mmap(nil, ringSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0),
              ring != UnsafeMutableRawPointer(bitPattern: -1) else { throw IORingError("mmap buffer ring") }
        let registration = UnsafeMutableRawPointer.allocate(byteCount: ABI.bufferRegSize, alignment: 8)
        defer { registration.deallocate() }
        registration.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.bufferRegSize)
        registration.storeBytes(of: UInt64(UInt(bitPattern: ring)), toByteOffset: ABI.bufferRegRing, as: UInt64.self)
        registration.storeBytes(of: count, toByteOffset: ABI.bufferRegEntries, as: UInt32.self)
        registration.storeBytes(of: id, toByteOffset: ABI.bufferRegGroup, as: UInt16.self)
        let ringFD = fd
        let register = { sys_syscall(ABI.sysRegister, Int(ringFD), ABI.registerBufferRing, Int(bitPattern: registration), 1, 0, 0) }
        var rc = register()
        if rc < 0 && errno == EINVAL {
            // Ubuntu's 6.8.0-136 and later kernels refuse reserved words that are all zero (Launchpad #2162843, an
            // inverted check), and take a nonzero one: the first of 6.8's three 8-byte reserved words. Kernels without
            // the bug take the zeros above, so they never see this.
            registration.storeBytes(of: UInt64(1), toByteOffset: ABI.bufferRegReserved68, as: UInt64.self)
            rc = register()
        }
        guard rc == 0 else {
            let error = IORingError("io_uring_register buffer ring")
            munmap(ring, ringSize)
            throw error
        }
        guard let base = aligned_alloc(4096, (Int(count) * Int(size) + 4095) & ~4095) else {
            munmap(ring, ringSize)
            throw IORingError("buffers", ENOMEM)
        }
        groups.append(BufferGroup(ring: ring, base: base, count: count, size: size))
        for buffer in 0..<count { recycle(group: id, buffer: UInt16(buffer)) }
        groups[Int(id)].publish()
        return id
    }

    // ---------- submitting ----------

    /// The next free submission entry, zeroed; when the queue is full, what is in it is submitted first.
    @inlinable @inline(__always) mutating func entry() -> UnsafeMutableRawPointer {
        if sqeTail &- sqHead.pointee.load(ordering: .acquiring) >= sqEntries {
            submit()
            guard sqeTail &- sqHead.pointee.load(ordering: .acquiring) < sqEntries else {
                fatalError("io_uring submission queue stuck")
            }
        }
        let sqe = sqes + Int(sqeTail & sqMask) * ABI.sqeSize
        sqeTail &+= 1
        sqe.initializeMemory(as: UInt64.self, repeating: 0, count: ABI.sqeSize / 8)
        return sqe
    }

    /// Free submission entries.
    @inlinable public var space: UInt32 {
        sqEntries &- (sqeTail &- sqHead.pointee.load(ordering: .acquiring))
    }

    /// Makes room for `count` entries, submitting what is prepared if there isn't, so a chain of linked requests goes
    /// to the kernel in one piece.
    @inlinable public mutating func reserve(_ count: UInt32) {
        precondition(count <= sqEntries)
        if space < count { submit() }
    }

    /// Prepares `request`; it goes to the kernel with the next `submit`, and its completion carries `userData`.
    @inlinable @inline(__always)
    public mutating func prepare(_ request: Request, userData: UInt64, link: Link = .none) {
        let sqe = entry()
        sqe.userData = userData
        switch link {
        case .none: break
        case .soft: sqe.sqeFlags = ABI.sqeLink
        case .hard: sqe.sqeFlags = ABI.sqeHardLink
        }
        switch request {
        case .nop:
            sqe.opcode = ABI.opNop
        case .read(let file, let buffer, let offset):
            sqe.opcode = ABI.opRead
            sqe.setFile(file)
            sqe.address = UInt64(UInt(bitPattern: buffer.baseAddress))
            sqe.length = UInt32(buffer.count)
            sqe.offset = offset
        case .write(let file, let bytes, let offset):
            sqe.opcode = ABI.opWrite
            sqe.setFile(file)
            sqe.address = UInt64(UInt(bitPattern: bytes.baseAddress))
            sqe.length = UInt32(bytes.count)
            sqe.offset = offset
        case .send(let file, let bytes, let flags):
            sqe.opcode = ABI.opSend
            sqe.setFile(file)
            sqe.address = UInt64(UInt(bitPattern: bytes.baseAddress))
            sqe.length = UInt32(bytes.count)
            sqe.opFlags = flags | ABI.messageNoSignal
        case .receive(let file, let buffer, let flags):
            sqe.opcode = ABI.opReceive
            sqe.setFile(file)
            sqe.address = UInt64(UInt(bitPattern: buffer.baseAddress))
            sqe.length = UInt32(buffer.count)
            sqe.opFlags = flags
        case .receiveProvided(let file, let group, let multishot, let flags):
            sqe.opcode = ABI.opReceive
            sqe.setFile(file)
            sqe.sqeFlags |= ABI.sqeBufferSelect
            sqe.bufferGroup = group
            if multishot { sqe.ioprio = ABI.receiveMultishot }
            sqe.opFlags = flags
        case .accept(let file, let multishot, let direct, let nonblocking):
            sqe.opcode = ABI.opAccept
            sqe.setFile(file)
            if multishot { sqe.ioprio = ABI.acceptMultishot }
            // A direct descriptor has no close-on-exec flag (the kernel refuses SOCK_CLOEXEC with a slot).
            if direct { sqe.fileIndex = ABI.fileIndexAlloc } else { sqe.opFlags = ABI.socketCloexec }
            if nonblocking { sqe.opFlags |= ABI.socketNonblock }
        case .connect(let file, let address, let length):
            sqe.opcode = ABI.opConnect
            sqe.setFile(file)
            sqe.address = UInt64(UInt(bitPattern: address))
            sqe.offset = UInt64(length)
        case .shutdown(let file, let how):
            sqe.opcode = ABI.opShutdown
            sqe.setFile(file)
            sqe.length = how
        case .close(let file):
            sqe.opcode = ABI.opClose
            switch file {
            case .descriptor(let fd): sqe.fd = fd
            case .fixed(let slot): sqe.fileIndex = slot &+ 1
            }
        case .cancel(let target):
            sqe.opcode = ABI.opAsyncCancel
            sqe.fd = -1
            sqe.address = target
        case .poll(let file, let events, let multishot):
            sqe.opcode = ABI.opPollAdd
            sqe.setFile(file)
            sqe.opFlags = events
            if multishot { sqe.length = ABI.pollAddMultishot }
        }
    }

    /// Submits what was prepared, then waits for `count` completions, or until `timeout`. Always asks the kernel to post
    /// completions (IORING_ENTER_GETEVENTS), which a deferTaskRun ring does only then. Returns the number submitted,
    /// or -errno (-ETIME: the wait timed out).
    @inlinable @discardableResult
    public mutating func submit(waitingFor count: UInt32 = 0, timeout: Duration? = nil) -> Int32 {
        let submitted = sqeTail &- sqTail.pointee.load(ordering: .relaxed)
        if submitted != 0 { sqTail.pointee.store(sqeTail, ordering: .releasing) }
        var flags = ABI.enterGetEvents
        var argument = 0
        var argumentSize = 0
        if count > 0, let timeout {
            let (seconds, attoseconds) = timeout.components
            waitArgument.storeBytes(of: max(seconds, 0), toByteOffset: ABI.getEventsArgSize, as: Int64.self)
            waitArgument.storeBytes(of: seconds < 0 ? 0 : attoseconds / 1_000_000_000, toByteOffset: ABI.getEventsArgSize + 8, as: Int64.self)
            flags |= ABI.enterExtArg
            argument = Int(bitPattern: waitArgument)
            argumentSize = ABI.getEventsArgSize
        }
        let rc = sys_syscall(ABI.sysEnter, Int(fd), Int(submitted), Int(count), Int(flags), argument, argumentSize)
        return rc < 0 ? -errno : Int32(truncatingIfNeeded: rc)
    }

    /// Whether a `submit` would do anything: entries to submit, completions waiting for this thread to post them (with
    /// taskRunFlag), or completions that overflowed the queue.
    @inlinable public var needsEnter: Bool {
        let flags = sqFlags.pointee.load(ordering: .acquiring)
        return sqeTail != sqTail.pointee.load(ordering: .relaxed) || flags & (ABI.sqTaskrun | ABI.sqCQOverflow) != 0
    }

    // ---------- completions ----------

    /// The next completion, consumed; the kernel sees the space at `finishCompletions()`.
    @inlinable @inline(__always) public mutating func nextCompletion() -> Completion? {
        guard cqHead != cqTail.pointee.load(ordering: .acquiring) else { return nil }
        let cqe = cqes + Int(cqHead & cqMask) * ABI.cqeSize
        let completion = Completion(userData: cqe.load(fromByteOffset: ABI.cqeUserData, as: UInt64.self),
                                    result: cqe.load(fromByteOffset: ABI.cqeResult, as: Int32.self),
                                    flags: cqe.load(fromByteOffset: ABI.cqeFlags, as: UInt32.self))
        cqHead &+= 1
        return completion
    }

    /// Hands the consumed completions' space and the recycled buffers back to the kernel.
    @inlinable public mutating func finishCompletions() {
        cqHeadShared.pointee.store(cqHead, ordering: .releasing)
        for group in groups.indices { groups[group].publish() }
    }

    // ---------- provided buffers ----------

    /// The first `count` bytes of buffer `buffer` of `group`, as a completion reported them: valid until the buffer is
    /// recycled.
    @inlinable public func buffer(group: UInt16, _ buffer: UInt16, count: Int) -> UnsafeRawBufferPointer {
        let g = groups[Int(group)]
        precondition(count <= Int(g.size))
        return UnsafeRawBufferPointer(start: g.base + Int(buffer) * Int(g.size), count: count)
    }

    /// Gives buffer `buffer` back to `group` (the kernel sees it at the next `finishCompletions()`).
    @inlinable public mutating func recycle(group: UInt16, buffer: UInt16) {
        groups[Int(group)].recycle(buffer)
    }
}

/// A registered ring of provided buffers and the buffers themselves, `count` of `size` bytes.
@usableFromInline struct BufferGroup {
    @usableFromInline let ring: UnsafeMutableRawPointer
    @usableFromInline let base: UnsafeMutableRawPointer
    @usableFromInline let count: UInt32
    @usableFromInline let size: UInt32
    /// Ours, published to the ring's shared tail (the first entry's last two bytes) by `publish`.
    @usableFromInline var tail: UInt16 = 0

    init(ring: UnsafeMutableRawPointer, base: UnsafeMutableRawPointer, count: UInt32, size: UInt32) {
        self.ring = ring
        self.base = base
        self.count = count
        self.size = size
    }

    /// An entry's fields are written one by one: the first entry's last two bytes are the ring's tail.
    @inlinable mutating func recycle(_ id: UInt16) {
        let entry = ring + Int(tail & UInt16(truncatingIfNeeded: count &- 1)) * ABI.bufferEntrySize
        entry.storeBytes(of: UInt64(UInt(bitPattern: base + Int(id) * Int(size))), toByteOffset: ABI.bufferAddress, as: UInt64.self)
        entry.storeBytes(of: size, toByteOffset: ABI.bufferLength, as: UInt32.self)
        entry.storeBytes(of: id, toByteOffset: ABI.bufferID, as: UInt16.self)
        tail &+= 1
    }

    @inlinable func publish() {
        (ring + ABI.bufferRingTail).assumingMemoryBound(to: Atomic<UInt16>.self).pointee.store(tail, ordering: .releasing)
    }
}

public struct IORingError: Error, CustomStringConvertible {
    public let call: String
    public let code: Int32
    init(_ call: String, _ code: Int32 = errno) {
        self.call = call
        self.code = code
    }
    public var description: String { "\(call): \(String(cString: strerror(code)))" }
}

extension IORing {
    /// A ring's mappings, unmapped when it goes.
    private struct Maps {
        let sq: UnsafeMutableRawPointer
        let sqSize: Int
        /// The same as `sq` with IORING_FEAT_SINGLE_MMAP.
        let cq: UnsafeMutableRawPointer
        let cqSize: Int
        let sqes: UnsafeMutableRawPointer
        let sqesSize: Int

        func unmap() {
            munmap(sqes, sqesSize)
            if cq != sq { munmap(cq, cqSize) }
            munmap(sq, sqSize)
        }
    }

    /// io_uring_setup and the rings' mappings, laid out as the kernel's offsets say.
    private struct Rings {
        let fd: Int32
        let maps: Maps
        let sqHead, sqTail, sqFlags, cqHead, cqTail: UnsafeMutablePointer<Atomic<UInt32>>
        let sqMask, sqEntries, cqMask: UInt32
        let sqes: UnsafeMutableRawPointer
        let cqes: UnsafeMutableRawPointer

        init(_ entries: UInt32, _ completionEntries: UInt32, _ flags: UInt32) throws(IORingError) {
            let params = UnsafeMutableRawPointer.allocate(byteCount: ABI.paramsSize, alignment: 8)
            defer { params.deallocate() }
            params.initializeMemory(as: UInt8.self, repeating: 0, count: ABI.paramsSize)
            params.storeBytes(of: completionEntries, toByteOffset: ABI.paramsCQEntries, as: UInt32.self)
            params.storeBytes(of: flags, toByteOffset: ABI.paramsFlags, as: UInt32.self)
            let fd = Int32(truncatingIfNeeded: sys_syscall(ABI.sysSetup, Int(entries), Int(bitPattern: params), 0, 0, 0, 0))
            guard fd >= 0 else { throw IORingError("io_uring_setup") }

            let sqEntries = params.load(fromByteOffset: ABI.paramsSQEntries, as: UInt32.self)
            let cqEntries = params.load(fromByteOffset: ABI.paramsCQEntries, as: UInt32.self)
            let features = params.load(fromByteOffset: ABI.paramsFeatures, as: UInt32.self)
            let sq = { (field: Int) in Int(params.load(fromByteOffset: ABI.paramsSQOffsets + field, as: UInt32.self)) }
            let cq = { (field: Int) in Int(params.load(fromByteOffset: ABI.paramsCQOffsets + field, as: UInt32.self)) }
            var sqSize = sq(ABI.ringArray) + Int(sqEntries) * 4
            var cqSize = cq(ABI.ringCQEs) + Int(cqEntries) * ABI.cqeSize
            let single = features & ABI.featSingleMmap != 0
            if single {
                sqSize = max(sqSize, cqSize)
                cqSize = sqSize
            }
            func map(_ size: Int, _ offset: Int) -> UnsafeMutableRawPointer? {
                let p = mmap(nil, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, off_t(offset))
                return p == UnsafeMutableRawPointer(bitPattern: -1) ? nil : p
            }
            guard let sqMap = map(sqSize, ABI.offSQRing) else {
                let error = IORingError("mmap io_uring")
                Glibc.close(fd)
                throw error
            }
            guard let cqMap = single ? sqMap : map(cqSize, ABI.offCQRing) else {
                let error = IORingError("mmap io_uring")
                munmap(sqMap, sqSize)
                Glibc.close(fd)
                throw error
            }
            let sqesSize = Int(sqEntries) * ABI.sqeSize
            guard let sqes = map(sqesSize, ABI.offSQEs) else {
                let error = IORingError("mmap io_uring")
                if !single { munmap(cqMap, cqSize) }
                munmap(sqMap, sqSize)
                Glibc.close(fd)
                throw error
            }
            func field(_ base: UnsafeMutableRawPointer, _ offset: Int) -> UnsafeMutablePointer<Atomic<UInt32>> {
                (base + offset).assumingMemoryBound(to: Atomic<UInt32>.self)
            }
            self.fd = fd
            maps = Maps(sq: sqMap, sqSize: sqSize, cq: cqMap, cqSize: cqSize, sqes: sqes, sqesSize: sqesSize)
            sqHead = field(sqMap, sq(ABI.ringHead))
            sqTail = field(sqMap, sq(ABI.ringTail))
            sqFlags = field(sqMap, sq(ABI.ringFlags))
            sqMask = sqMap.load(fromByteOffset: sq(ABI.ringMask), as: UInt32.self)
            self.sqEntries = sqMap.load(fromByteOffset: sq(ABI.ringEntries), as: UInt32.self)
            self.sqes = sqes
            if flags & ABI.setupNoSQArray == 0 {
                // The index array maps each place in the queue to the entry at the same place.
                let array = (sqMap + sq(ABI.ringArray)).assumingMemoryBound(to: UInt32.self)
                for i in 0..<Int(self.sqEntries) { array[i] = UInt32(i) }
            }
            cqHead = field(cqMap, cq(ABI.ringHead))
            cqTail = field(cqMap, cq(ABI.ringTail))
            cqMask = cqMap.load(fromByteOffset: cq(ABI.ringMask), as: UInt32.self)
            cqes = cqMap + cq(ABI.ringCQEs)
        }
    }
}

/// The kernel's io_uring interface (linux/io_uring.h): system call numbers, flags, opcodes, and the byte offsets of the
/// fields used here. The same on x86-64 and arm64.
@usableFromInline enum ABI {
    @usableFromInline static let sysSetup = 425, sysEnter = 426, sysRegister = 427

    @usableFromInline static let setupCQSize: UInt32 = 1 << 3, setupNoSQArray: UInt32 = 1 << 16
    @usableFromInline static let featSingleMmap: UInt32 = 1 << 0
    @usableFromInline static let offSQRing = 0, offCQRing = 0x800_0000, offSQEs = 0x1000_0000
    @usableFromInline static let enterGetEvents: UInt32 = 1 << 0, enterExtArg: UInt32 = 1 << 3
    @usableFromInline static let sqCQOverflow: UInt32 = 1 << 1, sqTaskrun: UInt32 = 1 << 2
    @usableFromInline static let cqeBuffer: UInt32 = 1 << 0, cqeMore: UInt32 = 1 << 1, cqeBufferShift: UInt32 = 16

    @usableFromInline static let opNop: UInt8 = 0, opPollAdd: UInt8 = 6, opAccept: UInt8 = 13, opAsyncCancel: UInt8 = 14
    @usableFromInline static let opConnect: UInt8 = 16, pollAddMultishot: UInt32 = 1 << 0
    @usableFromInline static let opClose: UInt8 = 19, opRead: UInt8 = 22, opWrite: UInt8 = 23
    @usableFromInline static let opSend: UInt8 = 26, opReceive: UInt8 = 27, opShutdown: UInt8 = 34
    @usableFromInline static let sqeFixedFile: UInt8 = 1 << 0, sqeLink: UInt8 = 1 << 2, sqeHardLink: UInt8 = 1 << 3
    @usableFromInline static let sqeBufferSelect: UInt8 = 1 << 5
    @usableFromInline static let acceptMultishot: UInt16 = 1 << 0, receiveMultishot: UInt16 = 1 << 1
    @usableFromInline static let fileIndexAlloc = UInt32.max
    // sys/socket.h: MSG_NOSIGNAL (a send to a closed peer fails with EPIPE, no SIGPIPE), and SOCK_CLOEXEC for accepts.
    @usableFromInline static let messageNoSignal: UInt32 = 0x4000, socketCloexec: UInt32 = 0o2000000
    @usableFromInline static let socketNonblock: UInt32 = 0o4000

    @usableFromInline static let registerFiles2 = 13, registerBufferRing = 22, registerUnregisterBufferRing = 23
    @usableFromInline static let resourceRegisterSparse: UInt32 = 1 << 0

    // io_uring_params, and the io_sqring_offsets and io_cqring_offsets in it.
    @usableFromInline static let paramsSize = 120, paramsSQEntries = 0, paramsCQEntries = 4, paramsFlags = 8, paramsFeatures = 20
    @usableFromInline static let paramsSQOffsets = 40, paramsCQOffsets = 80
    @usableFromInline static let ringHead = 0, ringTail = 4, ringMask = 8, ringEntries = 12, ringFlags = 16, ringArray = 24, ringCQEs = 20
    // io_uring_sqe (64 bytes) and io_uring_cqe (16).
    @usableFromInline static let sqeSize = 64, cqeSize = 16
    @usableFromInline static let cqeUserData = 0, cqeResult = 8, cqeFlags = 12
    // io_uring_buf (16 bytes; the ring's tail is the first one's last two), io_uring_buf_reg (40),
    // io_uring_rsrc_register (32), io_uring_getevents_arg (24, its timespec pointer at 16).
    @usableFromInline static let bufferEntrySize = 16, bufferAddress = 0, bufferLength = 8, bufferID = 12, bufferRingTail = 14
    @usableFromInline static let bufferRegSize = 40, bufferRegRing = 0, bufferRegEntries = 8, bufferRegGroup = 12
    @usableFromInline static let bufferRegReserved68 = 16
    @usableFromInline static let resourceRegSize = 32, resourceRegCount = 0, resourceRegFlags = 4
    @usableFromInline static let getEventsArgSize = 24, getEventsArgTimespec = 16
}

/// A submission entry's fields, at io_uring_sqe's offsets.
extension UnsafeMutableRawPointer {
    @inlinable func setFile(_ file: IORing.File) {
        switch file {
        case .descriptor(let fd):
            self.fd = fd
        case .fixed(let slot):
            fd = Int32(bitPattern: slot)
            sqeFlags |= ABI.sqeFixedFile
        }
    }

    @usableFromInline var opcode: UInt8 {
        get { load(fromByteOffset: 0, as: UInt8.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 0, as: UInt8.self) }
    }
    @usableFromInline var sqeFlags: UInt8 {
        get { load(fromByteOffset: 1, as: UInt8.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 1, as: UInt8.self) }
    }
    @usableFromInline var ioprio: UInt16 {
        get { load(fromByteOffset: 2, as: UInt16.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 2, as: UInt16.self) }
    }
    @usableFromInline var fd: Int32 {
        get { load(fromByteOffset: 4, as: Int32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 4, as: Int32.self) }
    }
    @usableFromInline var offset: UInt64 {
        get { load(fromByteOffset: 8, as: UInt64.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 8, as: UInt64.self) }
    }
    @usableFromInline var address: UInt64 {
        get { load(fromByteOffset: 16, as: UInt64.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 16, as: UInt64.self) }
    }
    @usableFromInline var length: UInt32 {
        get { load(fromByteOffset: 24, as: UInt32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 24, as: UInt32.self) }
    }
    /// msg_flags, accept_flags (and the other per-opcode flags in the same union).
    @usableFromInline var opFlags: UInt32 {
        get { load(fromByteOffset: 28, as: UInt32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 28, as: UInt32.self) }
    }
    @usableFromInline var userData: UInt64 {
        get { load(fromByteOffset: 32, as: UInt64.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 32, as: UInt64.self) }
    }
    @usableFromInline var bufferGroup: UInt16 {
        get { load(fromByteOffset: 40, as: UInt16.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 40, as: UInt16.self) }
    }
    @usableFromInline var fileIndex: UInt32 {
        get { load(fromByteOffset: 44, as: UInt32.self) }
        nonmutating set { storeBytes(of: newValue, toByteOffset: 44, as: UInt32.self) }
    }
}

// syscall(2), declared here so this file is all the module needs (`@_extern(c)`, the Extern experimental feature).
// It is variadic in C; glibc's is assembly that takes the number and up to six arguments from where a call with that
// many fixed integer arguments puts them (registers, then the stack), on x86-64 and arm64 alike.
@_extern(c, "syscall") @usableFromInline func sys_syscall(_ number: Int, _ a1: Int, _ a2: Int, _ a3: Int, _ a4: Int, _ a5: Int, _ a6: Int) -> Int
#endif
