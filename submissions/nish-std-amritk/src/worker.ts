// One HTTP worker: a single-threaded `pollWait` loop holding its share of both listeners (SO_REUSEPORT on PORT and
// EXTRA_PORT, one `Http1Server` each), every keep-alive connection the kernel gives it, and two `PgPool`s (reads and
// writes). Nothing is shared with the other workers.
//
// A request is routed and its token checked when its head arrives, and answered when its body has ended: either at
// once (an error), or as a ticket on a pool. The worker binds the request's one statement (SPEC.md's SQL, prepared
// once per connection), and writes the response from the rows as they arrive, straight from the connection's input
// buffer into that connection's JSON writer. It goes to the client once the group's ReadyForQuery says its implicit
// transaction committed (rule 8). A request whose connection is lost answers 500 and is never sent again: a lost
// write may have committed.
//
// Memory: an idle keep-alive connection holds no input or head buffer (`Http1Config.idleInput`); a request's state is
// a `FeedReq` from a free list; nothing is allocated per request once the worker has served its peak.
import { connectResult, netAddress, netClose, netRead, netWrite, pollAdd, pollCreate, pollModify, pollRemove, pollWait, tcpConnect } from "nish:net"
import { monotonicNanos } from "nish:process"
import { secret, wipe } from "nish:secret"
import {
  H1_BODY,
  H1_DONE,
  H1_END,
  H1_ERROR,
  H1_NEED_MORE,
  H1_POOL_FULL,
  H1_REQUEST,
  H1_WANT_READ,
  H1_WANT_WRITE,
  Http1Config,
  Http1Connection,
  Http1Server,
} from "nish/net/http1-server"
import { Http1Parser } from "nish/net/http1"
import { HTTP_CODING_IDENTITY, HttpCompressor } from "nish/net/http-compress"
import { JsonWriter } from "nish/json"
import {
  JSON_TOK_ARRAY_BEGIN,
  JSON_TOK_ARRAY_END,
  JSON_TOK_END,
  JSON_TOK_KEY,
  JSON_TOK_OBJECT_BEGIN,
  JSON_TOK_OBJECT_END,
  JSON_TOK_STRING,
  JsonLimits,
  JsonTokenizer,
} from "nish/json-tokenizer"
import {
  PG_COMPLETE,
  PG_ERROR,
  PG_FAILED,
  PG_GROUP_DONE,
  PG_LOST,
  PG_NEED_MORE,
  PG_POOL_FULL,
  PG_ROW,
  PgConfig,
  PgConnection,
  PgPool,
} from "nish/net/postgres"
import { JWT_INVALID, JWT_OK, Jwt } from "./jwt"

const R_NONE: i32 = 0
const R_HEALTH: i32 = 1
const R_FEED: i32 = 2
const R_POST: i32 = 3
const R_CREATE: i32 = 4
const R_LIKE: i32 = 5

const POOL_READ: i32 = 0
const POOL_WRITE: i32 = 1

/** Loop tokens: HTTP slots are `server * SLOT_SPAN + slot`, listeners and pool slots negative. */
const SLOT_SPAN: i32 = 16777216
const TOKEN_LISTENER: i32 = -1
const TOKEN_POOL: i32 = -16
const POOL_TOKENS: i32 = 1024

const AGAIN: i32 = -11
const READABLE: i32 = 5
const TICKET_RING: i32 = 131072
const QUEUE: i32 = 65536
const PG_MAX_OUTPUT: i32 = 4194304
/** The largest response body: 20 posts whose 500-character bodies are all escapes. */
const RESPONSE_ROOM: i32 = 262144
/** The longest post body SPEC.md allows, in characters, and the request body the server reads. */
const MAX_CHARS: i32 = 500
const MAX_BODY: i32 = 65536

/** The SELECT of SPEC.md's post object. */
const POST_SQL: string =
  "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id"

const bytesOf = (s: string): u8[] => {
  const out: u8[] = new Array<u8>(s.length)
  for (let k: i32 = 0; k < toI32(s.length); k++) {
    out[k] = toU8(s.charCodeAt(k))
  }
  return out
}

/** Whether `b[start, end)` is exactly the ASCII `s`. */
const bytesAre = (b: u8[], start: i32, end: i32, s: string): boolean => {
  if (end - start !== toI32(s.length)) {
    return false
  }
  for (let k: i32 = 0; k < end - start; k++) {
    if (toI32(b[start + k]) !== toI32(s.charCodeAt(k))) {
      return false
    }
  }
  return true
}

/** `b[start, end)` as a positive decimal id (digits only, at most 18), or -1. */
const idOf = (b: u8[], start: i32, end: i32): i64 => {
  if (end <= start || end - start > 18) {
    return -1
  }
  let v: i64 = 0
  for (let k: i32 = start; k < end; k++) {
    const d: i32 = toI32(b[k]) - 48
    if (d < 0 || d > 9) {
      return -1
    }
    v = v * 10 + toI64(d)
  }
  return v > 0 ? v : -1
}

