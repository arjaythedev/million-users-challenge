// The worker: one epoll loop on this process's only JavaScript thread. It owns its SO_REUSEPORT listeners on both
// ports, its client connections and its Postgres connections, all in one epoll set. The loop is synchronous: no
// Promise, timer or runtime I/O is used once it starts, so nothing else ever needs the thread.
//
// A client connection is a few bytes in typed arrays indexed by fd; an idle one owns no buffer and no object. A
// request that needs Postgres is collected for the whole pass; at its end the pass's reads and writes are dealt in
// contiguous runs to the least loaded connections (one Sync and one send() per connection). A generation number,
// bumped on close, drops answers for client connections that closed (and whose fd may be reused) meanwhile.
import {
  alloc, C, EAGAIN, EINTR, EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLLERR, EPOLLHUP, EPOLLIN, EPOLLOUT, EPOLLRDHUP, errno,
  EV_DATA, EV_SIZE, AF_INET, MSG_NOSIGNAL, SO_REUSEADDR, SO_REUSEPORT, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_STREAM,
  sockaddrIn, SOL_SOCKET, Z, IPPROTO_TCP, TCP_NODELAY,
} from "./ffi";
import * as http from "./http";
import { bytes, Writer } from "./json";
import * as jwt from "./jwt";
import {
  connect, Entry, lastError, onError, PgConn, type PgConfig, type PgHost, S_FEED, S_HEALTH, S_INSERT, S_LIKE, S_POST,
  setLastError, unavailable,
} from "./pg";

const MAX_EVENTS = 512;
const RECV_SIZE = 64 * 1024;
const HEAD_ROOM = 256; // bytes kept free in front of every response body for the head
const BATCH_MICROS = 2000; // a pass starts at most this often, so requests pile up and share Syncs and sends
const DEAL_SIZE = 16; // target requests per Postgres round trip
const PG_TIMEOUT_MS = 10_000;
const MAX_WAIT_MS = 5000;
const GZIP_MIN = 1024;

const K_CLIENT = 0, K_LISTENER = 1, K_PG = 3;

// per-fd connection state
const F_BUSY = 1, F_DEAD = 2, F_CLOSE_AFTER = 4, F_CONTINUE_SENT = 8, F_WANT_OUT = 16;
let cap = 1 << 16;
let gens = new Uint32Array(cap);
let flags = new Uint8Array(cap);
const inPend = new Map<number, Uint8Array>(); // bytes of a partial or pipelined request
const outPend = new Map<number, Uint8Array>(); // unsent tail of a response

function growTo(fd: number): void {
  let n = cap;
  while (n <= fd) n *= 2;
  const g = new Uint32Array(n); g.set(gens); gens = g;
  const f = new Uint8Array(n); f.set(flags); flags = f;
  cap = n;
}

// buffers with fixed addresses
const events = alloc(MAX_EVENTS * EV_SIZE);
const evU32 = new Uint32Array(events.u8.buffer);
const ev = alloc(16);
const evOne = new Uint32Array(ev.u8.buffer);
const recvBuf = alloc(RECV_SIZE);
const body = alloc(256 * 1024); // response body at HEAD_ROOM
const gz = alloc(256 * 1024); // gzipped body at HEAD_ROOM
const scratch = alloc(64);
const w = new Writer(body.u8);

let epfd = -1;
let compressor = 0;
const startedAt = Date.now();

// ---- constant bytes ----
const J_POSTS_OPEN = bytes('{"posts":[');
const J_POSTS_CLOSE = bytes("]}");
const J_POST_OPEN = bytes('{"post":');
const J_ID = bytes('{"id":');
const J_BODY = bytes(',"body":');
const J_CREATED = bytes(',"created_at":');
const J_AUTHOR = bytes(',"author":');
const J_LIKES = bytes(',"like_count":');
const J_LIKES0_CLOSE2 = bytes(',"like_count":0}}');
const J_LIKED = bytes('{"liked":true,"already_liked":');
const J_POST_ID = bytes(',"post_id":');
const J_TRUE = bytes("true");
const J_FALSE = bytes("false");
const J_HEALTH = bytes('{"status":"ok","db":"ok","uptime_s":');
const J_DEGRADED = bytes('{"status":"degraded","db":"unreachable","error":');
const J_ERROR = bytes('{"error":');
const CONTINUE = bytes("HTTP/1.1 100 Continue\r\n\r\n");
const P_FEED = bytes("/feed");
const P_HEALTH = bytes("/health");
const P_POSTS = bytes("/posts");
const P_LIKE = bytes("/like");

