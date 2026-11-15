#if os(Linux)
import CShim
import CUring

func environment(_ name: StaticString) -> UnsafePointer<CChar>? {
    sv_getenv(UnsafeRawPointer(name.utf8Start).assumingMemoryBound(to: CChar.self))
}

func fail(_ message: StaticString) -> Never {
    _ = write(2, message.utf8Start, message.utf8CodeUnitCount)
    _ = write(2, "\n", 1)
    exit(1)
}

/// The Postgres connections in all, across every worker (rule 5 allows 512).
let totalConnections = 64
/// Listening sockets per port, shared out over the workers. Each has its own queue, capped at
/// net.core.somaxconn, so with few cores a burst of new connections waits in several.
let listenersPerPort = 8

final class WorkerConfig: @unchecked Sendable {
    let url: DatabaseURL
    let secret: ByteView
    let host: UnsafePointer<CChar>
    let ports: [Int32]
    let slots: Int
    let listeners: Int
    let readers: Int
    let writers: Int
    let startedAt: Int64

    init(url: DatabaseURL, secret: ByteView, host: UnsafePointer<CChar>, ports: [Int32], slots: Int, listeners: Int,
         readers: Int, writers: Int, startedAt: Int64) {
        self.url = url
        self.secret = secret
        self.host = host
        self.ports = ports
        self.slots = slots
        self.listeners = listeners
        self.readers = readers
        self.writers = writers
        self.startedAt = startedAt
    }
}

/// One worker's whole life. The ring is created here, on the thread that uses it
/// (IORING_SETUP_SINGLE_ISSUER), and listening starts only when its Postgres connections are up.
func runWorker(_ config: WorkerConfig) -> Never {
    var listeners: [Int32] = []
    for port in config.ports {
        for _ in 0..<config.listeners {
            let fd = sv_listen(config.host, port, 65535, 1)
            guard fd >= 0 else { fail("cannot listen") }
            listeners.append(fd)
        }
    }
    guard let server = Server(url: config.url, secret: config.secret, listeners: listeners, slots: config.slots,
                              readers: config.readers, writers: config.writers, startedAt: config.startedAt) else {
        fail("io_uring setup failed")
    }
    server.run()
}

/// Thread entry point. A global function, so it isn't isolated to the main actor.
func workerThread(_ raw: UnsafeMutableRawPointer?) -> UnsafeMutableRawPointer? {
    runWorker(Unmanaged<WorkerConfig>.fromOpaque(raw.unsafelyUnwrapped).takeUnretainedValue())
}

@main
struct Main {
    static func main() {
        guard let databaseURL = environment("DATABASE_URL") else { fail("DATABASE_URL is not set") }
        guard let url = DatabaseURL(databaseURL) else { fail("cannot parse DATABASE_URL") }
        guard let secret = environment("JWT_SECRET") else { fail("JWT_SECRET is not set") }
        let host = environment("HOST") ?? UnsafeRawPointer(("0.0.0.0" as StaticString).utf8Start).assumingMemoryBound(to: CChar.self)
        var ports: [Int32] = [environment("PORT").map { atoi($0) } ?? 80]
        if let extra = environment("EXTRA_PORT") { ports.append(atoi(extra)) }
        guard tu_probe() != 0 else { fail("io_uring is not available") }

        sv_ignore_sigpipe()
        let slots = Int(sv_raise_nofile())
        let workers = Int(sv_cpu_count())
        // One read and one write connection at the least, and about three reads for each write.
        let perWorker = max(2, totalConnections / workers)
        let writers = max(1, perWorker / 4)
        let config = WorkerConfig(
            url: url,
            secret: ByteView(base: UnsafeRawPointer(secret).assumingMemoryBound(to: UInt8.self), count: strlen(secret)),
            host: host, ports: ports, slots: slots, listeners: max(1, listenersPerPort / workers),
            readers: perWorker &- writers, writers: writers, startedAt: sv_monotonic_s())
        for _ in 1..<max(workers, 1) {
            guard sv_spawn(workerThread, Unmanaged.passRetained(config).toOpaque()) == 0 else { fail("cannot start a worker") }
        }
        runWorker(config)
    }
}
#else
@main
struct Main {
    static func main() { fatalError("this server runs on Linux only") }
}
#endif