/** JavaScript's `trim()` whitespace in ASCII: space, tab, LF, VT, FF, CR. */
const isSpace = (c: i32): boolean => c === 32 || (c >= 9 && c <= 13)

export class WorkerConfig {
  host: string = "0.0.0.0"
  ports: i32[]
  dbHost: string = "127.0.0.1"
  dbPort: i32 = 5432
  dbUser: string = "feed"
  dbPassword: string = ""
  dbName: string = "feed"
  readConns: i32 = 8
  writeConns: i32 = 2
  maxInFlight: i32 = 16
  slots: i32 = 1048576
  backlog: i32 = 65535
  idleTimeoutMs: i32 = 120000
  jwtSecret: string = ""
  /** gzip: the deflate level (1 is `DEFLATE_FAST`), and the shortest body worth compressing. */
  gzipLevel: i32 = 1
  gzipMin: i32 = 1024
  /** The wake-up batching window: after a loop turn with fewer than `batchMin` events, pause `batchMs` (0 = off). */
  batchMs: i32 = 2
  batchMin: i32 = 32
  /** Reads fill one connection to `maxInFlight` before the next (`PgConfig.pack`), so their results go in fuller packets. */
  pack: boolean = true
  /** Reads ask for binary results: ids, like counts and timestamps come as fixed-size integers, not text. */
  binary: boolean = true
  /** `monotonicNanos` when the server started, for /health's `uptime_s`. */
  started: i64 = 0

  constructor() {
    this.ports = [3000]
  }
}

/** One request in progress. */
class FeedReq {
  index: i32 = 0
  /** The HTTP server and slot, and the slot's generation when the request came. */
  server: i32 = 0
  slot: i32 = -1
  gen: i32 = 0
  route: i32 = 0
  /** The coding `Accept-Encoding` asked for, read with the head. */
  coding: i32 = 0
  /** A status to answer once the body has ended, and its message; 0 for none. */
  status: i32 = 0
  message: string = ""
  postId: i64 = 0
  userId: i64 = 0
  name: u8[]
  nameLen: i32 = 0
  body: u8[]
  bodyLen: i32 = 0
  /** The trimmed post body, decoded. */
  text: u8[]
  textStart: i32 = 0
  textLen: i32 = 0
  pool: i32 = -1
  ticket: i32 = -1
  inPool: boolean = false
  rows: i32 = 0
  affected: i64 = 0
  /** The SQLSTATE of an ErrorResponse, or "" for none. */
  pgError: string = ""
  newId: i64 = 0
  stamp: u8[]
  stampLen: i32 = 0

  constructor(index: i32) {
    this.index = index
    this.name = new Array<u8>(256)
    this.body = new Array<u8>(1024)
    this.text = new Array<u8>(2048)
    this.stamp = new Array<u8>(24)
  }

  restart(server: i32, slot: i32, gen: i32): void {
    this.server = server
    this.slot = slot
    this.gen = gen
    this.route = R_NONE
    this.coding = HTTP_CODING_IDENTITY
    this.status = 0
    this.message = ""
    this.postId = 0
    this.userId = 0
    this.nameLen = 0
    this.bodyLen = 0
    this.textStart = 0
    this.textLen = 0
    this.pool = -1
    this.ticket = -1
    this.inPool = false
    this.rows = 0
    this.affected = 0
    this.pgError = ""
    this.newId = 0
    this.stampLen = 0
  }

  /** Stops the request early with `status` and `{"error":message}`, answered once its body has ended. */
  refuse(status: i32, message: string): void {
    if (this.status === 0) {
      this.status = status
      this.message = message
    }
  }

  addBody(src: u8[], off: i32, len: i32): void {
    if (this.bodyLen + len > MAX_BODY) {
      this.refuse(400, "malformed JSON body")
      return
    }
    if (this.bodyLen + len > toI32(this.body.length)) {
      let size: i32 = toI32(this.body.length) * 2
      while (size < this.bodyLen + len) {
        size = size * 2
      }
      const grown: u8[] = new Array<u8>(size)
      for (let k: i32 = 0; k < this.bodyLen; k++) {
        grown[k] = this.body[k]
      }
      this.body = grown
    }
    for (let k: i32 = 0; k < len; k++) {
      this.body[this.bodyLen + k] = src[off + k]
    }
    this.bodyLen += len
  }
}

class PgSocket {
  fd: i32 = -1
  connected: boolean = false
  armed: i32 = 0
}

const pgConfig = (cfg: WorkerConfig, pack: boolean): PgConfig => {
  const config = new PgConfig()
  config.user = cfg.dbUser
  config.database = cfg.dbName
  config.applicationName = "feed-nish"
  // Session settings for every connection (rule 2 allows them): generic plans for the prepared statements, no JIT
  // or parallel workers for these small statements, and the ISO/UTC timestamps `PgText.timestamp` reads.
  config.options =
    "-c plan_cache_mode=force_generic_plan -c jit=off -c max_parallel_workers_per_gather=0 -c DateStyle=ISO -c TimeZone=UTC"
  config.maxInFlight = cfg.maxInFlight
  config.pack = pack
  config.inputSize = 65536
  config.maxOutput = PG_MAX_OUTPUT
  return config
}