const STATUS_LINES: Uint8Array[] = [];
for (const [code, reason] of [[200, "OK"], [201, "Created"], [400, "Bad Request"], [401, "Unauthorized"],
  [404, "Not Found"], [413, "Payload Too Large"], [500, "Internal Server Error"], [503, "Service Unavailable"]] as const)
  STATUS_LINES[code] = bytes(`HTTP/1.1 ${code} ${reason}\r\nContent-Type: application/json\r\n`);
const H_GZIP = bytes("Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
const H_CLOSE = bytes("Connection: close\r\n");
const H_LENGTH = bytes("Content-Length: ");
let dateSecond = -1;
let dateHeader = bytes("");
function date(): Uint8Array {
  const now = Date.now();
  const s = (now - (now % 1000)) / 1000;
  if (s !== dateSecond) {
    dateSecond = s;
    dateHeader = bytes(`Date: ${new Date(now).toUTCString()}\r\n`);
  }
  return dateHeader;
}

// ---- Postgres ----
let cfg: PgConfig;
let pg: (PgConn | null)[] = [];
let readSlots = 0;
let reconnectAt: number[] = [];
const entries: Entry[] = [];
const waiting: Entry[] = [];
const passReads: Entry[] = [];
const passWrites: Entry[] = [];
const dirty: PgConn[] = [];
const byLoad: PgConn[] = [];
const toClose: number[] = [];
let processingFd = -1;

// counters for the stats line
let open = 0, accepted = 0, closed = 0, answered = 0, passes = 0, syncs = 0;
let prevAnswered = 0, prevSyncs = 0, prevPasses = 0, prevAccepted = 0;
let lastHousekeeping = 0, lastStats = 0, workerId = 0, parentPid = 0;
const exitAt = Number(process.env.EXIT_AFTER_S) > 0 ? Date.now() + Number(process.env.EXIT_AFTER_S) * 1000 : 0;

const host: PgHost = {
  readDone,
  deliver,
  requeue(e: Entry) {
    e.resetResult();
    submit(e);
  },
  setPgEvents(c: PgConn, events: number) {
    epollCtl(EPOLL_CTL_MOD, c.fd, events, K_PG, c.slot);
  },
};

function epollCtl(op: number, fd: number, events: number, kind: number, index: number): number {
  evOne.fill(0);
  evOne[0] = events;
  evOne[EV_DATA] = index;
  evOne[EV_DATA + 1] = kind;
  return C.epoll_ctl(epfd, op, fd, ev.addr);
}

function listen(ip: string, port: number): number {
  const fd = C.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) throw new Error(`socket: errno ${errno()}`);
  new DataView(scratch.u8.buffer).setInt32(0, 1, true);
  C.setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, scratch.addr, 4);
  C.setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, scratch.addr, 4);
  C.setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, scratch.addr, 4);
  const sa = alloc(16);
  sockaddrIn(sa, ip, port);
  if (C.bind(fd, sa.addr, 16) !== 0) throw new Error(`bind ${ip}:${port}: errno ${errno()}`);
  if (C.listen(fd, 65535) !== 0) throw new Error(`listen: errno ${errno()}`);
  return fd;
}

export function start(id: number, ip: string, ports: number[], pgCfg: PgConfig, reads: number, writes: number): never {
  workerId = id;
  parentPid = C.getppid();
  cfg = pgCfg;
  compressor = Z.libdeflate_alloc_compressor(1) as unknown as number;
  if (!compressor) throw new Error("libdeflate_alloc_compressor failed");
  epfd = C.epoll_create1(0o2000000);
  if (epfd < 0) throw new Error(`epoll_create1: errno ${errno()}`);
  readSlots = reads;
  pg = new Array(reads + writes).fill(null);
  reconnectAt = new Array(reads + writes).fill(0);
  for (let slot = 0; slot < pg.length; slot++) tryConnect(slot);
  for (const port of ports) {
    const lfd = listen(ip, port);
    if (epollCtl(EPOLL_CTL_ADD, lfd, EPOLLIN, K_LISTENER, lfd) !== 0) throw new Error(`epoll_ctl: errno ${errno()}`);
  }
  run();
}

