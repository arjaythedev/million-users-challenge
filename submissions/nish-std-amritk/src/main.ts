// The Million Users Challenge feed API (https://github.com/arjaythedev/million-users-challenge) in Nish, on its
// standard library only: nish/net/http1-server, nish/net/http-compress, nish/net/postgres, nish/json, nish/crypto's HMAC, nish/net/prefork.
// README.md has the design and the measurements.
//
// One supervisor and HTTP_WORKERS worker processes (nish/net/prefork, restarts off: rule 10 does not restart a
// crashed server, so neither does this). Each worker binds HOST:PORT and HOST:EXTRA_PORT with SO_REUSEPORT and serves
// both on its own loop and its own Postgres connections.
//
// Configured by the environment: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT (SPEC.md), and optionally
// HTTP_WORKERS (the CPUs), DB_READ_CONNS (48) and DB_WRITE_CONNS (64) in total over the workers,
// DB_MAX_IN_FLIGHT (16) groups pipelined per connection, HTTP_SLOTS (1048576) connections per port per worker and
// IDLE_TIMEOUT_MS (120000), GZIP_LEVEL (1) and GZIP_MIN (1024).
import { availableParallelism, getenv, monotonicNanos } from "nish:process"
import { tcpListenFlags } from "nish:net"
import { PREFORK_STOPPED, PreforkConfig, preforkWith } from "nish/net/prefork"
import { FeedWorker, WorkerConfig } from "./worker"

/** `tcpListenFlags`: SO_REUSEPORT, and TCP_NODELAY on every accepted socket. */
const LISTEN_FLAGS: i32 = 3

/** The environment variable `name` as a positive number, or `fallback`. */
const envInt = (name: string, fallback: i32): i32 => {
  const v: string | null = getenv(name)
  if (v === null || v.length === 0 || v.length > 9) {
    return fallback
  }
  for (let k: i32 = 0; k < toI32(v.length); k++) {
    const c: i32 = toI32(v.charCodeAt(k))
    if (c < 48 || c > 57) {
      return fallback
    }
  }
  const n: i32 = parseInt(v)
  return n > 0 ? n : fallback
}

const lastIndexOfByte = (s: string, c: i32): i32 => {
  for (let k: i32 = toI32(s.length) - 1; k >= 0; k--) {
    if (toI32(s.charCodeAt(k)) === c) {
      return k
    }
  }
  return -1
}

/** Reads `postgres://user:password@host:port/database` into `cfg`; false when it is not that shape. */
const readDatabaseUrl = (url: string, cfg: WorkerConfig): boolean => {
  const scheme: i32 = url.indexOf("://")
  if (scheme < 0) {
    return false
  }
  const rest: string = url.substring(scheme + 3)
  const at: i32 = lastIndexOfByte(rest, 64)
  const slash: i32 = rest.indexOf("/", at < 0 ? 0 : at)
  if (at < 0 || slash < 0) {
    return false
  }
  const userInfo: string = rest.substring(0, at)
  const colon: i32 = userInfo.indexOf(":")
  cfg.dbUser = colon < 0 ? userInfo : userInfo.substring(0, colon)
  cfg.dbPassword = colon < 0 ? "" : userInfo.substring(colon + 1)
  const hostPort: string = rest.substring(at + 1, slash)
  const portAt: i32 = lastIndexOfByte(hostPort, 58)
  cfg.dbHost = portAt < 0 ? hostPort : hostPort.substring(0, portAt)
  cfg.dbPort = portAt < 0 ? 5432 : parseInt(hostPort.substring(portAt + 1))
  const query: i32 = rest.indexOf("?", slash)
  cfg.dbName = query < 0 ? rest.substring(slash + 1) : rest.substring(slash + 1, query)
  if (cfg.dbHost === "localhost") {
    cfg.dbHost = "127.0.0.1"
  }
  return cfg.dbUser.length > 0 && cfg.dbName.length > 0 && cfg.dbPort > 0
}

export const main = (): i32 => {
  const cfg = new WorkerConfig()
  cfg.started = monotonicNanos()
  if (!readDatabaseUrl(getenv("DATABASE_URL") ?? "", cfg)) {
    console.log("feed: DATABASE_URL is not postgres://user:password@host:port/database")
    return 2
  }
  cfg.jwtSecret = getenv("JWT_SECRET") ?? ""
  if (cfg.jwtSecret.length === 0) {
    console.log("feed: JWT_SECRET is not set")
    return 2
  }
  cfg.host = getenv("HOST") ?? "0.0.0.0"
  cfg.ports = [envInt("PORT", 3000)]
  const extra: i32 = envInt("EXTRA_PORT", 0)
  if (extra > 0) {
    cfg.ports.push(extra)
  }
  const workers: i32 = envInt("HTTP_WORKERS", availableParallelism() > 0 ? availableParallelism() : 1)
  // Rule 5: at most 512 Postgres connections in all. Every worker gets at least one of each.
  let reads: i32 = envInt("DB_READ_CONNS", 48) / workers
  let writes: i32 = envInt("DB_WRITE_CONNS", 64) / workers
  reads = reads < 1 ? 1 : reads
  writes = writes < 1 ? 1 : writes
  if ((reads + writes) * workers > 512) {
    console.log(`feed: ${(reads + writes) * workers} Postgres connections is over the 512 allowed`)
    return 2
  }
  cfg.readConns = reads
  cfg.writeConns = writes
  cfg.maxInFlight = envInt("DB_MAX_IN_FLIGHT", 16)
  cfg.slots = envInt("HTTP_SLOTS", 1048576)
  cfg.idleTimeoutMs = envInt("IDLE_TIMEOUT_MS", 120000)
  cfg.gzipLevel = envInt("GZIP_LEVEL", 1)
  cfg.gzipLevel = cfg.gzipLevel > 9 ? 9 : cfg.gzipLevel
  cfg.gzipMin = envInt("GZIP_MIN", 1024)
  cfg.batchMs = (getenv("BATCH_MS") ?? "") === "0" ? 0 : envInt("BATCH_MS", 2)
  cfg.batchMin = envInt("BATCH_MIN", 32)
  cfg.pack = (getenv("DB_PACK") ?? "") !== "0"
  cfg.binary = (getenv("DB_BINARY") ?? "") !== "0"
  const prefork = new PreforkConfig()
  prefork.workers = workers
  prefork.restart = false
  const index: i32 = preforkWith(process.argv, prefork)
  if (index < 0) {
    return index === PREFORK_STOPPED ? 0 : 1
  }
  const listeners: i32[] = []
  for (let k: i32 = 0; k < toI32(cfg.ports.length); k++) {
    const fd: i32 = tcpListenFlags(cfg.host, cfg.ports[k], cfg.backlog, LISTEN_FLAGS)
    if (fd < 0) {
      console.log(`feed: cannot listen on ${cfg.host}:${cfg.ports[k]} (${fd})`)
      return 1
    }
    listeners.push(fd)
  }
  if (index === 0) {
    console.log(`feed: ${workers} workers, ${reads} read + ${writes} write Postgres connections each, ports ${cfg.ports[0]}${extra > 0 ? `,${extra}` : ""}`)
  }
  return new FeedWorker(cfg, listeners).run()
}
