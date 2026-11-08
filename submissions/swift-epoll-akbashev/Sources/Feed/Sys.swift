// The Linux interface: the constants are the Linux ABI's; the calls Glibc's Swift module leaves out are declared in the
// Syscalls module.
import Glibc
import Syscalls

enum Sys {
    static let CLONE_FILES: Int32 = 0x0000_0400
    static let RLIMIT_NOFILE: Int32 = 7
    static let EFD_NONBLOCK: Int32 = 0o4000
    static let EFD_CLOEXEC: Int32 = 0o2000000
    static let AF_INET: Int32 = 2
    static let AF_INET6: Int32 = 10
    static let SOCK_STREAM: Int32 = 1
    static let SOCK_NONBLOCK: Int32 = 0o4000
    static let SOCK_CLOEXEC: Int32 = 0o2000000
    static let SOL_SOCKET: Int32 = 1
    static let SO_REUSEADDR: Int32 = 2
    static let SO_REUSEPORT: Int32 = 15
    static let SO_RCVTIMEO: Int32 = 20
    static let SO_SNDTIMEO: Int32 = 21
    static let IPPROTO_TCP: Int32 = 6
    static let TCP_NODELAY: Int32 = 1
    static let TCP_THIN_LINEAR_TIMEOUTS: Int32 = 16
    static let IPPROTO_IPV6: Int32 = 41
    static let IPV6_V6ONLY: Int32 = 26
    static let EPOLL_CLOEXEC: Int32 = 0o2000000
    static let EPOLL_CTL_ADD: Int32 = 1
    static let EPOLL_CTL_MOD: Int32 = 3
    static let EPOLLIN: UInt32 = 0x001
    static let EPOLLOUT: UInt32 = 0x004
    static let EPOLLERR: UInt32 = 0x008
    static let EPOLLHUP: UInt32 = 0x010
    static let EPOLLRDHUP: UInt32 = 0x2000
    static let EINTR: Int32 = 4
    static let EAGAIN: Int32 = 11
    static let ECONNABORTED: Int32 = 103

    /// `struct epoll_event` is `{ u32 events; u64 data; }`, packed on x86_64 (12 bytes) and naturally aligned
    /// elsewhere (16 bytes). Swift has no packed structs, so events are read and written at these offsets.
    #if arch(x86_64)
    static let epollEventStride = 12
    static let epollDataOffset = 4
    #else
    static let epollEventStride = 16
    static let epollDataOffset = 8
    #endif
}

extension Sys {
    struct RLimit {
        var cur: UInt64 = 0
        var max: UInt64 = 0
    }

    /// Monotonic time in nanoseconds.
    @inline(__always) static func monotonicNanos() -> UInt64 {
        var ts = timespec()
        clock_gettime(CLOCK_MONOTONIC, &ts)
        return UInt64(ts.tv_sec) &* 1_000_000_000 &+ UInt64(ts.tv_nsec)
    }

    /// Sleeps the calling thread.
    static func sleep(nanos: Int) {
        var request = timespec(tv_sec: nanos / 1_000_000_000, tv_nsec: nanos % 1_000_000_000)
        var remaining = timespec()
        while nanosleep(&request, &remaining) != 0 && errno == EINTR { request = remaining }
    }

    /// Seconds since the Unix epoch, as a Double.
    @inline(__always) static func unixNowF64() -> Double {
        var ts = timespec()
        clock_gettime(CLOCK_REALTIME, &ts)
        return Double(ts.tv_sec) + Double(ts.tv_nsec) / 1e9
    }

    /// The CPUs this process may run on: its affinity mask, which a cpuset narrows.
    static func availableParallelism() -> Int {
        var mask = InlineArray<16, UInt64>(repeating: 0)
        var span = mask.mutableSpan
        let rc = span.withUnsafeMutableBytes { sys_sched_getaffinity(0, $0.count, $0.baseAddress!) }
        guard rc == 0 else { return max(Int(sysconf(Int32(_SC_NPROCESSORS_ONLN))), 1) }
        var cpus = 0
        for i in 0..<16 { cpus += mask[i].nonzeroBitCount }
        return max(cpus, 1)
    }

    /// The calling thread gets a file descriptor table of its own.
    static func unshareFiles() {
        _ = sys_unshare(CLONE_FILES)
    }

    /// Raises the descriptor soft limit to the hard one (the box's unit allows 8,388,608) and returns it.
    static func raiseNofileLimit() -> Int {
        var rl = RLimit()
        guard sys_getrlimit(RLIMIT_NOFILE, &rl) == 0 else { return 0 }
        if rl.cur < rl.max {
            var raised = RLimit(cur: rl.max, max: rl.max)
            if sys_setrlimit(RLIMIT_NOFILE, &raised) == 0 { rl.cur = rl.max }
        }
        return Int(rl.cur)
    }

    @inline(__always) private static func setIntOption(_ fd: Int32, _ level: Int32, _ name: Int32, _ value: Int32) -> Int32 {
        var v = value
        return sys_setsockopt(fd, level, name, &v, 4)
    }