function tryConnect(slot: number): void {
  try {
    const fd = connect(cfg);
    const c = new PgConn(host, slot, fd, slot >= readSlots);
    pg[slot] = c;
    epollCtl(EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLRDHUP, K_PG, slot);
    setLastError("");
    for (let k = waiting.length; k > 0; k--) submit(waiting.shift()!);
  } catch (err) {
    setLastError((err as Error).message);
    console.error(`worker ${workerId}: Postgres connection ${slot}: ${(err as Error).message}`);
    reconnectAt[slot] = Date.now() + 1000;
  }
}

function run(): never {
  for (;;) {
    const busy = passReads.length + passWrites.length > 0;
    const n = C.epoll_wait(epfd, events.addr, MAX_EVENTS, busy ? 0 : waiting.length > 0 ? 100 : 1000);
    if (n < 0) {
      if (errno() === EINTR) continue;
      throw new Error(`epoll_wait: errno ${errno()}`);
    }
    const passStart = performance.now();
    try {
      pass(n);
    } catch (err) {
      console.error(err);
    }
    for (let i = 0; i < toClose.length; i++) C.close(toClose[i]!);
    toClose.length = 0;
    passes++;
    if (n === MAX_EVENTS) continue;
    const left = BATCH_MICROS - (performance.now() - passStart) * 1000;
    if (left > 50) C.usleep(left | 0);
  }
}

function pass(n: number): void {
  const stride = EV_SIZE >> 2;
  for (let i = 0; i < n; i++) {
    const base = i * stride;
    const evs = evU32[base]!;
    const index = evU32[base + EV_DATA]!;
    const kind = evU32[base + EV_DATA + 1]!;
    if (kind === K_CLIENT) {
      if ((flags[index]! & F_DEAD) !== 0) continue;
      try {
        onConn(index, evs);
      } catch (err) {
        console.error(err);
        if ((flags[index]! & F_DEAD) === 0) close(index);
      }
    } else if (kind === K_LISTENER) {
      acceptAll(index);
    } else if (kind === K_PG) {
      const c = pg[index];
      if (c == null) continue;
      try {
        onPg(c, evs);
      } catch (err) {
        console.error(err);
        pgFail(c, String(err));
      }
    }
  }
  if (waiting.length > 0) expireWaiting();
  const now = Date.now();
  if (now - lastHousekeeping >= 1000) housekeeping(now);
  flushPg();
}

// ---- clients ----

function acceptAll(lfd: number): void {
  for (;;) {
    const fd = C.accept4(lfd, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      const err = errno();
      if (err !== EAGAIN) console.error(`worker ${workerId}: accept4: errno ${err}`);
      return;
    }
    if (fd >= cap) growTo(fd);
    flags[fd] = 0;
    if (epollCtl(EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLRDHUP, K_CLIENT, fd) !== 0) {
      gens[fd] = (gens[fd]! + 1) >>> 0;
      C.close(fd);
      continue;
    }
    accepted++;
    open++;
  }
}

function onConn(fd: number, evs: number): void {
  if ((evs & (EPOLLERR | EPOLLHUP)) !== 0) {
    close(fd);
    return;
  }
  const gen = gens[fd]!;
  if ((evs & EPOLLOUT) !== 0) {
    flushOut(fd);
    if (gens[fd] !== gen) return;
  }
  if ((evs & (EPOLLIN | EPOLLRDHUP)) !== 0) onReadable(fd);
}

function onReadable(fd: number): void {
  const n = C.recv(fd, recvBuf.addr, RECV_SIZE, 0);
  if (n <= 0) {
    if (n < 0 && errno() === EAGAIN) return;
    close(fd);
    return;
  }
  const pending = inPend.get(fd);
  if (pending === undefined && (flags[fd]! & F_BUSY) === 0 && !outPend.has(fd)) {
    const used = processReqs(fd, recvBuf.u8, 0, n);
    if (used >= 0 && used < n) append(fd, recvBuf.u8, used, n);
    return;
  }
  const gen = gens[fd]!;
  append(fd, recvBuf.u8, 0, n);
  if (gens[fd] === gen && (flags[fd]! & F_BUSY) === 0 && !outPend.has(fd)) processBuffered(fd);
}

