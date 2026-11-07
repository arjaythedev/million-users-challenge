// A tiny connection pool that owns the wire protocol framing. The type system
// decides which prepared statement to run and the parameter bytes; this file
// frames Parse (once per connection) / Bind / Execute / Sync and splits the reply
// into column byte-strings. It interprets nothing about the application.
import { PgConn, parseErr } from "./pg.mjs";

const enc = (s) => Buffer.from(s, "latin1");
const i16 = (n) => { const b = Buffer.alloc(2); b.writeInt16BE(n | 0); return b; };
const i32 = (n) => { const b = Buffer.alloc(4); b.writeInt32BE(n | 0); return b; };
const cstr = (s) => Buffer.concat([enc(s), Buffer.from([0])]);
function msg(type, parts) {
  const body = Buffer.concat(parts.map((p) => (typeof p === "string" ? enc(p) : p)));
  return Buffer.concat([Buffer.from(type, "latin1"), i32(body.length + 4), body]);
}

// The five statements of SPEC.md, prepared once per connection as s1..s5.
export const STATEMENTS = {
  1: `SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20`,
  2: `SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1`,
  3: `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`,
  4: `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`,
  5: `SELECT 1`,
};

function parseConnStr(url) {
  const u = new URL(url);
  return { host: u.hostname, port: +(u.port || 5432), user: decodeURIComponent(u.username),
    password: decodeURIComponent(u.password), database: u.pathname.slice(1) };
}

class Lane {
  constructor(opts) { this.opts = opts; this.conn = null; this.queue = []; this.cur = null; this.up = false; }
  async start() {
    this.conn = new PgConn(this.opts);
    await this.conn.connect();
    // Parse all statements, then collect ParseComplete via a single Sync.
    const parts = [];
    for (const [id, sql] of Object.entries(STATEMENTS))
      parts.push(msg("P", [cstr("s" + id), cstr(sql), i16(0)]));
    parts.push(msg("S", []));
    this.pc = Buffer.alloc(0);
    await new Promise((resolve, reject) => {
      this.conn.onMessage((t, b) => { if (t === "Z") resolve(); else if (t === "E") reject(new Error(parseErr(b).M)); });
      this.conn.write(Buffer.concat(parts));
    });
    this.conn.onMessage((t, b) => this._msg(t, b));
    this.conn.onFatal(() => { this.up = false; const c = this.cur; this.cur = null; if (c) c.reject(new Error("connection lost")); this._restart(); });
    this.up = true;
    this._pump();
  }
  async _restart() { try { this.conn.end(); } catch {} setTimeout(() => this.start().catch(() => setTimeout(() => this._restart(), 500)), 200); }
  _msg(t, b) {
    const c = this.cur; if (!c) return;
    if (t === "D") { // DataRow -> array of column byte-strings (latin1), null column -> null
      const n = b.readInt16BE(0); let o = 2; const row = [];
      for (let i = 0; i < n; i++) { const len = b.readInt32BE(o); o += 4; if (len < 0) { row.push(null); } else { row.push(b.subarray(o, o + len).toString("latin1")); o += len; } }
      c.rows.push(row);
    } else if (t === "C") { c.cmd = b.subarray(0, b.indexOf(0)).toString("latin1"); }
    else if (t === "E") { c.code = parseErr(b).C || "XX000"; }
    else if (t === "Z") { const cc = this.cur; this.cur = null; cc.resolve({ rows: cc.rows, cmd: cc.cmd ?? null, code: cc.code ?? null }); this._pump(); }
  }
  _pump() {
    if (this.cur || !this.up || this.queue.length === 0) return;
    const job = this.queue.shift(); this.cur = { ...job, rows: [] };
    const { act, params } = job;
    const pb = [i16(0), i16(params.length)]; // 0 param format codes = all text
    for (const p of params) { const buf = enc(p); pb.push(i32(buf.length), buf); }
    pb.push(i16(0)); // 0 result format codes = all text
    const bind = msg("B", [cstr(""), cstr("s" + act), ...pb]);
    const exec = msg("E", [cstr(""), i32(0)]);
    const sync = msg("S", []);
    this.conn.write(Buffer.concat([bind, exec, sync]));
  }
  run(act, params) { return new Promise((resolve, reject) => { this.queue.push({ act, params, resolve, reject }); this._pump(); }); }
}

export class Pool {
  constructor(url, n) { this.opts = parseConnStr(url); this.lanes = []; this.n = n; this.rr = 0; }
  async start() { this.lanes = Array.from({ length: this.n }, () => new Lane(this.opts)); await Promise.all(this.lanes.map((l) => l.start())); }
  run(act, params) { const l = this.lanes[this.rr++ % this.lanes.length]; return l.run(act, params); }
}
