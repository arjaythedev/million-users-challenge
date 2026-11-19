// A Postgres wire-protocol (v3) client that lives inside the worker's epoll loop.
//
// `connect` does the blocking part: TCP connect, startup, authentication (SCRAM-SHA-256, MD5 or cleartext), and a
// Parse of each SPEC.md statement. The socket then turns non-blocking and joins the loop: requests become Bind +
// Execute (binary parameters and results) appended to the connection's output buffer, and at the end of every loop
// pass one Sync closes the batch and one send() writes it.
//
// Postgres runs a batch as one implicit transaction. Reads are answered as soon as their CommandComplete arrives.
// Writes go to their own connections, and their answers wait for the batch's ReadyForQuery, i.e. its commit (group
// commit: one commit per batch, every response after it). After an error Postgres skips the rest of the batch: the
// failed request gets its error, and the others (skipped, or rolled back) are submitted again.
import { createHash, createHmac, pbkdf2Sync, randomBytes, timingSafeEqual } from "node:crypto";
import {
  AF_INET, alloc, C, EAGAIN, EPOLLIN, EPOLLOUT, EPOLLRDHUP, errno, F_GETFL, F_SETFL, IPPROTO_TCP, type Mem,
  MSG_NOSIGNAL, O_NONBLOCK, SO_RCVTIMEO, SO_SNDTIMEO, SOCK_CLOEXEC, SOCK_STREAM, sockaddrIn, SOL_SOCKET, TCP_NODELAY,
} from "./ffi";

// Statement kinds; each is prepared under a one-letter name.
export const S_FEED = 0;
export const S_POST = 1;
export const S_INSERT = 2;
export const S_LIKE = 3;
export const S_HEALTH = 4;