function append(fd: number, src: Uint8Array, start: number, end: number): void {
  const old = inPend.get(fd);
  const oldLen = old === undefined ? 0 : old.length;
  if (oldLen + end - start > http.MAX_HEAD + http.MAX_BODY) {
    close(fd);
    return;
  }
  const next = new Uint8Array(oldLen + end - start);
  if (old !== undefined) next.set(old, 0);
  next.set(src.subarray(start, end), oldLen);
  inPend.set(fd, next);
}

function processBuffered(fd: number): void {
  const buf = inPend.get(fd);
  if (buf === undefined) return;
  const used = processReqs(fd, buf, 0, buf.length);
  if (used <= 0) return;
  if (gens[fd] === undefined || (flags[fd]! & F_DEAD) !== 0) return;
  if (used < buf.length) inPend.set(fd, buf.slice(used));
  else inPend.delete(fd);
}

// Handles every complete request in buf[off, end), in order, until one has to wait for Postgres. Returns the bytes
// consumed, or -1 if the connection was closed.
function processReqs(fd: number, buf: Uint8Array, off: number, end: number): number {
  const start = off;
  const gen = gens[fd]!;
  const outer = processingFd;
  processingFd = fd;
  try {
    while (off < end) {
      const r = http.parse(buf, off, end);
      if (r === http.INCOMPLETE) {
        if (http.needContinue && (flags[fd]! & F_CONTINUE_SENT) === 0) {
          flags[fd]! |= F_CONTINUE_SENT;
          body.u8.set(CONTINUE, 0);
          sendResponse(fd, body, 0, CONTINUE.length, false);
        }
        break;
      }
      if (r < 0) {
        respondError(fd, r === http.TOO_LARGE ? 413 : 400, r === http.TOO_LARGE ? "payload too large" : "bad request", false);
        return -1;
      }
      flags[fd]! &= ~F_CONTINUE_SENT;
      dispatch(fd, gen, buf);
      off += r;
      if (gens[fd] !== gen) return -1;
      if ((flags[fd]! & F_BUSY) !== 0 || outPend.has(fd)) break;
    }
    return off - start;
  } finally {
    processingFd = outer;
  }
}

function pathIs(buf: Uint8Array, s: number, e: number, p: Uint8Array): boolean {
  if (e - s !== p.length) return false;
  for (let i = 0; i < p.length; i++) if (buf[s + i] !== p[i]) return false;
  return true;
}

// A post id: 1 to 15 ASCII digits, positive. Returns 0 when invalid.
function parseId(buf: Uint8Array, s: number, e: number): number {
  if (e <= s || e - s > 15) return 0;
  let id = 0;
  for (let i = s; i < e; i++) {
    const d = buf[i]! - 48;
    if (d < 0 || d > 9) return 0;
    id = id * 10 + d;
  }
  return id;
}

function rent(stmt: number, fd: number, gen: number): Entry {
  const e = entries.pop() ?? new Entry();
  e.stmt = stmt;
  e.fd = fd;
  e.gen = gen;
  e.keepAlive = http.keepAlive;
  e.gzip = http.acceptsGzip;
  flags[fd]! |= F_BUSY; // its later pipelined requests wait for this one
  return e;
}

