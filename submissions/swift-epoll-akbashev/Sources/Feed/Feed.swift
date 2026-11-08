// The server: configuration, one check that Postgres takes every statement, and one reactor per core, each an actor
// on its own event loop, run as one task group. The reactors share nothing but immutable configuration.
import Glibc

@main
struct Feed {
    static func main() async {
        guard let databaseURL = env("DATABASE_URL") else { fatalError("DATABASE_URL must be set") }
        guard let jwtSecret = env("JWT_SECRET") else { fatalError("JWT_SECRET must be set") }
        let host = env("HOST") ?? "0.0.0.0"
        var ports = [UInt16(envInt("PORT", 80))]
        if let extra = env("EXTRA_PORT").flatMap({ UInt16($0) }) { ports.append(extra) }

        // One reactor per CPU this process may run on (its affinity mask), and 64 Postgres connections in all (rule 5
        // allows 512): three quarters for reads, a quarter for writes, at least one of each per reactor.
        let reactorCount = max(envInt("IO_THREADS", Sys.availableParallelism()), 1)
        let connections = envInt("DB_CONNECTIONS", 64)
        let reads = min(max(connections * 3 / 4 / reactorCount, 1), 48)
        let writes = min(max(connections / 4 / reactorCount, 1), 16)
        guard reactorCount * (reads + writes) <= 512 else { fatalError("more than 512 Postgres connections") }

        let signer = TokenSigner(secret: Array(jwtSecret.utf8))
        guard let db = DbEndpoint(url: databaseURL) else { fatalError("invalid DATABASE_URL") }
        do {
            var probe = try PgConn.connect(db)
            try probe.prepare(Statement.allCases)
        } catch {
            fatalError("Postgres: \(error)")
        }
        let nofile = Sys.raiseNofileLimit()
        let started = Sys.monotonicNanos()
        let pause = Reactor.Pause(nanos: envInt("PAUSE_US", 2000) * 1000)

        let reactors = (0..<reactorCount).map { _ in
            Reactor(loop: EventLoop(name: "reactor"), signer: signer, db: db, host: host, ports: ports,
                    readConnections: reads, writeConnections: writes, started: started, pause: pause)
        }
        Sys.logLine("listening on \(host):\(ports.map(String.init).joined(separator: ",")) with \(reactorCount) reactors, "
            + "\(reads) read + \(writes) write Postgres connections each, nofile=\(nofile), "
            + "SHA-256 \(SHA256.usesExtensions ? "on the CPU's SHA extensions" : "in Swift")")

        await withDiscardingTaskGroup { group in
            for reactor in reactors { group.addTask(executorPreference: reactor.loop) { await reactor.run() } }
        }
    }
}

extension Feed {
    private static func env(_ name: String) -> String? {
        getenv(name).map { String(cString: $0) }
    }

    private static func envInt(_ name: String, _ fallback: Int) -> Int {
        env(name).flatMap { Int($0) } ?? fallback
    }
}