const POST_SELECT = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id`;

// name, SQL (SPEC.md, verbatim), parameter type OIDs (20 = int8, 25 = text)
const STATEMENTS: [string, string, number[]][] = [
  ["f", POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", []],
  ["p", POST_SELECT + " WHERE p.id = $1", [20]],
  ["i", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", [20, 25]],
  ["l", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", [20, 20]],
  ["h", "SELECT 1", []],
];
const NAMES = STATEMENTS.map(([n]) => n.charCodeAt(0));

// One request waiting on Postgres. Pooled by the loop.
export class Entry {
  stmt = 0;
  fd = 0;
  gen = 0;
  keepAlive = true;
  gzip = false;
  a = 0; // int8 parameters
  b = 0;
  text: Uint8Array | null = null; // POST /posts body, trimmed, UTF-8
  user: Uint8Array | null = null; // token username, UTF-8
  resId = 0; // INSERT ... RETURNING
  resCreated = 0;
  inserted = false; // like: a row was inserted
  rows = 0;
  status = 0; // set when the answer is an error
  errMsg: string | null = null;
  endsBatch = false; // the last request before a Sync
  waitingSince = 0; // when it first found no live connection

  isWrite(): boolean {
    return this.stmt === S_INSERT || this.stmt === S_LIKE;
  }

  resetResult(): void {
    this.rows = 0;
    this.status = 0;
    this.errMsg = null;
    this.endsBatch = false;
    this.inserted = false;
  }
}

export interface PgHost {
  // A read's rows: DataRow messages in u8[start, end) (each: type, length, body). Builds and sends the response.
  readDone(e: Entry, u8: Uint8Array, dv: DataView, start: number, end: number): void;
  // Sends the response of a finished write or a failed request, and recycles the entry.
  deliver(e: Entry): void;
  requeue(e: Entry): void;
  setPgEvents(c: PgConn, events: number): void;
}

export interface PgConfig {
  ip: string;
  port: number;
  user: string;
  password: string;
  database: string;
}

export let lastError = "connecting";
export function setLastError(s: string): void {
  lastError = s;
}

// ---- the blocking handshake ----

const hs = alloc(64 * 1024);
const hsDv = new DataView(hs.u8.buffer);
const enc = new TextEncoder();
const dec = new TextDecoder();

function sendAll(fd: number, msg: Uint8Array): void {
  hs.u8.set(msg, 0);
  let off = 0;
  while (off < msg.length) {
    const n = C.send(fd, hs.addr + off, msg.length - off, MSG_NOSIGNAL);
    if (n <= 0) throw new Error(`send to Postgres failed: errno ${errno()}`);
    off += n;
  }
}

function recvExact(fd: number, n: number): void {
  if (n > hs.u8.length) throw new Error("message from Postgres too large");
  let got = 0;
  while (got < n) {
    const r = C.recv(fd, hs.addr + got, n - got, 0);
    if (r === 0) throw new Error("Postgres closed the connection");
    if (r < 0) throw new Error(`recv from Postgres failed: errno ${errno()}`);
    got += r;
  }
}

function readMsg(fd: number): [number, Uint8Array] {
  recvExact(fd, 5);
  const type = hs.u8[0]!;
  const len = hsDv.getInt32(1);
  if (len < 4 || len > 1 << 20) throw new Error("bad message from Postgres");
  recvExact(fd, len - 4);
  return [type, hs.u8.slice(0, len - 4)];
}

// A message: type byte (0: none, the startup message), length, then parts.
function message(type: number, ...parts: (Uint8Array | number[])[]): Uint8Array {
  let len = 4;
  for (const p of parts) len += p.length;
  const out = new Uint8Array(len + (type ? 1 : 0));
  let o = 0;
  if (type) out[o++] = type;
  new DataView(out.buffer).setInt32(o, len);
  o += 4;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}
const cstr = (s: string): Uint8Array => {
  const b = enc.encode(s);
  const out = new Uint8Array(b.length + 1);
  out.set(b);
  return out;
};
const i32be = (v: number): number[] => [(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255];
const i16be = (v: number): number[] => [(v >>> 8) & 255, v & 255];

export function errorFields(body: Uint8Array): [string, string] {
  let state = "", msg = "";
  let i = 0;
  while (i < body.length && body[i] !== 0) {
    const field = body[i]!;
    let j = i + 1;
    while (j < body.length && body[j] !== 0) j++;
    const v = dec.decode(body.subarray(i + 1, j));
    if (field === 67) state = v;
    else if (field === 77) msg = v;
    i = j + 1;
  }
  return [state, msg];
}

const tv = alloc(16);
const one = alloc(4);
const sa = alloc(16);

function setTimeouts(fd: number, secs: number): void {
  tv.u8.fill(0, 0, 16);
  new DataView(tv.u8.buffer).setUint32(0, secs, true); // tv_sec (low half of a little-endian 64-bit)
  C.setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, tv.addr, 16);
  C.setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, tv.addr, 16);
}

// Opens one connection, logs in, prepares the statements; returns the non-blocking fd.
export function connect(cfg: PgConfig): number {
  const fd = C.socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw new Error(`socket: errno ${errno()}`);
  try {
    setTimeouts(fd, 5);
    sockaddrIn(sa, cfg.ip, cfg.port);
    if (C.connect(fd, sa.addr, 16) !== 0) throw new Error(`connect to ${cfg.ip}:${cfg.port} failed: errno ${errno()}`);
    new DataView(one.u8.buffer).setInt32(0, 1, true);
    C.setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, one.addr, 4);

    sendAll(fd, message(0, i32be(3 << 16), cstr("user"), cstr(cfg.user), cstr("database"), cstr(cfg.database),
      cstr("application_name"), cstr("ts-bunffi"), [0]));
    authenticate(fd, cfg);

    const parts: Uint8Array[] = [];
    for (const [name, sql, oids] of STATEMENTS) {
      const o: number[] = [...i16be(oids.length)];
      for (const oid of oids) o.push(...i32be(oid));
      parts.push(message(80, cstr(name), cstr(sql), o));
    }
    parts.push(message(83));
    let total = 0;
    for (const p of parts) total += p.length;
    const all = new Uint8Array(total);
    let off = 0;
    for (const p of parts) { all.set(p, off); off += p.length; }
    sendAll(fd, all);
    for (;;) {
      const [type, body] = readMsg(fd);
      if (type === 69) { const [s, m] = errorFields(body); throw new Error(`prepare: ${m} (SQLSTATE ${s})`); }
      if (type === 90) break;
    }
    setTimeouts(fd, 0);
    C.fcntl(fd, F_SETFL, C.fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
  } catch (err) {
    C.close(fd);
    throw err;
  }
}

function authenticate(fd: number, cfg: PgConfig): void {
  let nonce = "", firstBare = "", serverSig: Buffer | null = null;
  for (;;) {
    const [type, body] = readMsg(fd);
    if (type === 69) { const [s, m] = errorFields(body); throw new Error(`${m} (SQLSTATE ${s})`); }
    if (type === 90) return; // ReadyForQuery: logged in
    if (type !== 82) continue; // parameter values, the cancel key, notices
    const dv = new DataView(body.buffer, body.byteOffset, body.byteLength);
    const method = dv.getInt32(0);
    const text = dec.decode(body.subarray(4));
    if (method === 0) continue;
    if (method === 3) {
      sendAll(fd, message(112, cstr(cfg.password)));
    } else if (method === 5) {
      const inner = createHash("md5").update(cfg.password + cfg.user).digest("hex");
      const outer = createHash("md5").update(Buffer.concat([Buffer.from(inner), Buffer.from(body.subarray(4, 8))])).digest("hex");
      sendAll(fd, message(112, cstr("md5" + outer)));
    } else if (method === 10) {
      if (!text.split("\0").includes("SCRAM-SHA-256")) throw new Error("server offers no SCRAM-SHA-256");
      nonce = randomBytes(18).toString("base64");
      firstBare = "n=,r=" + nonce;
      const first = enc.encode("n,," + firstBare);
      sendAll(fd, message(112, cstr("SCRAM-SHA-256"), i32be(first.length), first));
    } else if (method === 11) {
      const attrs = new Map(text.split(",").map((a) => [a.slice(0, 1), a.slice(2)] as [string, string]));
      const r = attrs.get("r") ?? "", s = attrs.get("s") ?? "", i = Number(attrs.get("i") ?? 0);
      if (!r.startsWith(nonce) || !s || !(i > 0)) throw new Error("bad SCRAM server-first message");
      const salted = pbkdf2Sync(cfg.password, Buffer.from(s, "base64"), i, 32, "sha256");
      const withoutProof = "c=biws,r=" + r;
      const authMessage = firstBare + "," + text + "," + withoutProof;
      const clientKey = createHmac("sha256", salted).update("Client Key").digest();
      const storedKey = createHash("sha256").update(clientKey).digest();
      const proof = createHmac("sha256", storedKey).update(authMessage).digest();
      for (let k = 0; k < proof.length; k++) proof[k]! ^= clientKey[k]!;
      const serverKey = createHmac("sha256", salted).update("Server Key").digest();
      serverSig = createHmac("sha256", serverKey).update(authMessage).digest();
      sendAll(fd, message(112, enc.encode(withoutProof + ",p=" + proof.toString("base64"))));
    } else if (method === 12) {
      const v = text.split(",")[0] ?? "";
      const got = Buffer.from(v.slice(2), "base64");
      if (!v.startsWith("v=") || !serverSig || got.length !== serverSig.length || !timingSafeEqual(got, serverSig))
        throw new Error("SCRAM server signature mismatch");
    } else {
      throw new Error(`unsupported authentication method ${method}`);
    }
  }
}

// ---- one connection inside the loop ----

export class PgConn {
  fd: number;
  slot: number;
  forWrites: boolean;
  host: PgHost;
  out: Mem = alloc(32 * 1024);
  outHead = 0;
  outTail = 0;
  inp: Mem = alloc(256 * 1024);
  dv: DataView;
  inHead = 0;
  inTail = 0;
  unsent: Entry[] = [];
  inflight: Entry[] = [];
  inflightHead = 0;
  committing: Entry[] = [];
  batchFailed = false;
  batchEndSeen = false;
  dirty = false;
  wantWrite = false;
  lastProgress = 0;
  failReason = "";

  constructor(host: PgHost, slot: number, fd: number, forWrites: boolean) {
    this.host = host;
    this.slot = slot;
    this.fd = fd;
    this.forWrites = forWrites;
    this.dv = new DataView(this.inp.u8.buffer);
  }

  load(): number {
    return this.unsent.length + this.inflight.length - this.inflightHead;
  }

  inflightCount(): number {
    return this.inflight.length - this.inflightHead;
  }

  stalled(now: number, timeoutMs: number): boolean {
    return (this.inflightCount() > 0 || this.committing.length > 0) && now - this.lastProgress > timeoutMs;
  }

  private reserveOut(n: number): void {
    if (this.out.u8.length - this.outTail >= n) return;
    const live = this.outTail - this.outHead;
    if (live + n <= this.out.u8.length) {
      this.out.u8.copyWithin(0, this.outHead, this.outTail);
    } else {
      const bigger = alloc(Math.max(this.out.u8.length * 2, live + n));
      bigger.u8.set(this.out.u8.subarray(this.outHead, this.outTail), 0);
      this.out = bigger;
    }
    this.outHead = 0;
    this.outTail = live;
  }

  // Bind (unnamed portal, prepared statement, binary parameters and results) + Execute (all rows).
  encode(e: Entry): void {
    const textLen = e.text === null ? 0 : e.text.length;
    this.reserveOut(80 + textLen);
    const b = this.out.u8;
    let p = this.outTail;
    b[p++] = 66; // 'B'
    const lenAt = p;
    p += 4;
    b[p++] = 0; // portal ""
    b[p++] = NAMES[e.stmt]!;
    b[p++] = 0;
    const stmt = e.stmt;
    if (stmt === S_POST) {
      b[p++] = 0; b[p++] = 1; b[p++] = 0; b[p++] = 1; // one format for all: binary
      b[p++] = 0; b[p++] = 1; // one parameter
      p = int8(b, p, e.a);
    } else if (stmt === S_LIKE) {
      b[p++] = 0; b[p++] = 1; b[p++] = 0; b[p++] = 1;
      b[p++] = 0; b[p++] = 2;
      p = int8(b, p, e.a);
      p = int8(b, p, e.b);
    } else if (stmt === S_INSERT) {
      b[p++] = 0; b[p++] = 1; b[p++] = 0; b[p++] = 1;
      b[p++] = 0; b[p++] = 2;
      p = int8(b, p, e.a);
      p = be32(b, p, textLen);
      b.set(e.text!, p);
      p += textLen;
    } else {
      b[p++] = 0; b[p++] = 0; // no formats
      b[p++] = 0; b[p++] = 0; // no parameters
    }
    b[p++] = 0; b[p++] = 1; b[p++] = 0; b[p++] = 1; // results: binary
    be32(b, lenAt, p - lenAt);
    b[p++] = 69; // 'E'
    p = be32(b, p, 9);
    b[p++] = 0;
    p = be32(b, p, 0);
    this.outTail = p;
    this.unsent.push(e);
  }

  // Closes the batch with a Sync and writes everything out. False: the connection is broken.
  flush(): boolean {
    const n = this.unsent.length;
    if (n > 0) {
      this.reserveOut(5);
      const b = this.out.u8;
      let p = this.outTail;
      b[p++] = 83; // 'S'
      p = be32(b, p, 4);
      this.outTail = p;
      this.unsent[n - 1]!.endsBatch = true;
      if (this.inflightCount() === 0) this.lastProgress = Date.now();
      for (let i = 0; i < n; i++) this.inflight.push(this.unsent[i]!);
      this.unsent.length = 0;
    }
    return this.sendOut();
  }

  sendOut(): boolean {
    while (this.outTail > this.outHead) {
      const n = C.send(this.fd, this.out.addr + this.outHead, this.outTail - this.outHead, MSG_NOSIGNAL);
      if (n > 0) {
        this.outHead += n;
        continue;
      }
      const err = n < 0 ? errno() : 0;
      if (err === EAGAIN) {
        if (!this.wantWrite) {
          this.wantWrite = true;
          this.host.setPgEvents(this, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
        }
        return true;
      }
      return this.fail(`send: errno ${err}`);
    }
    this.outHead = this.outTail = 0;
    if (this.wantWrite) {
      this.wantWrite = false;
      this.host.setPgEvents(this, EPOLLIN | EPOLLRDHUP);
    }
    return true;
  }

  // Makes room for at least n more bytes at inTail (compacting, or growing for a big message).
  private reserveIn(n: number): void {
    if (this.inp.u8.length - this.inTail >= n) return;
    const live = this.inTail - this.inHead;
    if (live + n <= this.inp.u8.length) {
      this.inp.u8.copyWithin(0, this.inHead, this.inTail);
    } else {
      const bigger = alloc(Math.max(this.inp.u8.length * 2, live + n));
      bigger.u8.set(this.inp.u8.subarray(this.inHead, this.inTail), 0);
      this.inp = bigger;
      this.dv = new DataView(bigger.u8.buffer);
    }
    this.inHead = 0;
    this.inTail = live;
  }

  // Reads what Postgres sent and handles every complete message. False: the connection is broken.
  onReadable(): boolean {
    for (;;) {
      this.reserveIn(32 * 1024);
      const room = this.inp.u8.length - this.inTail;
      const n = C.recv(this.fd, this.inp.addr + this.inTail, room, 0);
      if (n === 0) return this.fail("closed by Postgres");
      if (n < 0) {
        const err = errno();
        return err === EAGAIN || this.fail(`recv: errno ${err}`);
      }
      this.inTail += n;
      this.lastProgress = Date.now();
      if (!this.handleBuffered()) return false;
      if (n < room) return true;
    }
  }

  private peek(): Entry | null {
    return this.inflightHead < this.inflight.length ? this.inflight[this.inflightHead]! : null;
  }

  private dequeue(): Entry {
    const e = this.inflight[this.inflightHead]!;
    this.inflight[this.inflightHead++] = null as unknown as Entry;
    if (this.inflightHead === this.inflight.length) {
      this.inflight.length = 0;
      this.inflightHead = 0;
    }
    return e;
  }

  private handleBuffered(): boolean {
    const u8 = this.inp.u8;
    const dv = this.dv;
    while (this.inTail - this.inHead >= 5) {
      const h = this.inHead;
      const type = u8[h]!;
      const len = dv.getInt32(h + 1);
      if (len < 4 || len > 64 << 20) return this.fail(`bad message length ${len} (type ${type})`);
      if (this.inTail - h <= len) {
        if (len + 1 > u8.length) this.reserveIn(len + 1); // a message bigger than the buffer: grow it
        return true;
      }
      if (type === 68 || type === 67) {
        // DataRow / CommandComplete: the head request's result
        const e = this.peek();
        if (e === null) return this.fail("result with nothing in flight");
        if (!e.isWrite()) {
          // a read needs its whole result (rows through CommandComplete) buffered: then it is answered at once
          let p = h;
          for (;;) {
            if (this.inTail - p < 5) return true;
            const t = u8[p]!;
            const l = dv.getInt32(p + 1);
            if (this.inTail - p <= l) {
              if (p - h + l + 1 > u8.length) this.reserveIn(p - h + l + 1);
              return true;
            }
            if (t === 67) {
              this.dequeue();
              if (e.endsBatch) this.batchEndSeen = true;
              this.inHead = p + 1 + l;
              this.host.readDone(e, u8, dv, h, p);
              break;
            }
            p += 1 + l;
          }
          continue;
        }
        if (type === 68) {
          // INSERT ... RETURNING id, created_at
          e.rows++;
          let p = h + 7; // type, length, column count
          p += 4;
          e.resId = dv.getInt32(p) * 4294967296 + dv.getUint32(p + 4);
          p += 8 + 4;
          e.resCreated = dv.getInt32(p) * 4294967296 + dv.getUint32(p + 4);
        } else {
          // tag "INSERT 0 1" or "INSERT 0 0" (NUL-terminated)
          this.dequeue();
          if (e.endsBatch) this.batchEndSeen = true;
          e.inserted = u8[h + len - 1] !== 48; // the byte before the NUL
          this.committing.push(e);
        }
        this.inHead = h + 1 + len;
        continue;
      }
      this.inHead = h + 1 + len;
      if (type === 69) {
        // ErrorResponse: this request failed; Postgres skips the rest of the batch
        const [state, msg] = errorFields(u8.subarray(h + 5, h + 1 + len));
        if (state.startsWith("08") || state.startsWith("57P")) return this.fail(`Postgres ${state}: ${msg}`);
        if (this.batchEndSeen) {
          // every request of the batch has its answer: the commit itself failed; ReadyForQuery resubmits
          this.batchFailed = true;
          console.error(`Postgres commit: ${msg}`);
          continue;
        }
        const e = this.peek();
        if (e === null) return this.fail(`ErrorResponse with nothing in flight: ${msg}`);
        this.dequeue();
        if (e.endsBatch) this.batchEndSeen = true;
        this.batchFailed = true;
        onError(e, state, msg);
        this.host.deliver(e);
        continue;
      }
      if (type === 90) {
        // ReadyForQuery: the batch is over (committed, or rolled back after an error)
        const committing = this.committing;
        if (!this.batchFailed) {
          for (let i = 0; i < committing.length; i++) this.host.deliver(committing[i]!);
          committing.length = 0;
        } else {
          for (let i = 0; i < committing.length; i++) this.host.requeue(committing[i]!); // rolled back
          committing.length = 0;
          while (!this.batchEndSeen && this.peek() !== null) {
            const e = this.dequeue(); // skipped after the error
            this.batchEndSeen = e.endsBatch;
            this.host.requeue(e);
          }
        }
        this.batchFailed = this.batchEndSeen = false;
      }
      // anything else: BindComplete, ParseComplete, NoData, ParameterStatus, NoticeResponse, ...
    }
    return true;
  }

  fail(why: string): boolean {
    this.failReason = why;
    return false;
  }

  // The connection is gone: writes whose fate is unknown get a 500, everything else is submitted again.
  failAll(): void {
    for (const e of this.committing) { unavailable(e); this.host.deliver(e); }
    this.committing.length = 0;
    while (this.peek() !== null) {
      const e = this.dequeue();
      if (e.isWrite()) { unavailable(e); this.host.deliver(e); }
      else this.host.requeue(e);
    }
    const pending = this.unsent.slice();
    this.unsent.length = 0;
    for (const e of pending) this.host.requeue(e);
  }
}

function be32(b: Uint8Array, p: number, v: number): number {
  b[p] = (v >>> 24) & 255;
  b[p + 1] = (v >>> 16) & 255;
  b[p + 2] = (v >>> 8) & 255;
  b[p + 3] = v & 255;
  return p + 4;
}

// An int8 parameter: length 8, then the value (a non-negative integer below 2^53) big-endian.
function int8(b: Uint8Array, p: number, v: number): number {
  p = be32(b, p, 8);
  const hi = Math.floor(v / 4294967296);
  p = be32(b, p, hi);
  return be32(b, p, v - hi * 4294967296);
}

export function onError(e: Entry, state: string, msg: string): void {
  e.resetResult();
  if (e.stmt === S_LIKE && state === "23503") { // foreign key: no such post
    e.status = 404;
    e.errMsg = "post not found";
    return;
  }
  if (e.stmt === S_HEALTH) {
    e.status = 503;
    e.errMsg = msg;
    return;
  }
  console.error(`Postgres ${state}: ${msg}`);
  e.status = 500;
  e.errMsg = "internal server error";
}

// No live connection for this request, or it was lost with the request in flight.
export function unavailable(e: Entry): void {
  e.resetResult();
  e.status = e.stmt === S_HEALTH ? 503 : 500;
  e.errMsg = e.stmt === S_HEALTH ? lastError : "internal server error";
}