function dispatch(fd: number, gen: number, buf: Uint8Array): void {
  const ps = http.pathStart, pe = http.pathEnd;
  const m = http.method;
  if (m === http.M_GET) {
    if (pathIs(buf, ps, pe, P_FEED)) { submit(rent(S_FEED, fd, gen)); return; }
    if (pe - ps > 7 && pathIs(buf, ps, ps + 7, POSTS_SLASH)) {
      let slash = false;
      for (let i = ps + 7; i < pe; i++) if (buf[i] === 47) { slash = true; break; }
      if (!slash) {
        const id = parseId(buf, ps + 7, pe);
        if (id === 0) { respondError(fd, 400, "invalid post id", http.keepAlive); return; }
        const e = rent(S_POST, fd, gen);
        e.a = id;
        submit(e);
        return;
      }
    }
    if (pathIs(buf, ps, pe, P_HEALTH)) { submit(rent(S_HEALTH, fd, gen)); return; }
  } else if (m === http.M_POST) {
    if (pathIs(buf, ps, pe, P_POSTS)) { createPost(fd, gen, buf); return; }
    if (pe - ps > 12 && pathIs(buf, ps, ps + 7, POSTS_SLASH) && pathIs(buf, pe - 5, pe, P_LIKE)) {
      let slash = false;
      for (let i = ps + 7; i < pe - 5; i++) if (buf[i] === 47) { slash = true; break; }
      if (!slash) {
        const auth = jwt.verify(buf, http.authStart, http.authEnd);
        if (auth !== jwt.AUTH_OK) { respondError(fd, 401, jwt.authMessage(auth), http.keepAlive); return; }
        const id = parseId(buf, ps + 7, pe - 5);
        if (id === 0) { respondError(fd, 400, "invalid post id", http.keepAlive); return; }
        const e = rent(S_LIKE, fd, gen);
        e.a = jwt.authUserId;
        e.b = id;
        submit(e);
        return;
      }
    }
  }
  respondError(fd, 404, "not found", http.keepAlive);
}
const POSTS_SLASH = bytes("/posts/");

const textDec = new TextDecoder();
const textEnc = new TextEncoder();

function createPost(fd: number, gen: number, buf: Uint8Array): void {
  const auth = jwt.verify(buf, http.authStart, http.authEnd);
  if (auth !== jwt.AUTH_OK) { respondError(fd, 401, jwt.authMessage(auth), http.keepAlive); return; }
  let parsed: unknown;
  try {
    parsed = JSON.parse(textDec.decode(buf.subarray(http.bodyStart, http.bodyEnd)));
  } catch {
    respondError(fd, 400, "malformed JSON body", http.keepAlive);
    return;
  }
  let text = "";
  if (parsed !== null && typeof parsed === "object" && !Array.isArray(parsed)) {
    const v = (parsed as { body?: unknown }).body;
    if (typeof v === "string") text = v.trim();
  }
  if (text.length === 0) { respondError(fd, 400, "body is required", http.keepAlive); return; }
  let codePoints = text.length;
  if (codePoints > 500) {
    for (let i = 0; i < text.length; i++) {
      const c = text.charCodeAt(i);
      if (c >= 0xdc00 && c <= 0xdfff) codePoints--;
    }
    if (codePoints > 500) { respondError(fd, 400, "body must be at most 500 characters", http.keepAlive); return; }
  }
  const e = rent(S_INSERT, fd, gen);
  e.a = jwt.authUserId;
  e.text = textEnc.encode(text);
  e.user = jwt.authUsername;
  submit(e);
}

// ---- requests waiting on Postgres ----

function submit(e: Entry): void {
  if (e.isWrite()) passWrites.push(e);
  else passReads.push(e);
}

function noConnection(e: Entry): void {
  if (e.stmt === S_HEALTH) {
    unavailable(e);
    deliver(e);
    return;
  }
  if (e.waitingSince === 0) e.waitingSince = Date.now();
  waiting.push(e);
}

// Splits the pass's requests of one kind into contiguous runs of near-equal length, one per connection, given to
// the least loaded live connections of slots [from, to).
function deal(list: Entry[], from: number, to: number): void {
  const count = list.length;
  if (count === 0) return;
  let live = 0;
  for (let i = from; i < to; i++) {
    const c = pg[i];
    if (c != null) byLoad[live++] = c;
  }
  if (live === 0) {
    const stuck = list.slice();
    list.length = 0;
    for (const e of stuck) noConnection(e);
    return;
  }
  const runs = Math.min(live, Math.ceil(count / DEAL_SIZE));
  for (let r = 0; r < runs; r++) {
    let min = r;
    for (let j = r + 1; j < live; j++) if (byLoad[j]!.load() < byLoad[min]!.load()) min = j;
    const t = byLoad[r]!; byLoad[r] = byLoad[min]!; byLoad[min] = t;
    const conn = byLoad[r]!;
    const s = Math.floor((count * r) / runs), e = Math.floor((count * (r + 1)) / runs);
    for (let i = s; i < e; i++) conn.encode(list[i]!);
    if (!conn.dirty) { conn.dirty = true; dirty.push(conn); }
  }
  byLoad.length = 0;
  list.length = 0;
}