    /// A non-blocking listener on `host:port` with SO_REUSEPORT, so every reactor binds one of its own.
    static func bindListener(host: String, port: UInt16) throws(SystemError) -> Int32 {
        var v4 = in_addr()
        var v6 = in6_addr()
        let isV4 = inet_pton(AF_INET, host, &v4) == 1
        guard isV4 || inet_pton(AF_INET6, host, &v6) == 1 else { throw SystemError("invalid HOST address \(host)", EINVAL) }
        let fd = sys_socket(isV4 ? AF_INET : AF_INET6, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
        guard fd >= 0 else { throw SystemError("socket") }
        for opt in [SO_REUSEADDR, SO_REUSEPORT] {
            guard setIntOption(fd, SOL_SOCKET, opt, 1) == 0 else { throw SystemError("setsockopt") }
        }
        // Set once here: Linux gives every accepted socket the listener's TCP_NODELAY and linear timeouts.
        _ = setIntOption(fd, IPPROTO_TCP, TCP_NODELAY, 1)
        _ = setIntOption(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, 1)
        let bound: Int32
        if isV4 {
            var sa = sockaddr_in()
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_port = port.bigEndian
            sa.sin_addr = v4
            bound = sys_bind(fd, &sa, UInt32(MemoryLayout<sockaddr_in>.size))
        } else {
            guard setIntOption(fd, IPPROTO_IPV6, IPV6_V6ONLY, 0) == 0 else { throw SystemError("setsockopt") }
            var sa = sockaddr_in6()
            sa.sin6_family = sa_family_t(AF_INET6)
            sa.sin6_port = port.bigEndian
            sa.sin6_addr = v6
            bound = sys_bind(fd, &sa, UInt32(MemoryLayout<sockaddr_in6>.size))
        }
        guard bound == 0 else { throw SystemError("bind \(host):\(port)") }
        guard sys_listen(fd, 65535) == 0 else { throw SystemError("listen") }
        return fd
    }

    /// Send and receive timeouts on a blocking socket (0: none).
    static func setTimeouts(_ fd: Int32, seconds: Int) {
        var tv = timeval(tv_sec: seconds, tv_usec: 0)
        _ = sys_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, UInt32(MemoryLayout<timeval>.size))
        _ = sys_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, UInt32(MemoryLayout<timeval>.size))
    }

    /// TCP_NODELAY on a connected socket.
    @inline(__always) static func setNoDelay(_ fd: Int32) {
        _ = setIntOption(fd, IPPROTO_TCP, TCP_NODELAY, 1)
    }

    /// Retransmission timeouts that stay linear (one RTO each) instead of doubling while few packets are in flight,
    /// which is always the case for a request and its response: a response lost twice costs two RTOs, not three.
    @inline(__always) static func setLinearTimeouts(_ fd: Int32) {
        _ = setIntOption(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, 1)
    }

    /// Writes all of `bytes` to a blocking descriptor.
    static func writeAll(_ fd: Int32, _ bytes: UnsafeRawBufferPointer) throws(SystemError) {
        var off = 0
        while off < bytes.count {
            let n = sys_write(fd, bytes.baseAddress! + off, bytes.count - off)
            if n < 0 {
                if errno == EINTR { continue }
                throw SystemError("write")
            }
            off += n
        }
    }

    /// Whether /proc/cpuinfo's `flags` line lists `flag`.
    static func cpuHasFlag(_ flag: String) -> Bool {
        guard let file = fopen("/proc/cpuinfo", "r") else { return false }
        defer { fclose(file) }
        var line: UnsafeMutablePointer<CChar>?
        var size = 0
        defer { free(line) }
        while getline(&line, &size, file) > 0, let line {
            let text = String(cString: line)
            guard text.hasPrefix("flags") else { continue }
            return text.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" }).contains { $0 == flag }
        }
        return false
    }

    /// A line on standard error.
    static func logLine(_ line: String) {
        var text = line + "\n"
        text.withUTF8 { _ = sys_write(2, $0.baseAddress, $0.count) }
    }

    /// A thread's body on its way through pthread_create.
    private final class ThreadStart: Sendable {
        let name: String
        let body: @Sendable () -> Void
        init(name: String, body: @escaping @Sendable () -> Void) {
            self.name = name
            self.body = body
        }
    }

    /// Starts a detached thread named `name` running `body`.
    static func startThread(name: String, _ body: @escaping @Sendable () -> Void) {
        let start = Unmanaged.passRetained(ThreadStart(name: name, body: body)).toOpaque()
        var thread = pthread_t()
        let rc = pthread_create(&thread, nil, { raw in
            let start = Unmanaged<ThreadStart>.fromOpaque(raw!).takeRetainedValue()
            _ = sys_pthread_setname_np(pthread_self(), String(start.name.prefix(15)))
            start.body()
            return nil
        }, start)
        if rc != 0 { fatalError("pthread_create: \(rc)") }
        pthread_detach(thread)
    }
}

struct SystemError: Error, CustomStringConvertible {
    let call: String
    let code: Int32
    init(_ call: String, _ code: Int32 = errno) {
        self.call = call
        self.code = code
    }
    var description: String { "\(call): \(String(cString: strerror(code)))" }
}
