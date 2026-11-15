// Noncopyable owners of memory. A buffer has exactly one owner and moves instead of being copied, so it owns its bytes
// outright: an append checks capacity and copies, with no copy-on-write check and no reference count, and the memory
// goes when the owner does. What reads a buffer borrows a `Span` of it, whose lifetime the compiler ties to the borrow.
// Neither is Sendable (`~Sendable`): each stays with the reactor that owns it, and the compiler flags any place one
// would cross.

/// A growable byte buffer. An empty one holds no memory, so an idle connection costs nothing.
struct ByteBuffer: ~Copyable, ~Sendable {
    private var base: UnsafeMutablePointer<UInt8>?
    private(set) var count = 0
    private(set) var capacity = 0

    init() {}

    init(capacity: Int) {
        if capacity > 0 {
            base = .allocate(capacity: capacity)
            self.capacity = capacity
        }
    }

    deinit {
        base?.deallocate()
    }

    @inline(__always) var isEmpty: Bool { count == 0 }

    /// Room for `n` more bytes; the end of the bytes, where they go.
    @inline(__always) private mutating func reserve(_ n: Int) -> UnsafeMutablePointer<UInt8> {
        if count + n > capacity { grow(to: count + n) }
        return base.unsafelyUnwrapped + count
    }

    @inline(never) private mutating func grow(to needed: Int) {
        let next = Swift.max(needed, capacity * 2, 64)
        let fresh = UnsafeMutablePointer<UInt8>.allocate(capacity: next)
        if let base {
            fresh.update(from: base, count: count)
            base.deallocate()
        }
        base = fresh
        capacity = next
    }

    @inline(__always) mutating func append(_ byte: UInt8) {
        reserve(1).pointee = byte
        count += 1
    }

    @inline(__always) mutating func append(_ raw: UnsafeRawBufferPointer) {
        guard let source = raw.baseAddress, raw.count > 0 else { return }
        UnsafeMutableRawPointer(reserve(raw.count)).copyMemory(from: source, byteCount: raw.count)
        count += raw.count
    }

    @inline(__always) mutating func append(_ literal: StaticString) {
        append(UnsafeRawBufferPointer(start: literal.utf8Start, count: literal.utf8CodeUnitCount))
    }

    @inline(__always) mutating func append(_ span: Span<UInt8>) {
        span.withUnsafeBytes { append($0) }
    }

    @inline(__always) mutating func append(_ other: borrowing [UInt8]) {
        other.withUnsafeBytes { append($0) }
    }

    /// Appends bytes `range` of `other`.
    @inline(__always) mutating func append(_ other: borrowing ByteBuffer, _ range: Range<Int>) {
        precondition(range.lowerBound >= 0 && range.upperBound <= other.count)
        other.withBytes { append(UnsafeRawBufferPointer(rebasing: $0[range])) }
    }

    @inline(__always) mutating func append(repeating byte: UInt8, count n: Int) {
        guard n > 0 else { return }
        reserve(n).update(repeating: byte, count: n)
        count += n
    }

    /// Lets `body` fill up to `maximum` bytes past the end (a socket read straight into the buffer); returns what
    /// `body` returned, and keeps that many bytes if it is positive.
    @inline(__always) mutating func appendSpare(maximum: Int, _ body: (UnsafeMutableRawBufferPointer) -> Int) -> Int {
        let spare = UnsafeMutableRawBufferPointer(start: reserve(maximum), count: maximum)
        let n = body(spare)
        if n > 0 { count += n }
        return n
    }

    /// Room for at least `minimum` bytes past the end, for a read that lands there later (`commit` keeps what it wrote):
    /// nothing may change the buffer in between.
    @inline(__always) mutating func spare(minimum: Int) -> UnsafeMutableRawBufferPointer {
        let start = reserve(minimum)
        return UnsafeMutableRawBufferPointer(start: start, count: capacity - count)
    }

    /// Keeps `n` bytes a read wrote past the end (see `spare`).
    @inline(__always) mutating func commit(_ n: Int) {
        precondition(n >= 0 && count + n <= capacity)
        count += n
    }