function flushPg(): void {
  for (let round = 0; round < 4 && passReads.length + passWrites.length > 0; round++) {
    deal(passReads, 0, readSlots);
    deal(passWrites, readSlots, pg.length);
    for (let i = 0; i < dirty.length; i++) {
      const c = dirty[i]!;
      c.dirty = false;
      if (pg[c.slot] !== c) continue;
      if (c.unsent.length > 0) syncs++;
      if (!c.flush()) pgFail(c, c.failReason);
    }
    dirty.length = 0;
  }
}

function expireWaiting(): void {
  const now = Date.now();
  for (let k = waiting.length; k > 0; k--) {
    const e = waiting.shift()!;
    if (now - e.waitingSince < MAX_WAIT_MS) { waiting.push(e); continue; }
    unavailable(e);
    deliver(e);
  }
}

function housekeeping(now: number): void {
  lastHousekeeping = now;
  if (C.getppid() !== parentPid) process.exit(0); // the primary is gone
  if (exitAt > 0 && now >= exitAt) process.exit(0); // EXIT_AFTER_S: for profiling runs (bun --cpu-prof writes on exit)
  for (let slot = 0; slot < pg.length; slot++) {
    const c = pg[slot];
    if (c == null) {
      if (now >= reconnectAt[slot]!) tryConnect(slot);
    } else if (c.stalled(now, PG_TIMEOUT_MS)) {
      pgFail(c, `silent for ${PG_TIMEOUT_MS} ms with ${c.inflightCount()} requests in flight`);
    }
  }
  if (now - lastStats < 10_000) return;
  const secs = lastStats === 0 ? 10 : (now - lastStats) / 1000;
  lastStats = now;
  let live = 0, queued = 0;
  for (const c of pg) if (c != null) { live++; queued += c.load(); }
  const dA = answered - prevAnswered, dS = syncs - prevSyncs;
  console.log(`stats worker=${workerId} open=${open} accepted/s=${((accepted - prevAccepted) / secs).toFixed(0)} ` +
    `answers/s=${(dA / secs).toFixed(0)} passes/s=${((passes - prevPasses) / secs).toFixed(0)} ` +
    `pg_batch=${dS > 0 ? (dA / dS).toFixed(1) : "0"} pg_live=${live}/${pg.length} pg_queued=${queued} waiting=${waiting.length}`);
  prevAnswered = answered; prevSyncs = syncs; prevPasses = passes; prevAccepted = accepted;
}

function onPg(c: PgConn, evs: number): void {
  if ((evs & EPOLLERR) !== 0) { pgFail(c, "socket error"); return; }
  let ok = true;
  if ((evs & EPOLLOUT) !== 0) ok = c.sendOut();
  if (ok && (evs & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) !== 0) ok = c.onReadable();
  if (!ok) pgFail(c, c.failReason);
}

function pgFail(c: PgConn, why: string): void {
  if (pg[c.slot] !== c) return;
  pg[c.slot] = null;
  C.close(c.fd);
  setLastError("connection to Postgres lost");
  console.error(`worker ${workerId}: Postgres connection ${c.slot} lost (${why}), reconnecting`);
  c.failAll();
  reconnectAt[c.slot] = 0; // at the next housekeeping, then every second while it fails
}

// ---- responses ----

// A read's result rows (DataRow messages in u8[start, end)) straight into the response.
function readDone(e: Entry, u8: Uint8Array, dv: DataView, start: number, end: number): void {
  w.pos = HEAD_ROOM;
  const stmt = e.stmt;
  let status = 200;
  if (stmt === S_FEED) {
    w.raw(J_POSTS_OPEN);
    let p = start, first = true;
    while (p < end) {
      const len = dv.getInt32(p + 1);
      if (u8[p] === 68) {
        if (!first) w.byte(44);
        first = false;
        writePost(u8, dv, p + 7);
      }
      p += 1 + len;
    }
    w.raw(J_POSTS_CLOSE);
  } else if (stmt === S_POST) {
    let p = start, found = false;
    while (p < end) {
      const len = dv.getInt32(p + 1);
      if (u8[p] === 68) {
        w.raw(J_POST_OPEN);
        writePost(u8, dv, p + 7);
        w.byte(125);
        found = true;
        break;
      }
      p += 1 + len;
    }
    if (!found) {
      status = 404;
      w.raw(J_ERROR);
      w.jsStr("post not found");
      w.byte(125);
    }
  } else {
    w.raw(J_HEALTH);
    w.uint(Math.floor((Date.now() - startedAt) / 1000));
    w.byte(125);
  }
  finish(e, status);
}