export class FeedWorker {
  servers: Http1Server[]
  listeners: i32[]
  loop: i32 = -1
  /** An epoll set with nothing in it: waiting on it is the batching window's pause. */
  pause: i32 = -1
  batchMs: i32 = 2
  batchMin: i32 = 32
  binary: boolean = true
  jwt: Jwt
  pools: PgPool[]
  sockets: PgSocket[][]
  writers: JsonWriter[][]
  buffers: u8[][][]
  current: i32[][]
  tickets: i32[][]
  pgAddress: u8[]
  reqs: FeedReq[]
  free: i32[]
  /** Per server and slot: the request in progress (-1 for none) and the slot's generation. */
  slotReq: i32[][]
  slotGen: i32[][]
  jsonNames: u8[][]
  jsonValues: u8[][]
  plain: JsonWriter
  plainBuf: u8[]
  tokenizer: JsonTokenizer
  gzip: HttpCompressor
  started: i64 = 0

  constructor(cfg: WorkerConfig, listeners: i32[]) {
    this.started = cfg.started
    const http = new Http1Config()
    http.idleTimeout = cfg.idleTimeoutMs
    http.maxBody = MAX_BODY
    http.outputSize = RESPONSE_ROOM + 4096
    // Nearly every one of the million connections sits between requests: none holds an input or head buffer then.
    http.idleInput = true
    this.loop = pollCreate()
    this.pause = pollCreate()
    this.batchMs = cfg.batchMs
    this.batchMin = cfg.batchMin
    this.binary = cfg.binary
    this.listeners = listeners
    this.servers = []
    this.slotReq = []
    this.slotGen = []
    for (let k: i32 = 0; k < toI32(listeners.length); k++) {
      this.servers.push(new Http1Server(http, listeners[k], cfg.slots))
      pollAdd(this.loop, listeners[k], H1_WANT_READ, TOKEN_LISTENER - k)
      const req: i32[] = new Array<i32>(cfg.slots)
      for (let s: i32 = 0; s < cfg.slots; s++) {
        req[s] = -1
      }
      this.slotReq.push(req)
      this.slotGen.push(new Array<i32>(cfg.slots))
    }
    this.jwt = new Jwt(bytesOf(cfg.jwtSecret))
    const password = secret(bytesOf(cfg.dbPassword))
    this.pools = [
      new PgPool(pgConfig(cfg, cfg.pack), password, cfg.readConns, QUEUE),
      // Writes stay spread over their connections, so their commits' WAL flushes run side by side.
      new PgPool(pgConfig(cfg, false), password, cfg.writeConns, QUEUE),
    ]
    wipe(password)
    this.sockets = []
    this.writers = []
    this.buffers = []
    this.current = []
    this.tickets = []
    const none: i32[] = []
    for (let p: i32 = 0; p < 2; p++) {
      const pool: PgPool = this.pools[p]
      // The SQL of SPEC.md, one statement per request.
      pool.register("feed", POST_SQL + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", none)
      pool.register("post", POST_SQL + " WHERE p.id = $1", none)
      pool.register("health", "SELECT 1", none)
      if (p === POOL_WRITE) {
        pool.register("create", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", none)
        pool.register(
          "like",
          "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
          none
        )
      }
      const socks: PgSocket[] = []
      const ws: JsonWriter[] = []
      const bufs: u8[][] = []
      const cur: i32[] = []
      for (let s: i32 = 0; s < pool.size; s++) {
        socks.push(new PgSocket())
        const buf: u8[] = new Array<u8>(RESPONSE_ROOM)
        bufs.push(buf)
        ws.push(new JsonWriter(buf, 0))
        cur.push(-1)
      }
      this.sockets.push(socks)
      this.writers.push(ws)
      this.buffers.push(bufs)
      this.current.push(cur)
      this.tickets.push(new Array<i32>(TICKET_RING))
    }
    this.pgAddress = new Array<u8>(18)
    netAddress(this.pgAddress, cfg.dbHost, cfg.dbPort)
    this.reqs = []
    this.free = []
    this.jsonNames = [bytesOf("Content-Type")]
    this.jsonValues = [bytesOf("application/json")]
    this.plainBuf = new Array<u8>(8192)
    this.plain = new JsonWriter(this.plainBuf, 0)
    const limits = new JsonLimits()
    limits.maxDepth = 32
    this.tokenizer = new JsonTokenizer(this.plainBuf, 0, 0, limits)
    // A `gzipMin` past the largest body turns gzip off (`HttpCompressor` refuses a minimum above its maximum).
    this.gzip = new HttpCompressor(cfg.gzipLevel, cfg.gzipMin < RESPONSE_ROOM ? cfg.gzipMin : RESPONSE_ROOM, RESPONSE_ROOM)
    // No cache sits between the load generator and the server, and `Vary` would add 24 bytes to every response.
    this.gzip.vary = false
  }

  // ---- The loop ---------------------------------------------------------------------

  run(): i32 {
    const ready: i32[] = new Array<i32>(2 * 1024)
    while (true) {
      this.pump()
      const n: i32 = pollWait(this.loop, ready, 1000)
      if (n < 0 && n !== -4) {
        return 1
      }
      for (let k: i32 = 0; k < n; k++) {
        this.ready(ready[2 * k], ready[2 * k + 1])
      }
      const now: i64 = monotonicNanos()
      for (let k: i32 = 0; k < toI32(this.servers.length); k++) {
        this.servers[k].expire(now)
      }
      // A light turn: send what it produced, then let events gather for a moment. The next turn serves them in one
      // wake-up, and their queries go to Postgres together, in fewer and fuller packets.
      if (this.batchMs > 0 && n < this.batchMin) {
        this.pump()
        pollWait(this.pause, ready, this.batchMs)
      }
    }
  }

  ready(token: i32, events: i32): void {
    if (token >= 0) {
      const sv: i32 = token / SLOT_SPAN
      const slot: i32 = token % SLOT_SPAN
      if (sv < toI32(this.servers.length) && this.servers[sv].holds(slot)) {
        if ((events & READABLE) !== 0) {
          this.servers[sv].readable(slot)
        } else {
          this.servers[sv].writable(slot)
        }
        this.drive(sv, slot)
      }
    } else if (token > TOKEN_POOL) {
      this.acceptAll(TOKEN_LISTENER - token)
    } else {
      const n: i32 = TOKEN_POOL - token
      const p: i32 = n / POOL_TOKENS
      const s: i32 = n % POOL_TOKENS
      if (p >= 0 && p < 2 && s < this.pools[p].size) {
        this.pgReady(p, s, events)
      }
    }
  }

  acceptAll(sv: i32): void {
    if (sv < 0 || sv >= toI32(this.servers.length)) {
      return
    }
    const server: Http1Server = this.servers[sv]
    while (true) {
      const slot: i32 = server.accept()
      if (slot >= 0) {
        if (slot < toI32(this.slotGen[sv].length)) {
          // `expire` closes a slot without telling the worker: a request it left outside a pool is freed here.
          const old: i32 = this.slotReq[sv][slot]
          if (old >= 0 && !this.reqs[old].inPool) {
            this.release(this.reqs[old])
          }
          this.slotGen[sv][slot] = this.slotGen[sv][slot] + 1
          this.slotReq[sv][slot] = -1
        }
        pollAdd(this.loop, server.fd(slot), H1_WANT_READ, sv * SLOT_SPAN + slot)
      } else if (slot !== H1_POOL_FULL) {
        return
      }
    }
  }

  drive(sv: i32, slot: i32): void {
    const server: Http1Server = this.servers[sv]
    let event: i32 = server.next(slot)
    while (event !== H1_NEED_MORE && event !== H1_ERROR) {
      if (event === H1_REQUEST) {
        this.onHead(sv, slot)
      } else if (event === H1_BODY) {
        const r: i32 = this.slotReq[sv][slot]
        if (r >= 0) {
          const conn: Http1Connection = server.connection(slot)
          this.reqs[r].addBody(conn.data, conn.dataStart, conn.dataLength)
        }
      } else if (event === H1_END) {
        const r: i32 = this.slotReq[sv][slot]
        if (r >= 0) {
          this.onEnd(this.reqs[r])
        }
      }
      event = server.next(slot)
    }
    if (!server.holds(slot)) {
      return // closed by `answer`
    }
    const wants: i32 = server.flush(slot)
    if ((wants & H1_DONE) !== 0) {
      this.closeSlot(sv, slot)
    } else if (server.rearm(slot, wants)) {
      pollModify(this.loop, server.fd(slot), wants & (H1_WANT_READ | H1_WANT_WRITE), sv * SLOT_SPAN + slot)
    }
  }

  closeSlot(sv: i32, slot: i32): void {
    this.servers[sv].close(slot)
    if (slot >= 0 && slot < toI32(this.slotReq[sv].length)) {
      this.slotGen[sv][slot] = this.slotGen[sv][slot] + 1
      const r: i32 = this.slotReq[sv][slot]
      this.slotReq[sv][slot] = -1
      // A request still in a pool is freed when its answer comes back.
      if (r >= 0 && !this.reqs[r].inPool) {
        this.release(this.reqs[r])
      }
    }
  }

  take(sv: i32, slot: i32): FeedReq {
    if (this.free.length === 0) {
      this.reqs.push(new FeedReq(toI32(this.reqs.length)))
      this.free.push(toI32(this.reqs.length) - 1)
    }
    const r: FeedReq = this.reqs[this.free[this.free.length - 1]]
    this.free.pop()
    r.restart(sv, slot, this.slotGen[sv][slot])
    return r
  }

  release(r: FeedReq): void {
    const sv: i32 = r.server
    if (r.slot >= 0 && r.slot < toI32(this.slotReq[sv].length) && this.slotReq[sv][r.slot] === r.index) {
      this.slotReq[sv][r.slot] = -1
    }
    r.slot = -1
    r.inPool = false
    this.free.push(r.index)
  }

  // ---- Answers ----------------------------------------------------------------------

  /**
   * Answers `r` with `buf[0, len)` as its JSON body, and frees it. `later` is true when the answer comes from a pool
   * rather than the slot's own events, so the slot is driven after it: its next pipelined request may be waiting.
   */
  answer(r: FeedReq, status: i32, buf: u8[], len: i32, later: boolean): void {
    const sv: i32 = r.server
    const slot: i32 = r.slot
    const server: Http1Server = this.servers[sv]
    const live: boolean =
      slot >= 0 && slot < toI32(this.slotGen[sv].length) && server.holds(slot) && this.slotGen[sv][slot] === r.gen
    const coding: i32 = r.coding
    this.release(r)
    if (!live) {
      return
    }
    const conn: Http1Connection = server.connection(slot)
    // gzipped when the client asked for it and the body is at least `gzipMin` bytes, else sent as it is.
    if (conn.respondEncoded(status, this.jsonNames, this.jsonValues, buf, 0, len, this.gzip, coding) !== 0) {
      // No room in the output for a whole response: a client that pipelines without reading. Not served further.
      this.closeSlot(sv, slot)
      return
    }
    if (later) {
      this.drive(sv, slot)
    }
  }

  answerWritten(r: FeedReq, status: i32, w: JsonWriter, buf: u8[]): void {
    if (w.failed !== 0) {
      this.fail(r, 500, "internal server error", true)
      return
    }
    this.answer(r, status, buf, w.end, true)
  }

  /** Answers `r` with `{"error":message}`. */
  fail(r: FeedReq, status: i32, message: string, later: boolean): void {
    const w: JsonWriter = this.plain
    w.reset(this.plainBuf, 0)
    w.beginObject()
    w.key("error")
    w.string(message)
    w.endObject()
    this.answer(r, status, this.plainBuf, w.end, later)
  }

  // ---- A request's head and body ------------------------------------------------------

  onHead(sv: i32, slot: i32): void {
    const r: FeedReq = this.take(sv, slot)
    this.slotReq[sv][slot] = r.index
    const conn: Http1Connection = this.servers[sv].connection(slot)
    const p: Http1Parser = conn.parser
    r.coding = conn.acceptEncoding()
    this.route(r, p)
    if (r.status !== 0 || (r.route !== R_CREATE && r.route !== R_LIKE)) {
      return
    }
    // Authenticated routes: the token first, then the id (SPEC.md).
    const h: i32 = p.headerIndex("authorization")
    if (h < 0) {
      r.refuse(401, "missing bearer token")
      return
    }
    const start: i32 = p.spans[4 * h + 2]
    const end: i32 = p.spans[4 * h + 3]
    if (end - start < 7 || !bytesAre(p.head, start, start + 7, "Bearer ")) {
      r.refuse(401, "missing bearer token")
      return
    }
    const verdict: i32 = this.jwt.verify(p.head, start + 7, end - start - 7, toI64(Date.now()) / 1000)
    if (verdict !== JWT_OK) {
      r.refuse(401, verdict === JWT_INVALID ? "invalid or expired token" : "invalid token payload")
      return
    }
    r.userId = this.jwt.sub
    const n: i32 = this.jwt.nameLen
    if (n > toI32(r.name.length)) {
      r.name = new Array<u8>(n)
    }
    for (let k: i32 = 0; k < n; k++) {
      r.name[k] = this.jwt.name[k]
    }
    r.nameLen = n
    if (r.route === R_LIKE && r.postId < 0) {
      r.refuse(400, "invalid post id")
    }
  }

  /** Sets `r.route` (and `r.postId`) from the request line, or refuses it with 404 / 400. */
  route(r: FeedReq, p: Http1Parser): void {
    const b: u8[] = p.head
    const start: i32 = p.targetStart
    let end: i32 = start
    while (end < p.targetEnd && toI32(b[end]) !== 63) {
      end++
    }
    const get: boolean = p.methodIs("GET")
    const post: boolean = p.methodIs("POST")
    if (get && bytesAre(b, start, end, "/feed")) {
      r.route = R_FEED
      return
    }
    if (get && bytesAre(b, start, end, "/health")) {
      r.route = R_HEALTH
      return
    }
    if (post && bytesAre(b, start, end, "/posts")) {
      r.route = R_CREATE
      return
    }
    if (end - start > 7 && bytesAre(b, start, start + 7, "/posts/")) {
      let slash: i32 = start + 7
      while (slash < end && toI32(b[slash]) !== 47) {
        slash++
      }
      if (get && slash === end) {
        r.route = R_POST
        r.postId = idOf(b, start + 7, end)
        if (r.postId < 0) {
          r.refuse(400, "invalid post id")
        }
        return
      }
      if (post && bytesAre(b, slash, end, "/like")) {
        // The id is checked after the token (onHead).
        r.route = R_LIKE
        r.postId = idOf(b, start + 7, slash)
        return
      }
    }
    r.refuse(404, "not found")
  }

  /** The body has ended: answer a refused request, or check the post body and send the statement. */
  onEnd(r: FeedReq): void {
    if (r.status !== 0) {
      this.fail(r, r.status, r.message, false)
      return
    }
    if (r.route === R_CREATE && !this.postBody(r)) {
      this.fail(r, r.status, r.message, false)
      return
    }
    this.submit(r, false)
  }

  /**
   * Reads `{"body": "..."}`: the whole text must be JSON (else 400 malformed), `body` a string that is not empty
   * once trimmed (else 400 required) and at most 500 characters (code points, as Postgres's `char_length` counts).
   * The last `body` key wins, as with `JSON.parse`.
   */
  postBody(r: FeedReq): boolean {
    const t: JsonTokenizer = this.tokenizer
    t.reset(r.body, 0, r.bodyLen)
    let depth: i32 = 0
    let found: boolean = false
    let first: boolean = true
    let isObject: boolean = false
    let k: i32 = t.next()
    while (k > 0) {
      if (first) {
        isObject = k === JSON_TOK_OBJECT_BEGIN
        first = false
      }
      if (k === JSON_TOK_OBJECT_BEGIN || k === JSON_TOK_ARRAY_BEGIN) {
        depth++
      } else if (k === JSON_TOK_OBJECT_END || k === JSON_TOK_ARRAY_END) {
        depth--
      } else if (k === JSON_TOK_KEY && depth === 1 && isObject && t.keyEquals("body")) {
        const v: i32 = t.next()
        if (v < 0) {
          k = v
          break
        }
        found = false
        if (v === JSON_TOK_STRING) {
          const len: i32 = t.stringLength()
          if (len > toI32(r.text.length)) {
            r.text = new Array<u8>(len)
          }
          const n: i32 = t.stringInto(r.text, 0)
          if (n >= 0) {
            found = true
            r.textStart = 0
            r.textLen = n
          }
        } else if (v === JSON_TOK_OBJECT_BEGIN || v === JSON_TOK_ARRAY_BEGIN) {
          depth++
        }
      }
      k = t.next()
    }
    if (k !== JSON_TOK_END || t.failed !== 0 || first) {
      r.refuse(400, "malformed JSON body")
      return false
    }
    if (!found) {
      r.refuse(400, "body is required")
      return false
    }
    let s: i32 = r.textStart
    let e: i32 = r.textStart + r.textLen
    while (s < e && isSpace(toI32(r.text[s]))) {
      s++
    }
    while (e > s && isSpace(toI32(r.text[e - 1]))) {
      e--
    }
    if (s === e) {
      r.refuse(400, "body is required")
      return false
    }
    let chars: i32 = 0
    for (let q: i32 = s; q < e; q++) {
      if ((toI32(r.text[q]) & 192) !== 128) {
        chars++
      }
    }
    if (chars > MAX_CHARS) {
      r.refuse(400, "body must be at most 500 characters")
      return false
    }
    r.textStart = s
    r.textLen = e - s
    return true
  }

  // ---- Postgres ------------------------------------------------------------------------

  submit(r: FeedReq, later: boolean): void {
    const p: i32 = r.route === R_CREATE || r.route === R_LIKE ? POOL_WRITE : POOL_READ
    const ticket: i32 = this.pools[p].request()
    if (ticket === PG_POOL_FULL) {
      this.fail(r, 500, "internal server error", later)
      return
    }
    this.tickets[p][ticket % TICKET_RING] = r.index
    r.pool = p
    r.ticket = ticket
    r.inPool = true
  }

  /** Hands queued requests to connections, opens the connections the pools want, and sends what they hold. */
  pump(): void {
    for (let p: i32 = 0; p < 2; p++) {
      const pool: PgPool = this.pools[p]
      let s: i32 = pool.dispatch()
      while (s >= 0) {
        const conn: PgConnection = pool.connection(s)
        const ri: i32 = this.tickets[p][pool.ticket % TICKET_RING]
        const bound: boolean = ri >= 0 && ri < toI32(this.reqs.length) && this.bind(this.reqs[ri], conn)
        if (!bound) {
          conn.bindCancel()
        }
        if (!pool.send() && conn.bindEnd()) {
          pool.send()
        }
        if (!bound && ri >= 0 && this.reqs[ri].inPool) {
          this.reqs[ri].inPool = false
          this.fail(this.reqs[ri], 500, "internal server error", true)
        }
        s = pool.dispatch()
      }
      s = pool.wanted()
      while (s >= 0 && s < pool.size) {
        const socket: PgSocket = this.sockets[p][s]
        const nonce = new Array<u8>(18)
        crypto.getRandomValues(nonce)
        pool.connect(s, nonce)
        socket.fd = tcpConnect(this.pgAddress)
        socket.connected = false
        socket.armed = H1_WANT_WRITE
        if (socket.fd < 0 || pollAdd(this.loop, socket.fd, H1_WANT_WRITE, TOKEN_POOL - (p * POOL_TOKENS + s)) < 0) {
          pool.connection(s).closed()
          this.pgEvents(p, s)
        }
        s = pool.wanted()
      }
      for (let k: i32 = 0; k < pool.size; k++) {
        const socket: PgSocket = this.sockets[p][k]
        if (socket.fd >= 0 && socket.connected) {
          const conn: PgConnection = pool.connection(k)
          if (!this.pgFlush(conn, socket.fd)) {
            conn.closed()
            this.pgEvents(p, k)
          } else if (socket.fd >= 0) {
            // Re-armed only on a change: each pollModify is an epoll_ctl.
            const wants: i32 = conn.outputEnd() > conn.outputStart() ? H1_WANT_READ | H1_WANT_WRITE : H1_WANT_READ
            if (wants !== socket.armed) {
              socket.armed = wants
              pollModify(this.loop, socket.fd, wants, TOKEN_POOL - (p * POOL_TOKENS + k))
            }
          }
        }
      }
    }
  }

  bind(r: FeedReq, conn: PgConnection): boolean {
    switch (r.route) {
      case R_HEALTH:
        return conn.bindStart("health") && conn.bindEnd()
      case R_FEED:
        return (this.binary ? conn.bindStartBinary("feed") : conn.bindStart("feed")) && conn.bindEnd()
      case R_POST:
        return (
          (this.binary ? conn.bindStartBinary("post") : conn.bindStart("post")) &&
          conn.paramInt(r.postId) &&
          conn.bindEnd()
        )
      case R_CREATE:
        return (
          conn.bindStart("create") &&
          conn.paramInt(r.userId) &&
          conn.paramBytes(r.text, r.textStart, r.textLen) &&
          conn.bindEnd()
        )
      case R_LIKE:
        return conn.bindStart("like") && conn.paramInt(r.userId) && conn.paramInt(r.postId) && conn.bindEnd()
      default:
        return false
    }
  }

  pgFlush(conn: PgConnection, fd: i32): boolean {
    while (conn.outputEnd() > conn.outputStart()) {
      const n: i32 = netWrite(fd, conn.outputBuffer(), conn.outputStart(), conn.outputEnd() - conn.outputStart())
      if (n === AGAIN) {
        return true
      }
      if (n <= 0) {
        return false
      }
      conn.sent(n)
    }
    return true
  }

  pgReady(p: i32, s: i32, events: i32): void {
    const socket: PgSocket = this.sockets[p][s]
    if (socket.fd < 0) {
      return
    }
    const conn: PgConnection = this.pools[p].connection(s)
    if (!socket.connected) {
      if (connectResult(socket.fd) === 0) {
        socket.connected = true
      } else {
        conn.closed()
      }
      this.pgEvents(p, s)
      return
    }
    if ((events & READABLE) !== 0) {
      while (true) {
        let room: i32 = conn.inputRoom()
        if (room <= 0) {
          this.pgEvents(p, s)
          if (socket.fd < 0) {
            return
          }
          room = conn.inputRoom()
          if (room <= 0) {
            break
          }
        }
        const got: i32 = netRead(socket.fd, conn.inputBuffer(), conn.inputEnd(), room)
        if (got === AGAIN) {
          break
        }
        if (got <= 0) {
          conn.closed()
          break
        }
        conn.received(got)
        if (got < room) {
          break
        }
        this.pgEvents(p, s)
        if (socket.fd < 0) {
          return
        }
      }
    }
    this.pgEvents(p, s)
  }

  pgEvents(p: i32, s: i32): void {
    const pool: PgPool = this.pools[p]
    const conn: PgConnection = pool.connection(s)
    let event: i32 = pool.next(s)
    while (event !== PG_NEED_MORE) {
      if (event === PG_FAILED) {
        const socket: PgSocket = this.sockets[p][s]
        if (socket.fd >= 0) {
          pollRemove(this.loop, socket.fd)
          netClose(socket.fd)
          socket.fd = -1
          socket.connected = false
        }
        this.current[p][s] = -1
        return
      }
      const ticket: i32 = pool.ticket
      if (ticket >= 0) {
        const ri: i32 = this.tickets[p][ticket % TICKET_RING]
        if (ri >= 0 && ri < toI32(this.reqs.length) && this.reqs[ri].inPool && this.reqs[ri].ticket === ticket) {
          this.onEvent(this.reqs[ri], p, s, conn, event)
        }
      }
      event = pool.next(s)
    }
  }

  onEvent(r: FeedReq, p: i32, s: i32, conn: PgConnection, event: i32): void {
    const w: JsonWriter = this.writers[p][s]
    if (this.current[p][s] !== r.ticket) {
      this.current[p][s] = r.ticket
      w.reset(this.buffers[p][s], 0)
      w.beginObject()
      if (r.route === R_FEED) {
        w.key("posts")
        w.beginArray()
      } else if (r.route === R_POST) {
        w.key("post")
      }
    }
    if (event === PG_ROW) {
      if (r.route === R_FEED || (r.route === R_POST && r.rows === 0)) {
        this.post(w, conn)
      } else if (r.route === R_CREATE) {
        r.newId = conn.int(0)
        r.stampLen = conn.text.timestamp(conn.inputBuffer(), conn.colStart(1), conn.colLength(1), r.stamp, 0)
      }
      r.rows += 1
    } else if (event === PG_COMPLETE) {
      r.affected = conn.tagRows()
    } else if (event === PG_ERROR) {
      const code: string = conn.lastError?.code ?? ""
      r.pgError = code.length > 0 ? code : "XX000"
    } else if (event === PG_GROUP_DONE || event === PG_LOST) {
      this.current[p][s] = -1
      this.tickets[p][r.ticket % TICKET_RING] = -1
      r.inPool = false
      if (event === PG_LOST) {
        r.pgError = "08006"
      } else if (conn.groupFailed && r.pgError.length === 0) {
        r.pgError = "XX000"
      }
      this.finish(r, w, this.buffers[p][s])
    }
  }

  /** One row of SPEC.md's post query as a post object, its columns in binary or in text as the bind asked. */
  post(w: JsonWriter, conn: PgConnection): void {
    const b: u8[] = conn.inputBuffer()
    w.beginObject()
    w.key("id")
    if (this.binary) {
      w.i64(conn.binaryInt(0))
    } else {
      w.raw(b, conn.colStart(0), conn.colLength(0))
    }
    w.key("body")
    w.stringBytes(b, conn.colStart(1), conn.colLength(1))
    w.key("created_at")
    const stamp: u8[] = this.plainBuf
    const n: i32 = this.binary
      ? conn.binary.timestamp(b, conn.colStart(2), conn.colLength(2), stamp, 0)
      : conn.text.timestamp(b, conn.colStart(2), conn.colLength(2), stamp, 0)
    if (n < 0) {
      w.nullValue()
    } else {
      w.stringBytes(stamp, 0, n)
    }
    w.key("author")
    w.stringBytes(b, conn.colStart(3), conn.colLength(3))
    w.key("like_count")
    if (this.binary) {
      w.i64(conn.binaryInt(4))
    } else {
      w.raw(b, conn.colStart(4), conn.colLength(4))
    }
    w.endObject()
  }

  finish(r: FeedReq, w: JsonWriter, buf: u8[]): void {
    if (r.route === R_HEALTH) {
      const h: JsonWriter = this.plain
      h.reset(this.plainBuf, 0)
      h.beginObject()
      if (r.pgError.length === 0) {
        h.key("status")
        h.string("ok")
        h.key("db")
        h.string("ok")
        h.key("uptime_s")
        h.i64((monotonicNanos() - this.started) / 1000000000)
        h.endObject()
        this.answer(r, 200, this.plainBuf, h.end, true)
      } else {
        h.key("status")
        h.string("degraded")
        h.key("db")
        h.string("unreachable")
        h.key("error")
        h.string("SQLSTATE " + r.pgError)
        h.endObject()
        this.answer(r, 503, this.plainBuf, h.end, true)
      }
      return
    }
    if (r.route === R_LIKE && r.pgError === "23503") {
      this.fail(r, 404, "post not found", true)
      return
    }
    if (r.pgError.length > 0) {
      console.log(`feed: query failed, SQLSTATE ${r.pgError}`)
      this.fail(r, 500, "internal server error", true)
      return
    }
    if (r.route === R_FEED) {
      w.endArray()
      w.endObject()
      this.answerWritten(r, 200, w, buf)
    } else if (r.route === R_POST) {
      if (r.rows === 0) {
        this.fail(r, 404, "post not found", true)
        return
      }
      w.endObject()
      this.answerWritten(r, 200, w, buf)
    } else if (r.route === R_CREATE) {
      w.reset(buf, 0)
      w.beginObject()
      w.key("post")
      w.beginObject()
      w.key("id")
      w.i64(r.newId)
      w.key("body")
      w.stringBytes(r.text, r.textStart, r.textLen)
      w.key("created_at")
      w.stringBytes(r.stamp, 0, r.stampLen < 0 ? 0 : r.stampLen)
      w.key("author")
      w.stringBytes(r.name, 0, r.nameLen)
      w.key("like_count")
      w.i64(toI64(0))
      w.endObject()
      w.endObject()
      this.answerWritten(r, 201, w, buf)
    } else {
      w.reset(buf, 0)
      w.beginObject()
      w.key("liked")
      w.boolean(true)
      w.key("already_liked")
      w.boolean(r.affected <= 0)
      w.key("post_id")
      w.i64(r.postId)
      w.endObject()
      this.answerWritten(r, r.affected > 0 ? 201 : 200, w, buf)
    }
  }
}