    @inline(__always) subscript(i: Int) -> UInt8 {
        get {
            precondition(i >= 0 && i < count)
            return base.unsafelyUnwrapped[i]
        }
        set {
            precondition(i >= 0 && i < count)
            base.unsafelyUnwrapped[i] = newValue
        }
    }

    /// Writes `value` big-endian at `offset` (a length patched in after the fact).
    @inline(__always) mutating func storeBigEndian(_ value: UInt32, at offset: Int) {
        precondition(offset >= 0 && offset + 4 <= count)
        UnsafeMutableRawPointer(base.unsafelyUnwrapped + offset).storeBytes(of: value.bigEndian, as: UInt32.self)
    }

    /// Keeps the memory.
    @inline(__always) mutating func removeAll() {
        count = 0
    }

    /// Clears, and gives memory back past `threshold` bytes, keeping `limit`.
    mutating func removeAll(shrinkingAbove threshold: Int, to limit: Int) {
        if capacity > threshold {
            release()
            self = ByteBuffer(capacity: limit)
        } else {
            count = 0
        }
    }

    /// Frees the memory.
    @inline(__always) mutating func release() {
        base?.deallocate()
        base = nil
        count = 0
        capacity = 0
    }

    @inline(__always) mutating func truncate(_ n: Int) {
        if n < count { count = Swift.max(n, 0) }
    }

    /// Drops the first `n` bytes.
    @inline(__always) mutating func removeFirst(_ n: Int) {
        precondition(n >= 0 && n <= count)
        if n < count { base.unsafelyUnwrapped.update(from: base.unsafelyUnwrapped + n, count: count - n) }
        count -= n
    }

    var span: Span<UInt8> {
        @_lifetime(borrow self) borrowing get {
            let span = Span(_unsafeElements: UnsafeBufferPointer(start: base, count: count))
            return _overrideLifetime(span, borrowing: self)
        }
    }

    @inline(__always) func withBytes<R>(_ body: (UnsafeRawBufferPointer) throws -> R) rethrows -> R {
        try body(UnsafeRawBufferPointer(start: base, count: count))
    }

    @inline(__always) mutating func withMutableBytes<R>(_ body: (UnsafeMutableRawBufferPointer) throws -> R) rethrows -> R {
        try body(UnsafeMutableRawBufferPointer(start: base, count: count))
    }
}

/// A growable array of noncopyable elements: a reactor's connection slots, each holding its own buffers, and its
/// Postgres pipelines. Elements are reached in place through `unsafeAddress`, never copied.
struct UniqueArray<Element: ~Copyable>: ~Copyable, ~Sendable {
    private var base: UnsafeMutablePointer<Element>
    private(set) var count: Int = 0
    private var capacity: Int

    init(capacity: Int = 0) {
        self.capacity = Swift.max(capacity, 1)
        base = .allocate(capacity: self.capacity)
    }

    deinit {
        base.deinitialize(count: count)
        base.deallocate()
    }

    @inline(__always) subscript(i: Int) -> Element {
        unsafeAddress { UnsafePointer(base + i) }
        unsafeMutableAddress { base + i }
    }

    @inline(__always) mutating func append(_ element: consuming Element) {
        if count == capacity { grow(to: count + 1) }
        (base + count).initialize(to: element)
        count += 1
    }

    @inline(never) private mutating func grow(to needed: Int) {
        let next = Swift.max(needed, capacity * 2)
        let fresh = UnsafeMutablePointer<Element>.allocate(capacity: next)
        fresh.moveInitialize(from: base, count: count)
        base.deallocate()
        base = fresh
        capacity = next
    }
}

/// A first-in, first-out queue: appends at the back, removes from the front without moving the rest.
struct Fifo<Element> {
    private var items: [Element] = []
    private var head = 0

    @inline(__always) var count: Int { items.count - head }
    @inline(__always) var isEmpty: Bool { head == items.count }
    @inline(__always) var first: Element? { head < items.count ? items[head] : nil }

    @inline(__always) mutating func append(_ item: Element) { items.append(item) }

    mutating func removeFirst() -> Element {
        let item = items[head]
        head += 1
        if head == items.count {
            items.removeAll(keepingCapacity: true)
            head = 0
        } else if head >= 1024 && head * 2 >= items.count {
            items.removeFirst(head)
            head = 0
        }
        return item
    }
}