// Columns from p (the first column's length): id int8, body text, created_at timestamptz, username text, like_count int4.
function writePost(u8: Uint8Array, dv: DataView, p: number): void {
  w.raw(J_ID);
  w.uint(dv.getInt32(p + 4) * 4294967296 + dv.getUint32(p + 8));
  p += 12;
  let len = dv.getInt32(p);
  w.raw(J_BODY);
  w.str(u8, p + 4, p + 4 + len);
  p += 4 + len;
  w.raw(J_CREATED);
  w.timestamp(dv.getInt32(p + 4) * 4294967296 + dv.getUint32(p + 8));
  p += 12;
  len = dv.getInt32(p);
  w.raw(J_AUTHOR);
  w.str(u8, p + 4, p + 4 + len);
  p += 4 + len;
  w.raw(J_LIKES);
  w.uint(dv.getInt32(p + 4));
  w.byte(125);
}

// A finished write, or any request that failed: build its response from the entry, send it, recycle it.
function deliver(e: Entry): void {
  w.pos = HEAD_ROOM;
  let status: number;
  if (e.errMsg !== null) {
    status = e.status;
    if (status === 503) {
      w.raw(J_DEGRADED);
      w.jsStr(e.errMsg || "unreachable");
    } else {
      w.raw(J_ERROR);
      w.jsStr(e.errMsg);
    }
    w.byte(125);
  } else if (e.stmt === S_LIKE) {
    status = e.inserted ? 201 : 200;
    w.raw(J_LIKED);
    w.raw(e.inserted ? J_FALSE : J_TRUE);
    w.raw(J_POST_ID);
    w.uint(e.b);
    w.byte(125);
  } else if (e.stmt === S_INSERT) {
    if (e.rows === 0) {
      status = 500;
      w.raw(J_ERROR);
      w.jsStr("internal server error");
      w.byte(125);
    } else {
      status = 201;
      w.raw(J_POST_OPEN);
      w.raw(J_ID);
      w.uint(e.resId);
      w.raw(J_BODY);
      w.str(e.text!, 0, e.text!.length);
      w.raw(J_CREATED);
      w.timestamp(e.resCreated);
      w.raw(J_AUTHOR);
      w.str(e.user!, 0, e.user!.length);
      w.raw(J_LIKES0_CLOSE2);
    }
  } else {
    status = 500;
    w.raw(J_ERROR);
    w.jsStr("internal server error");
    w.byte(125);
  }
  finish(e, status);
}

// Sends the body in `w` for entry e and recycles e; then resumes the client's pipelined requests, if any.
function finish(e: Entry, status: number): void {
  const fd = e.fd, gen = e.gen, keepAlive = e.keepAlive, gzip = e.gzip;
  e.text = null;
  e.user = null;
  e.waitingSince = 0;
  e.resetResult();
  entries.push(e);
  if (gens[fd] !== gen || (flags[fd]! & F_DEAD) !== 0) return;
  answered++;
  flags[fd]! &= ~F_BUSY;
  respond(fd, status, gzip, keepAlive);
  if (gens[fd] !== gen || outPend.has(fd) || !inPend.has(fd) || fd === processingFd) return;
  try {
    processBuffered(fd);
  } catch (err) {
    console.error(err);
    if (gens[fd] === gen) close(fd);
  }
}

function respondError(fd: number, status: number, message: string, keepAlive: boolean): void {
  w.pos = HEAD_ROOM;
  w.raw(J_ERROR);
  w.jsStr(message);
  w.byte(125);
  respond(fd, status, false, keepAlive);
}

