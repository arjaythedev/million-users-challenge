// Data mover between the Rust front (WebSocket), Postgres (web/pg.js) and the CSS. No decisions in here: it inserts HTML, reads the CSS
// outputs (the `counter-reset` of V / D / r / B elements: "name value name value ..." exact int32; plus the per-position `--d` custom
// props of the A chunks), frames the bytes CSS asked for into Postgres messages, turns the raw result back into integer inline
// properties for the CSS, fills the response templates the CSS owns (`--tpl-*`), JSON-escapes DB strings, and sends complete raw
// HTTP responses to the front. Frame passes (gen/http.py): the front sends the head of a connection buffer as an
// Hs..Z fragment; the glue only reads the 13 counters of Z and returns them. Protocol: src/hub.rs.
(() => {
  'use strict';
  const params = new URLSearchParams(location.search);
  const PORT = params.get('port'), WORKER = params.get('w');
  const ws = new WebSocket('ws://127.0.0.1:' + PORT + '/?w=' + WORKER);
  const root = document.getElementById('root');
  const box = document.getElementById('c');
  const gcs = getComputedStyle;
  const enc = new TextEncoder(), dec = new TextDecoder();
  let AUTH_CHUNK = 64;
  // Garbage collection policy (the front only says how many heavy requests a batch carried, `gc`): Blink frees the computed
  // styles of finished requests only when its heap is collected; ~5 MB per SHA-256 block stays resident until then.
  let GC_EVERY = 1, GC_LIGHT_EVERY = 20000, heavyAcc = 0, lightAcc = 0;
  // async major GC: the call returns at once and marking/sweeping run on background threads (a synchronous gc() stalls this
  // page's only thread for ~40 ms per request's garbage; measured in-page)
  const collect_garbage = typeof gc === 'function' ? () => { gc({ type: 'major', execution: 'async' }); } : () => {};
  const gcCredit = (n) => { if (GC_EVERY && (heavyAcc += n) >= GC_EVERY) { heavyAcc = 0; collect_garbage(); } };

  let pool = null;
  const reqs = new Map(); // rid -> { act, un, body, errmsg, heavy } for requests that wait for Postgres

  const prop = (cs, name) => cs.getPropertyValue(name);

  // computed `counter-reset` ("name value name value ...") -> {name: int}
  function counters(cs) {
    const t = cs.counterReset.split(' '), o = {};
    for (let i = 0; i + 1 < t.length; i += 2) o[t[i]] = +t[i + 1];
    return o;
  }

  // ---- the templates the CSS owns -------------------------------------------------------------------------------------------
  // a computed `<string>` custom property is a CSS string token: strip the quotes, decode the escapes
  function cssString(t) {
    t = t.trim();
    if (t.length >= 2 && (t[0] === '"' || t[0] === "'")) t = t.slice(1, -1);
    return t.replace(/\\(?:([0-9a-fA-F]{1,6})[ \t\n\r\f]?|([^]))/g, (m, h, c) => (h ? String.fromCodePoint(parseInt(h, 16)) : c));
  }
  const TPL = { kind: [], row: '', ts: '', sep: '', head: '', close: '' };
  const msgCache = new Map(), reasonCache = new Map();
  const readStr = (name) => cssString(prop(gcs(root), name));
  function readTemplates() {
    for (let k = 1; k <= 8; k++) TPL.kind[k] = readStr('--tpl-' + k);
    TPL.row = readStr('--tpl-row'); TPL.ts = readStr('--tpl-ts'); TPL.sep = readStr('--tpl-sep');
    TPL.head = readStr('--tpl-head'); TPL.close = readStr('--tpl-close');
  }
  const cached = (m, key, name) => { let v = m.get(key); if (v === undefined) { v = readStr(name); m.set(key, v); } return v; };

  // JSON string escaping, exactly the old src/json.rs::esc (= Go's encoding/json for valid UTF-8): only `"`, `\` and control
  // characters change; <, >, &, /, DEL and non-ASCII stay as they are
  const ESC = { '"': '\\"', '\\': '\\\\', '\n': '\\n', '\r': '\\r', '\t': '\\t', '\b': '\\b', '\f': '\\f' };
  const esc = (s) => s.replace(/["\\\u0000-\u001f]/g, (c) => ESC[c] || '\\u00' + (c.charCodeAt(0) < 16 ? '0' : '') + c.charCodeAt(0).toString(16));

  // `%x` markers are filled in one pass over the template, so data is never re-scanned
  const fill = (tpl, f) => tpl.replace(/%([a-z])/g, (_, c) => f(c));

  // ---- request chains ---------------------------------------------------------------------------------------------------------
  // ancestors of the verdict element with class `cls`, outermost first (= chunk order)
  function chain(v, cls) {
    const a = [];
    for (let e = v.parentElement; e && e !== box; e = e.parentElement) if (e.classList.contains(cls)) a.push(e);
    return a.reverse();
  }

  // values >= 0 of --{p}0..--{p}{size-1} over all chunks, in order (-1 / unset = "no output at this position")
  function collect(chunks, p, size) {
    const out = [];
    for (const el of chunks) {
      const cs = gcs(el);
      for (let j = 0; j < size; j++) {
        const x = parseInt(prop(cs, '--' + p + j), 10);
        if (x >= 0) out.push(x);
      }
    }
    return out;
  }

  // username JSON string literal: decoded-payload bytes [uns, une) of the token (UTF-8), quotes included, kept verbatim
  function username(v, o) {
    const s = o.uns, e = o.une;
    if (e <= s) return '';
    return dec.decode(Uint8Array.from(collect(chain(v, 'A'), 'd', AUTH_CHUNK).slice(s, e)));
  }

  // the body text: events of the B chunks (counters a{j}/b{j}: -1 none, else epoch * 2^21 + payload; payload > 0x10FFFF means
  // (payload - 0x110000) U+FFFD) of the winning `body` occurrence (epoch bep), cut to the trimmed UTF-16 length blen
  function bodyText(v, o) {
    const cps = [];
    for (const el of chain(v, 'B')) {
      const t = gcs(el).counterReset.split(' ');
      for (let i = 1; i < t.length; i += 2) {
        const x = +t[i];
        if (x < 0 || (x >>> 21) !== o.bep) continue;
        const p = x & 0x1fffff;
        if (p > 0x10ffff) for (let n = p - 0x110000; n > 0; n--) cps.push(0xfffd);
        else cps.push(p);
      }
    }
    let str = '';
    for (let i = 0; i < cps.length; i += 8192) str += String.fromCodePoint.apply(null, cps.slice(i, i + 8192));
    return str.slice(0, o.blen);
  }

  // ---- stage 3: the raw Postgres result as integer properties -------------------------------------------------------------------
  // column indexes of (id, created_at, like_count) per statement; the strings (body, username) stay here as JS strings
  const COLS = { 2: [0, 2, 4], 3: [0, 2, 4], 4: [0, 1, -1] };
  function bytesProps(prefix, col) {
    let s = '';
    if (col) for (let i = 0; i < col.length; i++) if (col[i]) s += '--' + prefix + i + ':' + col[i] + ';';
    return s;
  }
  function stage3Html(rid, r) {
    let h = "<b class='D' style='--rid:" + rid + ';';
    if (r.code) for (let i = 0; i < 5; i++) h += '--e' + i + ':' + (r.code.charCodeAt(i) || 0) + ';'; // SQLSTATE as char codes
    if (r.cmd) for (let i = 0; i < r.cmd.length && i < 16; i++) h += '--t' + i + ':' + r.cmd[i] + ';';  // CommandComplete tag
    h += "'>";
    const c = COLS[r.act];
    if (c) {
      for (const row of r.rows) {
        h += "<b class='r' style='--rid:" + rid + ';' + bytesProps('i', row[c[0]]) + bytesProps('s', row[c[1]]) +
          (c[2] >= 0 ? bytesProps('l', row[c[2]]) : '') + "'></b>";
      }
    }
    return h + '</b>';
  }

  // ---- the response -----------------------------------------------------------------------------------------------------------
  // o: counters of V (CSS answered alone) or D; rq: request state; rows: the r elements' counters + the DB row for strings
  function respond(o, rq, rows) {
    const msg = () => esc(cached(msgCache, o.err, '--msg-' + o.err));
    let r = '';
    const t = TPL.kind[o.kind];
    if (t.indexOf('%r') >= 0) {
      const parts = [];
      for (const x of rows) {
        const digits = String(x.c.dt).slice(1) + String(x.c.tm).slice(1);
        let k = 0;
        const when = TPL.ts.replace(/X/g, () => digits[k++]);
        const created = o.kind === 5;
        parts.push(fill(TPL.row, (m) => (m === 'i' ? x.c.id : m === 'b' ? esc(created ? rq.body : dec.decode(x.db[1])) :
          m === 'd' ? when : m === 'a' ? (created ? rq.un || '""' : '"' + esc(dec.decode(x.db[3])) + '"') : m === 'l' ? x.c.lc : '')));
      }
      r = parts.join(TPL.sep);
    }
    const body = enc.encode(fill(t, (m) => (m === 'u' ? o.up : m === 'e' ? esc(rq.errmsg || '') : m === 'p' ? o.pid : m === 'm' ? msg() : m === 'r' ? r : '')));
    const head = enc.encode(fill(TPL.head, (m) => (m === 's' ? o.st : m === 'w' ? cached(reasonCache, o.st, '--reason-' + o.st) : m === 'c' ? (o.cl ? TPL.close : '') : m === 'n' ? body.length : '')));
    const out = new Uint8Array(head.length + body.length);
    out.set(head); out.set(body, head.length);
    return out;
  }

  // ---- messages to the front: kind | ndone | ndone x (rid len bytes) | npend | npend x rid   (little endian) -------------------------
  const FRAME_FIELDS = ['ok', 'bad', 'hl', 'cl', 'used', 'close', 'cont', 'mt', 'ps', 'pe', 'as', 'ae', 'slow'];
  function pack(kind, done, pend, frames) {
    frames = frames || [];
    let n = 1 + 4 + 4 + 4 * pend.length + 4 + frames.length * 4 * (1 + FRAME_FIELDS.length);
    for (const d of done) n += 8 + d[1].length;
    const o = new Uint8Array(n), dv = new DataView(o.buffer);
    let p = 0;
    o[p++] = kind;
    dv.setUint32(p, done.length, true); p += 4;
    for (const d of done) {
      dv.setUint32(p, d[0], true); dv.setUint32(p + 4, d[1].length, true); p += 8;
      o.set(d[1], p); p += d[1].length;
    }
    dv.setUint32(p, pend.length, true); p += 4;
    for (const r of pend) { dv.setUint32(p, r, true); p += 4; }
    dv.setUint32(p, frames.length, true); p += 4;
    for (const [rid, z] of frames) {
      dv.setUint32(p, rid, true); p += 4;
      for (const f of FRAME_FIELDS) { dv.setUint32(p, z[f] >>> 0, true); p += 4; }
    }
    return o;
  }
  const removeChain = (rid) => { const q = document.getElementById('q' + rid); if (q) q.remove(); };

  // ---- Postgres results come back (possibly many requests per call) ---------------------------------------------------------------
  function onResults(list) {
    const mine = [];
    for (const r of list) {
      const rq = reqs.get(r.tag), v = document.getElementById('v' + r.tag);
      if (!rq || !v) { reqs.delete(r.tag); continue; }
      rq.errmsg = r.msg;
      v.insertAdjacentHTML('beforeend', stage3Html(r.tag, r));
      mine.push([r, rq, v]);
    }
    const done = [];
    for (const [r, rq, v] of mine) {
      const d = v.firstElementChild;
      const o = counters(gcs(d));
      const rows = [];
      if (o.kind >= 3 && o.kind <= 5) {
        const rs = d.children;
        for (let j = 0; j < rs.length; j++) rows.push({ c: counters(gcs(rs[j])), db: r.rows[j] });
      }
      done.push([r.tag, respond(o, rq, rows)]);
      reqs.delete(r.tag);
    }
    if (!done.length) return;
    ws.send(pack(1, done, []));
    let heavy = 0;
    for (const d of done) { removeChain(d[0]); }
    for (const [, rq] of mine) if (rq.heavy) heavy++;
    if (heavy) gcCredit(heavy);
  }

  // a connection could not be had at all: the CSS still decides what that means (health: degraded 503, others: 500)
  function noConnection(rid, act, msg) {
    return { tag: rid, act, cmd: null, code: null, msg, rows: [] };
  }

  // ---- messages from the front ------------------------------------------------------------------------------------------------
  function onInit(init) {
    AUTH_CHUNK = init.auth_chunk;
    GC_EVERY = init.gc_every;
    GC_LIGHT_EVERY = init.gc_light_every;
    root.setAttribute('style', init.style); // once; never touched again (would invalidate every in-flight request)
    readTemplates();
    pool = PG.createPool({
      url: (i) => 'ws://127.0.0.1:' + PORT + '/pg?w=' + WORKER + '&i=' + i,
      user: init.pg.user, password: init.pg.password, database: init.pg.database, conns: init.pg.conns,
      onResults,
    });
    // ready once every connection has come up or failed its first attempt: a dead database must not stall startup (health says degraded)
    pool.ready.then(() => ws.send('{"ready":1}'));
  }

  function onBatch(m) {
    if (m.add) box.insertAdjacentHTML('beforeend', m.add);
    const done = [], pend = [], failed = [], doneChains = [], frames = [];
    // frame passes first: read Z and drop the chain (nothing else is needed from it)
    for (const rid of m.frames || []) {
      const z = document.getElementById('z' + rid);
      frames.push([rid, z ? counters(gcs(z)) : { bad: 1 }]);
      const h = document.getElementById('h' + rid);
      if (h) h.remove();
    }
    const nw = m.new;
    const heavyBatch = m.gc > 0;
    let heavyDone = 0;
    for (let i = 0; i < nw.length; i++) {
      const rid = nw[i];
      const v = document.getElementById('v' + rid);
      if (!v) continue;
      const o = counters(gcs(v));
      if (o.act === 0) { // the CSS answers by itself (404, 400, 401): no database
        done.push([rid, respond(o, null, [])]);
        doneChains.push(rid);
        if (heavyBatch) heavyDone++;
        continue;
      }
      const rq = { act: o.act, un: '', body: '', errmsg: null, heavy: heavyBatch };
      let p;
      const pb = () => Uint8Array.of(o.pb0, o.pb1, o.pb2, o.pb3, o.pb4, o.pb5, o.pb6, o.pb7);
      const ub = () => Uint8Array.of(o.ub0, o.ub1, o.ub2, o.ub3, o.ub4, o.ub5, o.ub6, o.ub7);
      if (o.act === 4) { rq.un = username(v, o); rq.body = bodyText(v, o); p = [ub(), enc.encode(rq.body)]; }
      else if (o.act === 5) p = [ub(), pb()];
      else if (o.act === 3) p = [pb()];
      else p = [];
      reqs.set(rid, rq);
      pend.push(rid);
      if (!pool.submit(rid, o.act, p)) failed.push(noConnection(rid, o.act, pool.lastError || 'no database connection'));
    }
    pool.flush();
    ws.send(pack(0, done, pend, frames));
    for (const rid of doneChains) removeChain(rid);
    if (failed.length) onResults(failed);
    // after the reply is out and the DOM is released: collect (heavy batches every GC_EVERY heavy requests; the requests that
    // went to Postgres count when they finish; pages that only see light requests every GC_LIGHT_EVERY requests)
    if (heavyBatch) {
      if (heavyDone) gcCredit(heavyDone);
    } else if ((nw.length || frames.length) && GC_LIGHT_EVERY && (lightAcc += m.w || nw.length) >= GC_LIGHT_EVERY) {
      lightAcc = 0;
      collect_garbage();
    }
  }

  ws.onmessage = (e) => {
    const m = JSON.parse(e.data);
    if (m.init) onInit(m.init); else onBatch(m);
  };
})();
