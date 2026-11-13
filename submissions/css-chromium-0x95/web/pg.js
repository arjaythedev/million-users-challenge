// A minimal PostgreSQL v3 wire-protocol client for the page. The bytes travel over WebSockets to a dumb relay in the Rust front
// (src/relay.rs: binary frames <-> TCP, no inspection). This file is a data mover: it frames and parses protocol messages, it
// decides nothing about the application (which statement to run and with which bytes comes from the CSS).
//
//   startup + authentication: trust, cleartext, MD5, SCRAM-SHA-256 (WebCrypto PBKDF2/HMAC/SHA-256)
//   per connection, once: Parse of the five statements of SPEC.md (named s1..s5)
//   per request: Bind / Execute / Sync of one statement (binary parameters and results), pipelined: many requests per write;
//                the answer is delimited by ReadyForQuery (the statement is committed by then: autocommit), an ErrorResponse
//                gives the SQLSTATE; exactly one query per request
//   robustness: reconnect with backoff, a request that waits more than 10 s fails (and its connection is recycled), a lost
//               connection fails only the requests in flight on it
//
// API:  const pool = PG.createPool({ url(i), user, password, database, conns, onResults(list), open?, timeoutMs? })
//       pool.ready              Promise: resolves once every connection has either come up or failed its first attempt
//       pool.submit(tag, act, params)   act 1..5 = statement; params = array of Uint8Array (binary Bind parameters);
//                                       false: the database is down (every connection failed twice and is backing off): nothing queued.
//                                       While a connection is being (re)established a request waits for it, up to 1 s.
//       pool.flush()            write what submit() queued (also happens in a microtask)
//       pool.lastError          text of the latest connection failure
//   onResults(list): [{ tag, act, cmd: Uint8Array|null (CommandComplete tag), code: string|null (SQLSTATE), msg: string|null,
//                       rows: Uint8Array[][] (binary column values, DataRow order) }]; ok iff cmd !== null && code === null.
(function (root) {
  'use strict';
  const enc = new TextEncoder();
  const dec = new TextDecoder();
  const subtle = () => (root.crypto || globalThis.crypto).subtle;

  // ---- statements (SPEC.md, exactly) -----------------------------------------------------------------------------------------
  const POST_SQL = 'SELECT p.id, p.body, p.created_at, u.username, ' +
    '(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count ' +
    'FROM posts p JOIN users u ON u.id = p.user_id';
  const INT8 = 20;
  const STATEMENTS = [
    null,
    { sql: 'SELECT 1', types: [] },                                                            // act 1
    { sql: POST_SQL + ' ORDER BY p.created_at DESC, p.id DESC LIMIT 20', types: [] },          // act 2
    { sql: POST_SQL + ' WHERE p.id = $1', types: [INT8] },                                     // act 3
    { sql: 'INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at', types: [INT8, 0] }, // act 4
    { sql: 'INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING', types: [INT8, INT8] }, // act 5
  ];

  // ---- byte helpers ---------------------------------------------------------------------------------------------------------
  const cstr = (s) => { const b = enc.encode(s); const o = new Uint8Array(b.length + 1); o.set(b); return o; };
  function concat(parts) {
    let n = 0;
    for (const p of parts) n += p.length;
    const o = new Uint8Array(n);
    let k = 0;
    for (const p of parts) { o.set(p, k); k += p.length; }
    return o;
  }
  const rd32 = (b, p) => ((b[p] << 24) | (b[p + 1] << 16) | (b[p + 2] << 8) | b[p + 3]) | 0;
  const rd16 = (b, p) => (b[p] << 8) | b[p + 1];
  function w32(o, p, n) { o[p] = n >>> 24; o[p + 1] = (n >>> 16) & 255; o[p + 2] = (n >>> 8) & 255; o[p + 3] = n & 255; }
  // a protocol message: type byte (0 = none: the startup packet) + Int32 length (counts itself) + body parts
  function message(type, ...parts) {
    let n = 4;
    for (const p of parts) n += p.length;
    const o = new Uint8Array(n + (type ? 1 : 0));
    let k = 0;
    if (type) o[k++] = type;
    w32(o, k, n); k += 4;
    for (const p of parts) { o.set(p, k); k += p.length; }
    return o;
  }
  const T = (c) => c.charCodeAt(0);
  const i32b = (n) => { const o = new Uint8Array(4); w32(o, 0, n); return o; };
  const i16b = (n) => Uint8Array.of((n >>> 8) & 255, n & 255);
  const b64 = (u8) => { let s = ''; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return btoa(s); };
  const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
  const cstrAt = (b, p) => { let e = p; while (b[e] !== 0 && e < b.length) e++; return [dec.decode(b.subarray(p, e)), e + 1]; };

  // ---- MD5 (the legacy password method; WebCrypto has none) -----------------------------------------------------------------
  const MD5_K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) >>> 0);
  const MD5_S = [].concat(...[[7, 12, 17, 22], [5, 9, 14, 20], [4, 11, 16, 23], [6, 10, 15, 21]].map((r) => [].concat(r, r, r, r)));
  function md5(msg) {
    const n = msg.length;
    const blocks = Math.ceil((n + 9) / 64);
    const padded = new Uint8Array(blocks * 64);
    padded.set(msg);
    padded[n] = 0x80;
    const dv = new DataView(padded.buffer);
    const bits = n * 8;
    dv.setUint32(blocks * 64 - 8, bits >>> 0, true);
    dv.setUint32(blocks * 64 - 4, Math.floor(bits / 4294967296), true);
    let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (let blk = 0; blk < blocks; blk++) {
      const M = new Uint32Array(16);
      for (let i = 0; i < 16; i++) M[i] = dv.getUint32(blk * 64 + i * 4, true);
      let A = a0, B = b0, C = c0, D = d0;
      for (let i = 0; i < 64; i++) {
        let F, g;
        if (i < 16) { F = (B & C) | (~B & D); g = i; }
        else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
        else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
        else { F = C ^ (B | ~D); g = (7 * i) & 15; }
        F = (F + A + MD5_K[i] + M[g]) >>> 0;
        A = D; D = C; C = B;
        B = (B + ((F << MD5_S[i]) | (F >>> (32 - MD5_S[i])))) >>> 0;
      }
      a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
    }
    const out = new Uint8Array(16), odv = new DataView(out.buffer);
    odv.setUint32(0, a0, true); odv.setUint32(4, b0, true); odv.setUint32(8, c0, true); odv.setUint32(12, d0, true);
    return out;
  }
  const hex = (u8) => Array.from(u8, (x) => x.toString(16).padStart(2, '0')).join('');
  // "md5" + md5(md5(password + user) + salt)
  function md5Password(user, password, salt) {
    const inner = enc.encode(hex(md5(enc.encode(password + user))));
    return 'md5' + hex(md5(concat([inner, salt])));
  }

  // ---- SCRAM-SHA-256 (RFC 5802 / 7677) ----------------------------------------------------------------------------------------
  const saltedCache = new Map(); // key derivation only: (password, salt, iterations) -> salted password
  async function hmac(key, data) {
    const k = await subtle().importKey('raw', key, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    return new Uint8Array(await subtle().sign('HMAC', k, typeof data === 'string' ? enc.encode(data) : data));
  }
  async function saltedPassword(password, salt, iter) {
    const ck = password + '\0' + b64(salt) + '\0' + iter;
    let p = saltedCache.get(ck);
    if (!p) {
      p = (async () => {
        const k = await subtle().importKey('raw', enc.encode(password), 'PBKDF2', false, ['deriveBits']);
        return new Uint8Array(await subtle().deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations: iter }, k, 256));
      })();
      saltedCache.set(ck, p);
      p.catch(() => saltedCache.delete(ck));
    }
    return p;
  }

  // ---- ErrorResponse / DataRow parsing -----------------------------------------------------------------------------------------
  function parseError(b) {
    const f = {};
    let p = 0;
    while (p < b.length && b[p] !== 0) {
      const code = String.fromCharCode(b[p++]);
      let s;
      [s, p] = cstrAt(b, p);
      f[code] = s;
    }
    return f;
  }

  // ---- one connection ---------------------------------------------------------------------------------------------------------
  class Conn {
    constructor(pool, idx) {
      this.pool = pool; this.idx = idx;
      this.state = 'down'; // connecting | auth | prep | ready | down
      this.ws = null; this.rest = null; this.out = []; this.q = []; this.qh = 0; this.fails = 0;
      this.timer = null; this.retry = null; this.fatal = null; this.fatalCode = null; this.done = [];
      this.settled = new Promise((r) => { this.settle = r; });
    }
    get inflight() { return this.q.length - this.qh; }

    connect() {
      this.retry = null;
      if (this.pool.closed) return;
      this.state = 'connecting'; this.rest = null; this.out = []; this.q = []; this.qh = 0; this.fatal = null; this.scram = null;
      let ws;
      try { ws = this.pool.open(this.pool.url(this.idx)); } catch (e) { return this.down('open failed: ' + e); }
      this.ws = ws;
      ws.binaryType = 'arraybuffer';
      this.timer = setTimeout(() => this.down('connect timeout'), 5000);
      ws.onopen = () => {
        if (this.ws !== ws) return;
        this.state = 'auth';
        const o = this.pool.opts;
        ws.send(message(0, i32b(196608), cstr('user'), cstr(o.user), cstr('database'), cstr(o.database),
          cstr('client_encoding'), cstr('UTF8'), cstr('application_name'), cstr('css-chromium'), Uint8Array.of(0)));
      };
      ws.onmessage = (ev) => { if (this.ws === ws) this.feed(new Uint8Array(ev.data)); };
      ws.onerror = () => {};
      ws.onclose = (ev) => { if (this.ws === ws) this.down(ev && ev.reason ? ev.reason : 'connection closed'); };
    }

    // a connection failed or closed: fail what is in flight on it, schedule a reconnect
    down(why) {
      const ws = this.ws;
      this.ws = null;
      if (this.timer) { clearTimeout(this.timer); this.timer = null; }
      if (ws) { ws.onopen = ws.onmessage = ws.onclose = ws.onerror = null; try { ws.close(); } catch (e) { /* closed already */ } }
      const wasReady = this.state === 'ready';
      this.state = 'down';
      this.rest = null; this.out = [];
      const msg = this.fatal || why;
      this.pool.lastError = (wasReady ? 'connection lost: ' : 'postgres unavailable: ') + msg;
      const failed = [];
      for (let i = this.qh; i < this.q.length; i++) {
        const r = this.q[i];
        failed.push({ tag: r.tag, act: r.act, cmd: null, code: i === this.qh && this.fatalCode ? this.fatalCode : null, msg: 'connection lost: ' + msg, rows: [] });
      }
      this.q = []; this.qh = 0; this.fatal = null; this.fatalCode = null;
      this.settle();
      if (failed.length) this.pool.deliver(failed);
      if (this.pool.closed) return;
      this.fails++;
      // the first retry is immediate (a terminated backend should cost almost nothing); then exponential backoff up to 5 s
      const delay = this.fails <= 1 ? Math.floor(Math.random() * 20) : Math.min(5000, 100 * 2 ** Math.min(this.fails - 1, 6)) + Math.floor(Math.random() * 100);
      this.retry = setTimeout(() => this.connect(), delay);
    }

    send(bytes) { try { this.ws.send(bytes); } catch (e) { this.down('send failed: ' + e); } }

    feed(chunk) {
      let b = chunk;
      if (this.rest) { b = concat([this.rest, chunk]); this.rest = null; }
      let p = 0;
      const n = b.length;
      while (n - p >= 5) {
        const len = rd32(b, p + 1);
        if (n - p < len + 1) break;
        const type = b[p];
        const body = b.subarray(p + 5, p + 1 + len);
        p += len + 1;
        try { this.on(type, body); } catch (e) { this.down('protocol error: ' + e); return; }
        if (this.state === 'down') return;
      }
      if (p < n) this.rest = b.subarray(p);
      if (this.done.length) { const d = this.done; this.done = []; this.pool.deliver(d); }
    }

    on(type, body) {
      switch (this.state) {
        case 'auth': return this.onAuth(type, body);
        case 'prep': return this.onPrep(type, body);
        case 'ready': return this.onReady(type, body);
      }
    }

    startupFail(body) {
      const e = parseError(body);
      this.down('startup: ' + (e.S || 'ERROR') + ' ' + (e.C || '') + ' ' + (e.M || ''));
    }

    onAuth(type, body) {
      if (type === T('E')) return this.startupFail(body);
      if (type === T('R')) {
        const code = rd32(body, 0), o = this.pool.opts;
        switch (code) {
          case 0: return; // AuthenticationOk
          case 3: return this.send(message(T('p'), cstr(o.password)));
          case 5: return this.send(message(T('p'), cstr(md5Password(o.user, o.password, body.subarray(4, 8)))));
          case 10: return this.scramStart(body.subarray(4));
          case 11: return this.scramContinue(dec.decode(body.subarray(4)));
          case 12: return this.scramFinish(dec.decode(body.subarray(4)));
          default: return this.down('unsupported authentication method ' + code);
        }
      }
      if (type === T('Z')) { // ReadyForQuery: prepare the statements
        const parts = [];
        for (let a = 1; a < STATEMENTS.length; a++) {
          const s = STATEMENTS[a];
          parts.push(message(T('P'), cstr('s' + a), cstr(s.sql), i16b(s.types.length), ...s.types.map(i32b)));
        }
        parts.push(message(T('S')));
        this.state = 'prep'; this.prepared = 0;
        this.send(concat(parts));
      }
      // ParameterStatus, BackendKeyData, NoticeResponse: nothing to do
    }

    scramStart(list) {
      let p = 0, found = false;
      while (p < list.length && list[p] !== 0) { let m; [m, p] = cstrAt(list, p); if (m === 'SCRAM-SHA-256') found = true; }
      if (!found) return this.down('server offers no SCRAM-SHA-256');
      const nonce = b64(crypto.getRandomValues(new Uint8Array(18)));
      const first = 'n=,r=' + nonce;
      this.scram = { nonce, first };
      const init = enc.encode('n,,' + first);
      this.send(message(T('p'), cstr('SCRAM-SHA-256'), i32b(init.length), init));
    }

    async scramContinue(serverFirst) {
      const ws = this.ws;
      try {
        const f = Object.fromEntries(serverFirst.split(',').map((x) => [x[0], x.slice(2)]));
        if (!f.r || !f.r.startsWith(this.scram.nonce)) throw new Error('bad server nonce');
        const iter = parseInt(f.i, 10);
        if (!(iter > 0 && iter <= 1e7)) throw new Error('bad iteration count');
        const salted = await saltedPassword(this.pool.opts.password, unb64(f.s), iter);
        const clientKey = await hmac(salted, 'Client Key');
        const stored = new Uint8Array(await subtle().digest('SHA-256', clientKey));
        const withoutProof = 'c=biws,r=' + f.r;
        const authMsg = this.scram.first + ',' + serverFirst + ',' + withoutProof;
        const sig = await hmac(stored, authMsg);
        const proof = clientKey.map((x, i) => x ^ sig[i]);
        const serverKey = await hmac(salted, 'Server Key');
        this.scram.expect = b64(await hmac(serverKey, authMsg));
        if (this.ws !== ws) return;
        this.send(message(T('p'), enc.encode(withoutProof + ',p=' + b64(proof))));
      } catch (e) { if (this.ws === ws) this.down('SCRAM: ' + e); }
    }

    scramFinish(serverFinal) {
      if (serverFinal !== 'v=' + this.scram.expect) this.down('SCRAM: server signature mismatch');
    }

    onPrep(type, body) {
      if (type === T('E')) return this.startupFail(body);
      if (type === T('1')) { this.prepared++; return; } // ParseComplete
      if (type === T('Z')) {
        if (this.prepared !== STATEMENTS.length - 1) return this.down('statements not prepared');
        if (this.timer) { clearTimeout(this.timer); this.timer = null; }
        this.state = 'ready'; this.fails = 0;
        this.settle();
        if (this.out.length) this.flush();
        this.pool.drain();
      }
    }

    onReady(type, body) {
      const r = this.q[this.qh]; // the request being answered (undefined when none is in flight)
      switch (type) {
        case 50: return; // '2' BindComplete
        case 68: { // 'D' DataRow
          if (!r) return;
          const nc = rd16(body, 0), cols = new Array(nc);
          let p = 2;
          for (let i = 0; i < nc; i++) {
            const l = rd32(body, p); p += 4;
            if (l < 0) cols[i] = null; else { cols[i] = body.subarray(p, p + l); p += l; }
          }
          r.rows.push(cols);
          return;
        }
        case 67: // 'C' CommandComplete
          if (r) r.cmd = body.subarray(0, body.length - 1);
          return;
        case 69: { // 'E' ErrorResponse
          const e = parseError(body);
          if (e.S === 'FATAL' || e.S === 'PANIC') { this.fatal = (e.S + ' ' + (e.C || '') + ' ' + (e.M || '')).trim(); this.fatalCode = e.C || null; }
          if (r) { r.code = e.C || '00000'; r.msg = e.M || 'error'; }
          return;
        }
        case 90: { // 'Z' ReadyForQuery: the statement is finished (and committed)
          if (!r) return;
          this.q[this.qh++] = null;
          if (this.qh > 256 && this.qh * 2 > this.q.length) { this.q = this.q.slice(this.qh); this.qh = 0; }
          this.done.push(r);
          return;
        }
        default: return; // NoticeResponse, ParameterStatus, ...
      }
    }

    // queue one request's Bind/Execute/Sync (written by flush())
    add(req, bytes) {
      req.t0 = performance.now();
      this.q.push(req);
      this.out.push(bytes);
    }

    flush() {
      if (!this.out.length || this.state !== 'ready') return;
      const o = this.out.length === 1 ? this.out[0] : concat(this.out);
      this.out = [];
      this.send(o);
    }
  }

  // Bind (unnamed portal, statement s<act>, all parameters and results in binary format) + Execute + Sync, in one buffer
  function bindExecuteSync(act, params) {
    let total = 1 + 4 + 1 + 3 + 2 + (params.length ? 4 : 0) + 2 + 2 + 2 + 10 + 5;
    for (const p of params) total += 4 + p.length;
    const o = new Uint8Array(total);
    let k = 0;
    o[k++] = T('B');
    const lenAt = k; k += 4;
    o[k++] = 0;                                      // portal ""
    o[k++] = T('s'); o[k++] = 48 + act; o[k++] = 0;  // statement name "s<act>"
    if (params.length) { o[k++] = 0; o[k++] = 1; o[k++] = 0; o[k++] = 1; } // one format code (binary), valid for all parameters
    else { o[k++] = 0; o[k++] = 0; }                 // no format codes (no parameters)
    o[k++] = 0; o[k++] = params.length;
    for (const p of params) { w32(o, k, p.length); k += 4; o.set(p, k); k += p.length; }
    o[k++] = 0; o[k++] = 1; o[k++] = 0; o[k++] = 1;  // one result format code: binary
    w32(o, lenAt, k - lenAt);
    o[k++] = T('E'); w32(o, k, 9); k += 4; o[k++] = 0; w32(o, k, 0); k += 4; // Execute portal "", no row limit
    o[k++] = T('S'); w32(o, k, 4); k += 4;                                    // Sync
    return o.subarray(0, k);
  }

  // ---- the pool -----------------------------------------------------------------------------------------------------------------
  function createPool(opts) {
    const pool = {
      opts, closed: false, lastError: null,
      url: opts.url,
      open: opts.open || ((u) => new WebSocket(u)),
      timeoutMs: opts.timeoutMs || 10000,
      conns: [], rr: 0, scheduled: false, waiting: [], waitMs: opts.waitMs || 1000,
      deliver(list) { opts.onResults(list); },
      submit(tag, act, params, nowait) {
        const n = pool.conns.length;
        let best = null, trying = false;
        for (let i = 0; i < n; i++) {
          const c = pool.conns[(pool.rr + i) % n];
          if (c.state === 'ready') { if (best === null || c.inflight < best.inflight) best = c; }
          else if (c.state !== 'down' || c.fails <= 1) trying = true; // being (re)established, or the immediate first retry is pending
        }
        pool.rr = (pool.rr + 1) % n;
        if (!best) {
          // no connection is up. If one is being (re)established right now the request waits for it (up to waitMs); if all of
          // them are backing off the database is down: fail at once.
          if (trying && !nowait && pool.waiting.length < 100000) { pool.waiting.push({ tag, act, params, t0: performance.now() }); return true; }
          return false;
        }
        best.add({ tag, act, cmd: null, code: null, msg: null, rows: [], t0: 0 }, bindExecuteSync(act, params));
        if (!pool.scheduled) { pool.scheduled = true; queueMicrotask(pool.flush); }
        return true;
      },
      drain() {
        const w = pool.waiting;
        if (!w.length) return;
        pool.waiting = [];
        for (const r of w) if (!pool.submit(r.tag, r.act, r.params)) pool.deliver([{ tag: r.tag, act: r.act, cmd: null, code: null, msg: pool.lastError || 'no database connection', rows: [] }]);
      },
      flush() {
        pool.scheduled = false;
        for (const c of pool.conns) c.flush();
      },
      // a request that waits for its answer for more than the timeout fails; its connection is recycled
      sweep() {
        const now = performance.now();
        if (pool.waiting.length && now - pool.waiting[0].t0 > pool.waitMs) {
          const late = pool.waiting.filter((r) => now - r.t0 > pool.waitMs);
          pool.waiting = pool.waiting.filter((r) => now - r.t0 <= pool.waitMs);
          pool.deliver(late.map((r) => ({ tag: r.tag, act: r.act, cmd: null, code: null, msg: pool.lastError || 'no database connection', rows: [] })));
        }
        for (const c of pool.conns) {
          if (c.state === 'ready' && c.inflight && now - c.q[c.qh].t0 > pool.timeoutMs) c.down('query timed out');
        }
      },
      close() { pool.closed = true; clearInterval(pool.sweeper); for (const c of pool.conns) { if (c.retry) clearTimeout(c.retry); c.down('closed'); } },
    };
    for (let i = 0; i < opts.conns; i++) pool.conns.push(new Conn(pool, i));
    pool.ready = Promise.all(pool.conns.map((c) => c.settled)).then(() => undefined);
    pool.sweeper = setInterval(pool.sweep, 1000);
    for (const c of pool.conns) c.connect();
    return pool;
  }

  const PG = { createPool, md5, md5Password, bindExecuteSync, STATEMENTS };
  if (typeof module !== 'undefined' && module.exports) module.exports = PG; else root.PG = PG;
})(typeof window !== 'undefined' ? window : globalThis);