// The body is in body.u8[HEAD_ROOM, w.pos): gzip it when asked and worth it, write the head in front, send.
function respond(fd: number, status: number, acceptsGzip: boolean, keepAlive: boolean): void {
  let src = body;
  let len = w.pos - HEAD_ROOM;
  let gzipped = false;
  if (acceptsGzip && len >= GZIP_MIN) {
    const zn = Z.libdeflate_gzip_compress(compressor, body.addr + HEAD_ROOM, len, gz.addr + HEAD_ROOM, gz.u8.length - HEAD_ROOM);
    if (zn > 0) {
      src = gz;
      len = zn;
      gzipped = true;
    }
  }
  const b = src.u8;
  const statusLine = STATUS_LINES[status] ?? STATUS_LINES[500]!;
  const d = date();
  let digits = 1;
  for (let v = len; v >= 10; v = Math.floor(v / 10)) digits++;
  const size = statusLine.length + d.length + (gzipped ? H_GZIP.length : 0) + (keepAlive ? 0 : H_CLOSE.length) +
    H_LENGTH.length + digits + 4;
  const start = HEAD_ROOM - size;
  let p = start;
  p = put(b, p, statusLine);
  p = put(b, p, d);
  if (gzipped) { p = put(b, p, H_GZIP); }
  if (!keepAlive) { p = put(b, p, H_CLOSE); }
  p = put(b, p, H_LENGTH);
  for (let v = len, i = p + digits - 1; i >= p; i--) {
    const q = Math.floor(v / 10);
    b[i] = 48 + (v - q * 10);
    v = q;
  }
  p += digits;
  b[p] = 13; b[p + 1] = 10; b[p + 2] = 13; b[p + 3] = 10;
  sendResponse(fd, src, start, HEAD_ROOM + len - start, !keepAlive);
}

// Sends m.u8[off, off+len). What the socket doesn't take now is kept and sent when it becomes writable.
function sendResponse(fd: number, m: { u8: Uint8Array; addr: number }, off: number, len: number, closeAfter: boolean): void {
  const n = C.send(fd, m.addr + off, len, MSG_NOSIGNAL);
  if (n === len) {
    if (closeAfter) close(fd);
    return;
  }
  if (n < 0 && errno() !== EAGAIN) {
    close(fd);
    return;
  }
  const sent = n > 0 ? n : 0;
  outPend.set(fd, m.u8.slice(off + sent, off + len));
  if (closeAfter) flags[fd]! |= F_CLOSE_AFTER;
  flags[fd]! |= F_WANT_OUT;
  epollCtl(EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP, K_CLIENT, fd);
}

const outTmp = alloc(256 * 1024);
function flushOut(fd: number): void {
  const pending = outPend.get(fd);
  if (pending === undefined) return;
  const chunk = Math.min(pending.length, outTmp.u8.length);
  outTmp.u8.set(pending.subarray(0, chunk), 0);
  const n = C.send(fd, outTmp.addr, chunk, MSG_NOSIGNAL);
  if (n < 0) {
    if (errno() !== EAGAIN) close(fd);
    return;
  }
  if (n < pending.length) {
    outPend.set(fd, pending.slice(n));
    return;
  }
  outPend.delete(fd);
  if ((flags[fd]! & F_CLOSE_AFTER) !== 0) { close(fd); return; }
  flags[fd]! &= ~F_WANT_OUT;
  epollCtl(EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLRDHUP, K_CLIENT, fd);
  if (inPend.has(fd) && (flags[fd]! & F_BUSY) === 0) processBuffered(fd);
}

function close(fd: number): void {
  if ((flags[fd]! & F_DEAD) !== 0) return;
  gens[fd] = (gens[fd]! + 1) >>> 0;
  flags[fd] = F_DEAD;
  inPend.delete(fd);
  outPend.delete(fd);
  closed++;
  open--;
  toClose.push(fd); // closed at the end of the pass, so the fd number isn't reused while this pass holds events
}

export { onError, lastError };

// Copies a short constant into b at p (a loop beats TypedArray.set's native call at this size); returns the end.
function put(b: Uint8Array, p: number, src: Uint8Array): number {
  const n = src.length;
  for (let i = 0; i < n; i++) b[p + i] = src[i]!;
  return p + n;
}
