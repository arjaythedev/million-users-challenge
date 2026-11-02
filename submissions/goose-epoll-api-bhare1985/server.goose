// A feed API on Postgres whose routes, SQL, checks and responses are read from an OpenAPI file at start-up: one epoll loop per CPU,
// each with its own SO_REUSEPORT listeners on PORT and EXTRA_PORT and its own libpq connections in pipeline mode.
// Config: DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT, API_CONFIG (default openapi.json). See README.md.
import std;
import ffi;
import json;
import os;
import slice_pool;
import auth;
import http;
import jsn;
import net;
import pq;
import io;
import wire;

let INBOX_SIZE = 256 * 1024;                // room for the bytes one wait brings in
let MAX_CARRY = 2 * http::MAX_REQUEST + 4096;    // unparsed bytes kept for one connection: a header block and a body
let GZIP_MIN = 1024;                        // smaller bodies (everything but the feed) go out uncompressed
let BATCH_MIN = 32;                         // an iteration with fewer events than this ...
let BATCH_US = 2000;                        // ... is followed by a 2 ms pause (see README.md, "Batching window")
let IDLE_US = 1000000;                      // an idle wait ends after a second, for the housekeeping below
let PACK = 64;                              // reads sent to one Postgres connection per iteration
let RING = 1024;                            // statements in flight per Postgres connection (a power of two)
let MAX_EVENTS = 256;

type Pool = slice_pool<u8>;

// ---- state ----

// What a statement sent to Postgres was for, which is what its response needs to know.
enum Job {
    Ep { ep: u32, off: u32, len: u32 },       // an operation of the OpenAPI file: the request's values wait in the pool at off, len
    Sync,                                      // a pipeline Sync, not a request
}

// One per client slot (the event loop's handle for it), so a million idle keep-alive connections cost 16 MB here. The
// bytes of a request that is split across receives live in the byte pool, and only while it is split.
struct Client {
    gen: u32,                               // bumped on close, so a late result for a closed client is dropped
    open: bool,
    busy: bool,                             // waiting for its statement; later requests wait too
    sending: bool,                          // its response is on its way out; later requests wait too
    closing: bool,                          // close once the response is out
    in_off: u32, in_len: u32,               // unparsed request bytes carried between receives (a pool run)
}

struct Pending {
    client: u32, gen: u32,
    job: Job,
    close: bool, gzip: bool,
}

struct Pg {
    c: pq::Conn, sock: i32,                 // c == 0: not connected
    head: u32, queued: u32,                 // the ring of sent statements and Syncs, oldest first
    batch: u16,                             // reads sent in this loop iteration, up to PACK
    dirty: bool,                            // something to flush at the end of this iteration
    unsynced: bool,                         // reads sent since the last Sync
    blocked: bool,                          // libpq could not flush everything: the poll waits for POLLOUT too
    in_result: bool,                        // this statement's result has arrived, its end marker has not
    has_held: bool, held_failed: bool,      // a write's result, waiting for its commit
    held: Pending, held_res: pq::Res,
}

// ---- the configured API ----
// The OpenAPI file is compiled at start-up into these flat tables: how to route a request, which values to take from it, the SQL
// and which of those values it takes, and how to turn the result into the response. The text they refer to (names,
// messages, the literal parts of responses) is in W.lit.

let S_JWT = 0;                              // where a request value comes from (the order is the order they are checked in)
let S_PATH = 1;
let S_QUERY = 2;
let S_BODY = 3;

let T_TEXT = 0;                             // what it is: any text, a positive integer, an integer
let T_ID = 1;
let T_INT = 2;

struct Slot {
    src: u8, kind: u8, trim: bool, required: bool, has_default: bool,
    index: i32,                             // path: the segment; jwt: the claim
    name_off: i32, name_len: i32,           // "path.id": what the OpenAPI file calls it
    key_off: i32, key_len: i32,             // body: the key in the JSON; query: the name in the query string
    err_off: i32, err_len: i32,             // the 400 for a value that is not valid (or, for a body, the 400 for a missing one)
    max_chars: i32, max_off: i32, max_len: i32,    // the longest text, and the 400 for a longer one
    def_off: i32, def_len: i32,             // a query value's default
}

struct Seg { off: i32, len: i32, slot: i32 }      // a path segment: literal text, or (slot >= 0) a :parameter
struct Claim { name_off: i32, name_len: i32, alias_off: i32, alias_len: i32, kind: u8 }      // a JWT claim a token must carry: what the token calls it, and what the OpenAPI file calls it
struct ErrRule { code_off: i32, status: i32, msg_off: i32, msg_len: i32 }    // SQLSTATE (5 bytes at code_off) -> status, message
struct Rename { from_off: i32, from_len: i32, to_off: i32, to_len: i32 }     // a column's name in the response

let P_LIT = 0;                              // literal bytes of the response: lit[a..a+b]
let P_VALUE = 1;                            // a value of the request: slot a of the endpoint
let P_ROWS = 2;                             // every row, as an array of objects
let P_ROW = 3;                              // the first row, as an object (null if there is none)
let P_COL = 4;                              // one column of the first row: the column named lit[a..a+b]
let P_AFFECTED = 5;                         // the number of rows the command changed
let P_AFFECTED_IS = 6;                      // whether that number is a: true or false
let P_UPTIME = 7;                           // seconds since the server started
struct Piece { kind: u8, a: i32, b: i32 }

struct Endpoint {
    method_off: i32, method_len: i32,
    seg_off: i32, seg_n: i32,
    auth: bool, write: bool,
    stmt: i32,                              // the statement's number, as prepared on every Postgres connection
    param_off: i32, param_n: i32,           // the SQL's parameters: slots of this endpoint, in order
    slot_off: i32, slot_n: i32,
    body_n: i32,                            // how many of the slots are body fields
    malformed_off: i32, malformed_len: i32, // the 400 for a body that is not JSON
    ok_status: i32, aff_n: i32, aff_status: i32,      // aff_n >= 0: when the command changed that many rows, the status is aff_status
    resp_off: i32, resp_n: i32,             // the response's pieces
    nr_status: i32, nr_off: i32, nr_len: i32,        // nr_status != 0: a query with no rows is this error
    err_off: i32, err_n: i32,               // SQLSTATE rules
    dbe_status: i32, dbe_off: i32, dbe_n: i32,       // dbe_n > 0: the response when Postgres cannot be reached
    ren_off: i32, ren_n: i32,               // column renames
}

root struct W {
    started: i64, now: i64,                 // seconds since the epoch: the start, and this loop iteration
    npg: i64,
    retry_at: i64,                          // the second a lost Postgres connection was last retried
    db_url: u8[>..] = [],                   // NUL-terminated
    events: io::Event[MAX_EVENTS],
    clients: Client[>..] = [],              // indexed by slot; dense, reused
    pgs: Pg[>..] = [],
    ring: Pending[>..] = [],                // see ring_index
    // the response being built
    out: u8[>..] = [],
    zbuf: u8[>..] = [],                     // gzip output
    text: u8[>..] = [],                     // a database value that needs escaping, or a timestamp
    body_start: i64, length_at: i64,
    gzip_ok: bool,
    // the compiled OpenAPI file
    lit: u8[>..] = [],
    pieces: Piece[>..] = [],
    eps: Endpoint[>..] = [],
    slots: Slot[>..] = [],
    segs: Seg[>..] = [],
    params: i32[>..] = [],
    errs: ErrRule[>..] = [],
    renames: Rename[>..] = [],
    claims: Claim[>..] = [],
    sql: u8[>..] = [],                      // the statements, each NUL-terminated
    sql_off: i32[>..] = [],
    sql_np: i32[>..] = [],
    keys: u8[>..] = [],                     // the column names of a result, as JSON keys
}

var INBOX: u8[>..] = [];                    // the bytes the last wait received, event by event
var RB: u8[>..] = [];                       // carried bytes being parsed again

// ---- responses ----

let LENGTH_DIGITS = 6;                      // room for the Content-Length value

fn begin(w: W&, status: i64, close: bool) {
    w.out.clear();
    w.out.append(http::status_line(status));
    if close { w.out.append("Connection: close\r\n"); }
    w.out.append("Content-Type: application/json\r\nContent-Length: ");
    w.length_at = w.out.len;
    w.out.append("      \r\n\r\n");         // LENGTH_DIGITS blanks, filled in by finish
    w.body_start = w.out.len;
}

// Fills in Content-Length. With Accept-Encoding: gzip, a large body is compressed and replaces the plain one.
fn finish(w: W&) {
    var n = w.out.len - w.body_start;
    if w.gzip_ok && n >= GZIP_MIN {
        let cap = n + n / 8 + 256;
        if w.zbuf.len < cap { w.zbuf.resize(cap, 0); }
        let z = net::gzip(w.zbuf[0], cap, w.out[w.body_start..]);
        if z > 0 {
            w.out.resize(w.length_at - "Content-Length: ".len);      // rewrite from the Content-Length line on
            format(w.out, "Content-Encoding: gzip\r\nContent-Length: ", z, "\r\n\r\n");
            w.out.append(w.zbuf[..z]);
            return;
        }
    }
    var at = w.length_at + LENGTH_DIGITS;     // the digits, right-aligned in the blanks
    loop { at--; w.out[at] = '0' + (n % 10) as! u8; n /= 10; if n == 0 { break; } }
}

fn error(w: W&, status: i64, message: const u8[:], close: bool) {
    begin(w, status, close);
    format(w.out, "{\"error\":\"", message, "\"}");
    finish(w);
}

fn internal_error(w: W&, close: bool) { error(w, 500, "internal server error", close); }

// ---- clients ----

// The pool run of len bytes at off.
inline fn run(sp: Pool&, off: u32, len: u32) -> u8[:] { sp.items[off..off + len] }

fn close_client(w: W&, sp: Pool&, client: u32) {
    let c .= w.clients[client];
    drop_carry(sp, c);
    c = Client { gen: c.gen + 1, .. };
    io::close(client);                    // the socket closes once its receive and send have ended
}

// Sends w.out to the client. If it is not all out at once, the next request waits for the SEND event.
fn send_resp(w: W&, sp: Pool&, client: u32, close: bool) {
    let c .= w.clients[client];
    if close { c.closing = true; }
    let sent = io::send(client, w.out[0], w.out.len, true);
    if sent < 0 { close_client(w, sp, client); return; }
    if sent == 0 { c.sending = true; return; }
    if close { close_client(w, sp, client); }
}

fn drop_carry(sp: Pool&, c: Client&) {
    sp.free_slice(run(sp, c.in_off, c.in_len));
    c.in_off = 0;
    c.in_len = 0;
}

// Adds data to the bytes carried for c. False if the carried request would grow too large.
fn add_carry(sp: Pool&, c: Client&, data: u8[:]) -> bool {
    let total = c.in_len + data.len;
    if total > MAX_CARRY { return false; }
    let carried = sp.realloc_slice(run(sp, c.in_off, c.in_len), total);     // grows in place when it can
    copy_into(carried[c.in_len..], data);
    c.in_off = sp.items.range_index(carried) as u32;
    c.in_len = total as u32;
    return true;
}

// ---- Postgres: a few pipelined connections per worker ----
// Every request runs one prepared statement. Reads queue up behind each other on a connection and share one Sync at
// the end of the loop iteration; each write gets its own Sync right after it, so it commits (or fails, e.g. a like's
// foreign key) on its own, and its response is sent only once that Sync's result arrives, i.e. after the commit.

fn pg_connect(w: W&, i: i64) -> bool {
    let c = pq::connect_n(w.db_url[0], w.sql_off.len as i32, w.sql[0], w.sql_off[0], w.sql_np[0]);
    if c == 0 { return false; }
    if c == 0xffffffffffffffff { exit(1); }       // Postgres refuses a statement of the OpenAPI file; shim.c said which and why
    let p .= w.pgs[i];
    p.c = c;
    p.sock = pq::socket(c);
    pg_watch(w, i);
    return true;
}

// Asks the ring for the next event on connection i's socket: readable, and writable too while libpq has output left.
// Each event is answered by asking again, which makes the watch level-triggered, as libpq's reads need.
fn pg_watch(w: W&, i: i64) {
    let p .= w.pgs[i];
    var mask: u32 = io::POLLIN;
    if p.blocked { mask = io::POLLIN | io::POLLOUT; }
    io::poll(i as i32, p.sock, mask);
}

// The statements in flight on connection i are a ring of RING entries, oldest first. Entry k lives at
// ring[(k % RING) * npg + i]: the rings of all connections are interleaved, so the one array grows a row of npg
// entries at a time and is only as long as the deepest ring has ever been.
fn ring_index(w: W&, i: i64, k: i64) -> i64 { (k & (RING - 1)) * w.npg + i }

fn queue_push(w: W&, i: i64, e: Pending) {
    let p .= w.pgs[i];
    let at = ring_index(w, i, p.head + p.queued);
    if at >= w.ring.len { w.ring.resize((at / w.npg / 16 + 1) * 16 * w.npg, Pending { .. }); }
    w.ring[at] = e;
    p.queued++;
}

fn queue_front(w: W&, i: i64) -> Pending { w.ring[ring_index(w, i, w.pgs[i].head)] }

fn queue_pop(w: W&, i: i64) {
    let p .= w.pgs[i];
    p.head = (p.head + 1) & (RING - 1);
    p.queued--;
}

fn is_sync(job: Job) -> bool { match job { Sync => true, _ => false } }
fn is_write(w: W&, job: Job) -> bool { match job { Ep n => w.eps[n.ep].write, _ => false } }

// The connection is gone: every request on it gets a 500.
fn pg_fail(w: W&, sp: Pool&, i: i64) {
    let p .= w.pgs[i];
    pq::lost(p.c);
    io::poll_cancel(i as i32);
    pq::finish(p.c);
    p.c = 0;
    if p.has_held {
        p.has_held = false;
        pq::clear(p.held_res);
        respond(w, sp, p.held, 0, true);
    }
    while p.queued > 0 {
        let e = queue_front(w, i);
        queue_pop(w, i);
        if !is_sync(e.job) { respond(w, sp, e, 0, true); }
    }
    p.in_result = false; p.unsynced = false; p.dirty = false; p.blocked = false; p.batch = 0;
}

// Reads fill one connection's batch for this loop iteration (up to PACK statements), so a batch is one write, one
// round trip and one Sync on one connection; the next batch goes to the connection with the shortest queue. Writes
// get their own connections (the last quarter), spread by queue length, so a slow commit never holds up the reads
// pipelined behind it. Returns the connection index, or -1.
fn pick(w: W&, writes: bool) -> i64 {
    var best: i64 = -1;
    let nwrite = max(1, w.npg / 4);
    for i in w.npg {
        let p .= w.pgs[i];
        if p.c == 0 && w.retry_at != w.now { w.retry_at = w.now; pg_connect(w, i); }    // one lost connection a second
        if p.c == 0 || (i >= w.npg - nwrite) != writes { continue; }
        if !writes && p.dirty && p.batch < PACK { return i; }
        if best < 0 || p.queued < w.pgs[best].queued { best = i; }
    }
    return best;
}

fn pg_sync(w: W&, i: i64) {
    let p .= w.pgs[i];
    if pq::send_sync(p.c) == 0 { return; }
    queue_push(w, i, Pending { job: Job.Sync, .. });
    p.unsynced = false;
}

// Frees what a job keeps in the pool.
fn release(sp: Pool&, job: Job) {
    match job {
        Ep n => { sp.free_slice(run(sp, n.off, n.len)); },
        _ => {},
    }
}

// Sends the job's statement (with its parameters in pbuf and poffs) on the best Postgres connection and queues it. False
// if no connection can take it.
fn submit(w: W&, sp: Pool&, client: u32, close: bool, job: Job, pbuf: u8[:], poffs: i32[:]) -> bool {
    var ei: u32 = 0;
    match job { Ep n => { ei = n.ep; }, _ => {} }
    let write = w.eps[ei].write;
    let i = pick(w, write);
    if i < 0 || w.pgs[i].queued + 2 >= RING { return false; }
    let e = w.eps[ei];
    if pq::send_n(w.pgs[i].c, e.stmt, e.param_n, pbuf[0], poffs[0]) == 0 { return false; }
    queue_push(w, i, Pending { client: client, gen: w.clients[client].gen, job: job, close: close, gzip: w.gzip_ok });
    let p .= w.pgs[i];
    p.dirty = true;
    p.unsynced = true;
    p.batch++;
    if write { pg_sync(w, i); }
    return true;
}

fn pg_flush(w: W&, sp: Pool&, i: i64) {
    let p .= w.pgs[i];
    let r = pq::flush(p.c);
    if r < 0 { pg_fail(w, sp, i); return; }
    let blocked = r != 0;
    if blocked != p.blocked {
        p.blocked = blocked;
        pg_watch(w, i);
    }
}

// Reads every result that has arrived. Statements and Syncs come back in the order they were sent.
fn pg_read(w: W&, sp: Pool&, i: i64) {
    let p .= w.pgs[i];
    if pq::consume_input(p.c) == 0 { pg_fail(w, sp, i); return; }
    while p.c != 0 && p.queued > 0 && pq::is_busy(p.c) == 0 {
        let e = queue_front(w, i);
        let res = pq::next_result(p.c);
        if is_sync(e.job) {
            if res == 0 { break; }
            if pq::status(res) != pq::PIPELINE_SYNC { p.held_failed = true; pq::clear(res); continue; }   // the commit failed
            pq::clear(res);
            queue_pop(w, i);
            if p.has_held {                       // committed: now the write can be answered
                let held = p.held;
                let held_res = p.held_res;
                let failed = p.held_failed;
                p.has_held = false;
                p.held_failed = false;
                respond(w, sp, held, held_res, failed);
                pq::clear(held_res);
            }
            continue;
        }
        if p.in_result {                          // the NULL that ends this statement's results
            if res != 0 { pq::clear(res); continue; }
            p.in_result = false;
            queue_pop(w, i);
            continue;
        }
        p.in_result = true;
        if is_write(w, e.job) {                      // wait for its Sync
            p.held = e;
            p.held_res = res;
            p.has_held = true;
            p.held_failed = false;
            continue;
        }
        respond(w, sp, e, res, res == 0);
        pq::clear(res);
    }
}

// ---- answering from the database ----

// Appends the text column at (row, col) as JSON string contents. It goes in raw first, and is escaped in place only on
// the rare occasion that it contains a quote, a backslash or a control character.
fn put_text(w: W&, res: pq::Res, row: i32, col: i32) {
    let start = w.out.len;
    pq::append_text(res, row, col, w.out);
    if find_any(w.out[start..], wire::ESCAPED) < 0 { return; }
    w.text.clear();
    w.text.append(w.out[start..]);
    w.out.resize(start);
    wire::putjs(w.out, w.text[..]);
}

// A timestamp column as Postgres writes it in a UTC session (2025-12-31 23:59:19.409123+00) in the API's form
// (2025-12-31T23:59:19.409Z): milliseconds, truncated.
fn put_timestamp(w: W&, res: pq::Res, row: i32, col: i32) {
    w.text.clear();
    pq::append_text(res, row, col, w.text);
    let s = w.text[..];
    w.out.push('"');
    if s.len < 19 || s[10] != ' ' { w.out.append(s); w.out.push('"'); return; }
    w.out.append(s[..10]);
    w.out.push('T');
    w.out.append(s[11..19]);
    w.out.push('.');
    var i = if s.len > 19 && s[19] == '.' { 20 } else { 19 };
    for digit in 3 {
        if i < s.len && s[i] >= '0' && s[i] <= '9' { w.out.push(s[i]); i++; } else { w.out.push('0'); }
    }
    w.out.append("Z\"");
}

// One cell as JSON, by its column's type.
fn put_cell(w: W&, res: pq::Res, row: i32, col: i32, oid: u32) {
    if pq::is_null(res, row, col) != 0 { w.out.append("null"); return; }
    if oid == 20 || oid == 21 || oid == 23 || oid == 26 || oid == 700 || oid == 701 || oid == 1700 || oid == 114 || oid == 3802 {
        pq::append_text(res, row, col, w.out);                      // integers, floats, numeric, json: as they are
        return;
    }
    if oid == 16 {
        w.text.clear();
        pq::append_text(res, row, col, w.text);
        w.out.append(if w.text[0] == 't' { "true" } else { "false" });
        return;
    }
    if oid == 1184 || oid == 1114 { put_timestamp(w, res, row, col); return; }
    w.out.push('"');
    put_text(w, res, row, col);
    w.out.push('"');
}

// The result's rows as JSON objects: one if first_only (null if there is none), else an array of all of them. Keys are the
// columns' names, renamed as the endpoint says.
fn put_rows(w: W&, ep: Endpoint, res: pq::Res, first_only: bool) {
    let nf = pq::fields(res);
    let nrows = pq::rows(res);
    if (first_only && nrows == 0) || nf > 64 { w.out.append("null"); return; }
    var koff: i32[65] = [0; 65];
    var oid: u32[64] = [0; 64];
    w.keys.clear();
    for c in nf {
        koff[c] = w.keys.len as i32;
        w.text.clear();
        pq::append_name(res, c, w.text);
        var name = w.text[..];
        for r in ep.ren_n {
            let rn = w.renames[ep.ren_off + r];
            if w.lit[rn.from_off..rn.from_off + rn.from_len] == name { name = w.lit[rn.to_off..rn.to_off + rn.to_len]; break; }
        }
        w.keys.push('"');
        wire::putjs(w.keys, name);
        w.keys.append("\":");
        oid[c] = pq::ftype(res, c);
    }
    koff[nf] = w.keys.len as i32;
    if !first_only { w.out.push('['); }
    let count = if first_only { 1 } else { nrows };
    for r in count {
        if r > 0 { w.out.push(','); }
        w.out.push('{');
        for c in nf {
            if c > 0 { w.out.push(','); }
            w.out.append(w.keys[koff[c]..koff[c + 1]]);
            put_cell(w, res, r, c, oid[c]);
        }
        w.out.push('}');
    }
    if !first_only { w.out.push(']'); }
}

// One column of the first row, by the column's name.
fn put_col(w: W&, res: pq::Res, name: u8[:]) {
    if pq::rows(res) == 0 { w.out.append("null"); return; }
    for c in pq::fields(res) {
        w.text.clear();
        pq::append_name(res, c, w.text);
        if w.text[..] == name { put_cell(w, res, 0, c, pq::ftype(res, c)); return; }
    }
    w.out.append("null");
}

// The k-th value of a request (see Endpoint's slots).
fn ctx_value(blob: u8[:], k: i64) -> u8[:] {
    var at = 0;
    var i = 0;
    loop {
        let n = (blob[at] as i64) | (blob[at + 1] as i64) << 8 | (blob[at + 2] as i64) << 16 | (blob[at + 3] as i64) << 24;
        if i == k { return blob[at + 4..at + 4 + n]; }
        at += 4 + n;
        i++;
    }
}

// Writes pieces off..off+n of the response, with the request's values in blob and the result in res (0: none).
fn render(w: W&, ep: Endpoint, off: i64, n: i64, blob: u8[:], res: pq::Res) {
    for k in n {
        let p = w.pieces[off + k];
        match p.kind {
            P_LIT => { w.out.append(w.lit[p.a..p.a + p.b]); },
            P_VALUE => {
                let v = ctx_value(blob, p.a);
                if w.slots[ep.slot_off + p.a].kind == T_TEXT { w.out.push('"'); wire::putjs(w.out, v); w.out.push('"'); }
                else { w.out.append(v); }
            },
            P_ROWS => { if res != 0 { put_rows(w, ep, res, false); } else { w.out.append("[]"); } },
            P_ROW => { if res != 0 { put_rows(w, ep, res, true); } else { w.out.append("null"); } },
            P_COL => { if res != 0 { put_col(w, res, w.lit[p.a..p.a + p.b]); } else { w.out.append("null"); } },
            P_AFFECTED => { format(w.out, if res != 0 { pq::affected(res) } else { 0 }); },
            P_AFFECTED_IS => { w.out.append(if res != 0 && pq::affected(res) == p.a as i64 { "true" } else { "false" }); },
            P_UPTIME => { format(w.out, w.now - w.started); },
            _ => {},
        }
    }
}

// Builds the response for an endpoint's statement: its result, or (failed) the news that Postgres could not run it.
fn answer(w: W&, sp: Pool&, n: Job.Ep, res: pq::Res, failed: bool, close: bool) {
    let ep = w.eps[n.ep];
    let blob = run(sp, n.off, n.len);
    if failed || res == 0 {
        if ep.dbe_n > 0 {
            begin(w, ep.dbe_status, close);
            render(w, ep, ep.dbe_off, ep.dbe_n, blob, 0);
            finish(w);
        } else { internal_error(w, close); }
        return;
    }
    let status = pq::status(res);
    if status == pq::TUPLES_OK || status == pq::COMMAND_OK {
        if ep.nr_status != 0 && status == pq::TUPLES_OK && pq::rows(res) == 0 {
            error(w, ep.nr_status, w.lit[ep.nr_off..ep.nr_off + ep.nr_len], close);
            return;
        }
        var code = ep.ok_status;
        if ep.aff_n >= 0 && pq::affected(res) == ep.aff_n as i64 { code = ep.aff_status; }
        begin(w, code, close);
        render(w, ep, ep.resp_off, ep.resp_n, blob, res);
        finish(w);
        return;
    }
    // Postgres refused the statement: the rule the OpenAPI file has for its SQLSTATE, else a 500
    w.text.clear();
    ffi::bytes(pq::error_field(res, 67), 5, w.text);
    for k in ep.err_n {
        let rule = w.errs[ep.err_off + k];
        if w.lit[rule.code_off..rule.code_off + 5] == w.text[..] {
            error(w, rule.status, w.lit[rule.msg_off..rule.msg_off + rule.msg_len], close);
            return;
        }
    }
    internal_error(w, close);
}

// Answers the request whose statement finished (or failed), then serves any request queued behind it.
fn respond(w: W&, sp: Pool&, e: Pending, res: pq::Res, failed: bool) {
    let c .= w.clients[e.client];
    if e.gen == c.gen && c.open {               // else the client left
        w.gzip_ok = e.gzip;
        match e.job {
            Ep n => answer(w, sp, n, res, failed, e.close),
            Sync => {},
        }
        c.busy = false;
        send_resp(w, sp, e.client, e.close);
        if c.open && !c.sending && !c.closing && c.in_len > 0 { process_carry(w, sp, e.client); }
    }
    release(sp, e.job);
}

// ---- reading the OpenAPI document ----

fn config_error(msg: const u8[:]) -> bool {
    write_stderr(str("openapi: ", msg, "\n"));
    return false;
}

// Keeps s in W.lit and says where it starts.
fn keep(w: W&, s: const u8[:]) -> i32 {
    let at = w.lit.len;
    w.lit.append(s);
    return at as i32;
}

fn kind_of(s: const u8[:]) -> i64 {
    if s == "id" { return T_ID; }
    if s == "int" { return T_INT; }
    return T_TEXT;
}

fn member_str(n: json::Node?, key: u8[:], fallback: const u8[:]) -> const u8[:] {
    let s, ok = json::as_string(json::member(n, key));
    return if ok { s } else { fallback };
}

fn member_bool(n: json::Node?, key: u8[:]) -> bool {
    let b, ok = json::as_bool(json::member(n, key));
    return ok && b;
}

fn member_int(n: json::Node?, key: u8[:], fallback: i64) -> i64 {
    let v, ok = json::as_i64(json::member(n, key));
    return if ok { v } else { fallback };
}

fn push_piece(w: W&, kind: i64, a: i64, b: i64) { w.pieces.push(Piece { kind: kind as u8, a: a as i32, b: b as i32 }); }

// Adds literal response text; start is the first piece of this response, so that text is only joined to its own.
fn emit_lit(w: W&, start: i64, bytes: const u8[:]) {
    let at = w.lit.len;
    w.lit.append(bytes);
    let last = w.pieces.len - 1;
    if last >= start && w.pieces[last].kind == P_LIT && (w.pieces[last].a + w.pieces[last].b) as i64 == at {
        w.pieces[last].b += bytes.len as i32;
        return;
    }
    push_piece(w, P_LIT, at, bytes.len);
}

fn trim_spaces(s: u8[:]) -> u8[:] {
    var a = 0;
    var e = s.len;
    while a < e && s[a] == ' ' { a++; }
    while e > a && s[e - 1] == ' ' { e--; }
    return s[a..e];
}

// {{name}} in a response: a value of the request, or a part of the result.
fn emit_placeholder(w: W&, slot_off: i64, slot_n: i64, start: i64, inner: u8[:]) -> bool {
    let key = trim_spaces(inner);
    if key == "rows" { push_piece(w, P_ROWS, 0, 0); return true; }
    if key == "row" { push_piece(w, P_ROW, 0, 0); return true; }
    if key == "affected" { push_piece(w, P_AFFECTED, 0, 0); return true; }
    if key == "uptime_s" { push_piece(w, P_UPTIME, 0, 0); return true; }
    if starts_with(key, "affected==") {
        let n, ok = parse_int(key[10..]);
        if !ok { return config_error(str("cannot read the number in {{", key, "}}")[..]); }
        push_piece(w, P_AFFECTED_IS, n, 0);
        return true;
    }
    if starts_with(key, "row.") { push_piece(w, P_COL, keep(w, key[4..]), key.len - 4); return true; }
    for k in slot_n {
        let s .= w.slots[slot_off + k];
        if w.lit[s.name_off..s.name_off + s.name_len] == key { push_piece(w, P_VALUE, k, 0); return true; }
    }
    return config_error(str("{{", key, "}} is not a value of this endpoint (path.x, query.x, body.x, jwt.x) or a part of the result (row, rows, row.column, affected)")[..]);
}

fn emit_key(w: W&, start: i64, key: u8[:]) {
    var q: u8[>..] = [];
    json::format_key(q, key);
    emit_lit(w, start, q[..]);
}

recursive fn emit_json(w: W&, slot_off: i64, slot_n: i64, start: i64, n: json::Node?) -> bool {
    guard n else { return true; }
    match n.val {
        Null => { emit_lit(w, start, "null"); },
        Bool b => { emit_lit(w, start, if b.v { "true" } else { "false" }); },
        Int i => { var t: u8[>..] = []; json::format_int(t, i.v); emit_lit(w, start, t[..]); },
        Num x => { var t: u8[>..] = []; json::format_number(t, x.v); emit_lit(w, start, t[..]); },
        Str t => {
            let s = t.s;
            if s.len >= 4 && starts_with(s, "{{") && s[s.len - 2..] == "}}" { return emit_placeholder(w, slot_off, slot_n, start, s[2..s.len - 2]); }
            var q: u8[>..] = [];
            json::format_string(q, s);
            emit_lit(w, start, q[..]);
        },
        Arr => {
            emit_lit(w, start, "[");
            var first = true;
            var ok = true;
            json::each(n) { c =>
                if !first { emit_lit(w, start, ","); }
                first = false;
                if !emit_json(w, slot_off, slot_n, start, c) { ok = false; }
            };
            emit_lit(w, start, "]");
            return ok;
        },
        Obj => {
            emit_lit(w, start, "{");
            var first = true;
            var ok = true;
            json::each(n) { c =>
                if !first { emit_lit(w, start, ","); }
                first = false;
                emit_key(w, start, c.key);
                if !emit_json(w, slot_off, slot_n, start, c) { ok = false; }
            };
            emit_lit(w, start, "}");
            return ok;
        },
    }
    return true;
}

// Adds the pieces of the response json to W.pieces. Returns false (after saying why) if it has a bad placeholder.
fn compile_template(w: W&, slot_off: i64, slot_n: i64, n: json::Node?) -> bool {
    return emit_json(w, slot_off, slot_n, w.pieces.len, n);
}

fn add_slot(w: W&, src: i64, name: const u8[:], kind: i64) -> i64 {
    let at = w.slots.len;
    w.slots.push(Slot { src: src as u8, kind: kind as u8, name_off: keep(w, name), name_len: name.len as i32, .. });
    return at;
}

// Where a value named name is among the endpoint's slots, or -1.
fn find_slot(w: W&, slot_off: i64, slot_n: i64, name: u8[:]) -> i64 {
    for k in slot_n {
        let s .= w.slots[slot_off + k];
        if w.lit[s.name_off..s.name_off + s.name_len] == name { return k; }
    }
    return -1;
}

// A reference like "#/components/parameters/PostId", followed to what it names (at most 8 hops). Names with a '/' or '~'
// in them are not supported. Anything that is not a reference is returned as it is.
fn deref(root: json::Node?, n: json::Node?) -> json::Node? {
    var cur = n;
    for hop in 8 {
        let r = json::member(cur, "$ref");
        guard r else { return cur; }
        let target, ok = json::as_string(r);
        if !ok || !starts_with(target, "#/") { return null; }
        var node = root;
        each_split(target[2..], '/') { part => node .= json::member(node, part); };
        cur .= node;
    }
    return null;
}

fn nonempty_array(n: json::Node?) -> bool {
    guard n else { return false; }
    match n.val {
        Arr a => { return a.count > 0; },
        _ => { return false; },
    }
}

fn in_array(arr: json::Node?, name: u8[:]) -> bool {
    var found = false;
    guard arr else { return false; }
    json::each(arr) { x =>
        let s, ok = json::as_string(x);
        if ok && s == name { found = true; }
    };
    return found;
}

// integer with a minimum of 1 or more is an id; any other integer is an int; the rest is text
fn schema_kind(root: json::Node?, schema0: json::Node?) -> i64 {
    let schema = deref(root, schema0);
    if member_str(schema, "type", "string") == "integer" { return if member_int(schema, "minimum", 0) >= 1 { T_ID } else { T_INT }; }
    return T_TEXT;
}

fn set_default(w: W&, at: i64, schema: json::Node?) {
    let dv = json::member(schema, "default");
    guard dv else { return; }
    var text: u8[>..] = [];
    let iv, is_int = json::as_i64(dv);
    if is_int { json::format_int(text, iv); } else { text.append(member_str(schema, "default", "")); }
    w.slots[at].has_default = true;
    w.slots[at].def_off = keep(w, text[..]);
    w.slots[at].def_len = text.len as i32;
}

// The parameter called name that is in "path" or in "query", declared on the path item or on the operation.
fn find_param(root: json::Node?, item: json::Node?, op: json::Node?, name: u8[:], where: u8[:]) -> json::Node? {
    var found: json::Node? = null;
    let lists = [json::member(item, "parameters"), json::member(op, "parameters")];
    for list in lists {
        if list {
            json::each(list) { p0 =>
                let p = deref(root, p0);
                if member_str(p, "name", "") == name && member_str(p, "in", "") == where { found .= p; }
            };
        }
    }
    return found;
}

fn upper_method(m: u8[:]) -> const u8[:] {
    if m == "get" { return "GET"; }
    if m == "post" { return "POST"; }
    if m == "put" { return "PUT"; }
    if m == "delete" { return "DELETE"; }
    if m == "patch" { return "PATCH"; }
    return "";
}

fn add_message(w: W&, at: i64, msg: const u8[:]) {
    w.slots[at].err_off = keep(w, msg);
    w.slots[at].err_len = msg.len as i32;
}

fn compile_operation(w: W&, root: json::Node?, path: u8[:], method: const u8[:], item: json::Node?, op: json::Node?) -> bool {
    var e = Endpoint { aff_n: -1, .. };
    let route = str(method, " ", path);
    e.method_off = keep(w, method);
    e.method_len = method.len as i32;
    e.slot_off = w.slots.len as i32;
    let slot_off = w.slots.len;
    var ok = true;

    // the token: the operation's own "security", else the document's
    var security = json::member(root, "security");
    let own = json::member(op, "security");
    if own { security .= own; }
    e.auth = nonempty_array(security);
    if e.auth {
        if w.claims.len == 0 { return config_error(str(route, " needs a token, but no security scheme has \"x-claims\"")[..]); }
        for k in w.claims.len {
            let c = w.claims[k];
            let at = add_slot(w, S_JWT, str("jwt.", w.lit[c.alias_off..c.alias_off + c.alias_len])[..], c.kind);
            w.slots[at].index = k as i32;
        }
    }

    // path parameters, in the order of the route's segments
    e.seg_off = w.segs.len as i32;
    if path.len > 1 {
        var index = 0;
        each_split(path[1..], '/') { part =>
            if part.len > 2 && part[0] == '{' && part[part.len - 1] == '}' {
                let name = part[1..part.len - 1];
                let decl = find_param(root, item, op, name, "path");
                if !decl { ok = config_error(str(route, ": {", name, "} is in the path but is not declared as a path parameter")[..]); }
                let at = add_slot(w, S_PATH, str("path.", name)[..], schema_kind(root, json::member(decl, "schema")));
                w.slots[at].index = index as i32;
                add_message(w, at, member_str(decl, "x-error", "invalid value"));
                w.segs.push(Seg { off: 0, len: 0, slot: (at - slot_off) as i32 });
            } else {
                w.segs.push(Seg { off: keep(w, part), len: part.len as i32, slot: -1 });
            }
            index++;
        };
    }
    e.seg_n = w.segs.len as i32 - e.seg_off;

    // query parameters
    let lists = [json::member(item, "parameters"), json::member(op, "parameters")];
    for list in lists {
        if list {
            json::each(list) { p0 =>
                let p = deref(root, p0);
                if member_str(p, "in", "") == "query" {
                    let name = member_str(p, "name", "");
                    let schema = deref(root, json::member(p, "schema"));
                    let at = add_slot(w, S_QUERY, str("query.", name)[..], schema_kind(root, schema));
                    w.slots[at].key_off = keep(w, name);
                    w.slots[at].key_len = name.len as i32;
                    add_message(w, at, member_str(p, "x-error", "invalid value"));
                    set_default(w, at, schema);
                    if !w.slots[at].has_default && !member_bool(p, "required") {
                        ok = config_error(str(route, ": the query parameter ", name, " needs a default or \"required\": true")[..]);
                    }
                }
            };
        }
    }

    // the body
    let rb = deref(root, json::member(op, "requestBody"));
    if rb {
        let malformed = member_str(rb, "x-malformed", "malformed JSON body");
        e.malformed_off = keep(w, malformed);
        e.malformed_len = malformed.len as i32;
        let schema = deref(root, json::member(json::member(json::member(rb, "content"), "application/json"), "schema"));
        let required = json::member(schema, "required");
        let props = json::member(schema, "properties");
        if props {
            json::each(props) { f0 =>
                let f = deref(root, f0);
                let at = add_slot(w, S_BODY, str("body.", f0.key)[..], schema_kind(root, f));
                w.slots[at].key_off = keep(w, f0.key);
                w.slots[at].key_len = f0.key.len as i32;
                w.slots[at].trim = member_bool(f, "x-trim");
                let is_required = in_array(required, f0.key);
                w.slots[at].required = is_required;
                if is_required {
                    let fallback = str(f0.key, " is required");
                    add_message(w, at, member_str(f, "x-required-error", fallback[..]));
                } else { add_message(w, at, ""); }
                w.slots[at].max_chars = member_int(f, "maxLength", 0) as i32;
                let too_long = member_str(f, "x-too-long-error", "value is too long");
                w.slots[at].max_off = keep(w, too_long);
                w.slots[at].max_len = too_long.len as i32;
                e.body_n += 1;
            };
        }
    }
    e.slot_n = w.slots.len as i32 - e.slot_off;
    let slot_n = e.slot_n as i64;
    if e.body_n > 8 { return config_error(str(route, ": at most 8 body properties")[..]); }

    // the statement and the values it takes
    let xs = json::member(op, "x-sql");
    let sql, sql_ok = json::as_string(json::member(xs, "query"));
    if !sql_ok { return config_error(str(route, " needs \"x-sql\": { \"query\": ... }")[..]); }
    e.write = member_bool(xs, "write");
    e.stmt = w.sql_off.len as i32;
    w.sql_off.push(w.sql.len as i32);
    w.sql.append(sql);
    w.sql.push(0);
    e.param_off = w.params.len as i32;
    let params = json::member(xs, "params");
    if params {
        json::each(params) { p =>
            let name, name_ok = json::as_string(p);
            let at = if name_ok { find_slot(w, slot_off, slot_n, name) } else { -1 };
            if at < 0 { ok = config_error(str(route, ": the parameter ", name, " is not one of its values (path.x, query.x, body.x, jwt.x)")[..]); }
            else { w.params.push(at as i32); }
        };
    }
    e.param_n = w.params.len as i32 - e.param_off;
    w.sql_np.push(e.param_n);
    if e.param_n > 32 { return config_error(str(route, ": at most 32 parameters")[..]); }

    // errors Postgres can give, and renamed columns
    let errors = json::member(xs, "errors");
    e.err_off = w.errs.len as i32;
    if errors {
        json::each(errors) { er =>
            if er.key.len != 5 { ok = config_error(str(route, ": \"", er.key, "\" is not a SQLSTATE (five characters, like 23503)")[..]); }
            let msg = member_str(er, "error", "internal server error");
            w.errs.push(ErrRule { code_off: keep(w, er.key), status: member_int(er, "status", 500) as i32, msg_off: keep(w, msg), msg_len: msg.len as i32 });
        };
    }
    e.err_n = w.errs.len as i32 - e.err_off;
    let cols = json::member(xs, "columns");
    e.ren_off = w.renames.len as i32;
    if cols {
        json::each(cols) { c =>
            let to = member_str(cols, c.key, c.key);
            w.renames.push(Rename { from_off: keep(w, c.key), from_len: c.key.len as i32, to_off: keep(w, to), to_len: to.len as i32 });
        };
    }
    e.ren_n = w.renames.len as i32 - e.ren_off;

    // the responses: the normal one (no "x-when"), one for a number of changed rows, one for no rows, one for no database
    var have_ok = false;
    let responses = json::member(op, "responses");
    if responses {
        json::each(responses) { r0 =>
            let r = deref(root, r0);
            let code, is_code = parse_int(r0.key);        // "default" and ranges like "4XX" only document
            let when = member_str(r, "x-when", "");
            if is_code {
                if when.len == 0 {
                    if code >= 200 && code < 300 && !have_ok {
                        have_ok = true;
                        e.ok_status = code as i32;
                        e.resp_off = w.pieces.len as i32;
                        if !compile_template(w, slot_off, slot_n, json::member(r, "x-template")) { ok = false; }
                        e.resp_n = w.pieces.len as i32 - e.resp_off;
                        if e.resp_n == 0 { ok = config_error(str(route, ": the ", code, " response needs an \"x-template\"")[..]); }
                    }
                } else if starts_with(when, "affected ==") {
                    let count, count_ok = parse_int(trim_spaces(when[11..]));
                    if !count_ok { ok = config_error(str(route, ": cannot read \"", when, "\"")[..]); }
                    e.aff_n = count as i32;
                    e.aff_status = code as i32;
                } else if when == "no_rows" {
                    let msg = member_str(r, "x-error", "not found");
                    e.nr_status = code as i32;
                    e.nr_off = keep(w, msg);
                    e.nr_len = msg.len as i32;
                } else if when == "db_error" {
                    e.dbe_status = code as i32;
                    e.dbe_off = w.pieces.len as i32;
                    if !compile_template(w, slot_off, slot_n, json::member(r, "x-template")) { ok = false; }
                    e.dbe_n = w.pieces.len as i32 - e.dbe_off;
                } else { ok = config_error(str(route, ": \"x-when\" is \"", when, "\", not one of \"affected == N\", \"no_rows\", \"db_error\"")[..]); }
            }
        };
    }
    if !have_ok { return config_error(str(route, " needs a 2xx response with an \"x-template\"")[..]); }
    w.eps.push(e);
    return ok;
}

fn compile_path_item(w: W&, root: json::Node?, path: u8[:], item: json::Node&) -> bool {
    var ok = true;
    if path.len == 0 || path[0] != '/' { return config_error(str("the path \"", path, "\" does not start with /")[..]); }
    json::each(item) { op =>
        let method = upper_method(op.key);
        if method.len > 0 && ok { ok = compile_operation(w, root, path, method, item, op); }
    };
    return ok;
}

// Reads the OpenAPI document at path into the tables above. False (after saying why) if it cannot be used.
fn load_config(w: W&, path: const u8[:]) -> bool {
    var text: u8[>..] = [];
    if !read_file(path, text) { return config_error(str("cannot read ", path)[..]); }
    var doc: json::Node[>..] = [];
    var decoded: u8[>..] = [];
    let root, err = json::parse(text[..], doc, decoded);
    guard root else { return config_error(err[..]); }
    if !starts_with(member_str(root, "openapi", ""), "3.") { return config_error("not an OpenAPI 3 document (\"openapi\": \"3.x\" is missing)"); }
    // the claims a token must carry: from the first security scheme that says (x-claims)
    let schemes = json::member(json::member(root, "components"), "securitySchemes");
    if schemes {
        json::each(schemes) { s =>
            let claims = json::member(s, "x-claims");
            if claims && w.claims.len == 0 {
                json::each(claims) { c =>
                    let name = member_str(c, "claim", c.key);
                    w.claims.push(Claim { name_off: keep(w, name), name_len: name.len as i32, alias_off: keep(w, c.key), alias_len: c.key.len as i32, kind: kind_of(member_str(c, "type", "text")) as u8 });
                };
            }
        };
    }
    let paths = json::member(root, "paths");
    guard paths else { return config_error("\"paths\" is missing"); }
    var ok = true;
    json::each(paths) { item0 =>
        let item = deref(root, item0);
        if ok && item { ok = compile_path_item(w, root, item0.key, item); }
    };
    if ok && w.eps.len == 0 { return config_error("\"paths\" has no operations (get, post, put, delete, patch)"); }
    return ok;
}

// ---- requests ----

// Whether the path (without its query string) is the endpoint's route.
fn route_matches(w: W&, e: Endpoint, path: u8[:]) -> bool {
    if path.len == 0 || path[0] != '/' { return false; }
    var at = 1;
    for k in e.seg_n {
        if at > path.len { return false; }
        let rest = path[at..];
        let slash = position(rest, '/');
        let seg = if slash < 0 { rest } else { rest[..slash] };
        let s = w.segs[e.seg_off + k];
        if s.slot < 0 { if seg != w.lit[s.off..s.off + s.len] { return false; } }
        else if seg.len == 0 { return false; }
        at += seg.len + 1;
    }
    return at == path.len + 1;
}

// The path's i-th segment.
fn segment(path: u8[:], i: i64) -> u8[:] {
    var rest = path[1..];
    for k in i {
        let slash = position(rest, '/');
        rest = rest[slash + 1..];
    }
    let slash = position(rest, '/');
    return if slash < 0 { rest } else { rest[..slash] };
}

// The value of name in a query string, if it has one.
fn query_value(qs: u8[:], name: u8[:]) -> u8[:], bool {
    var rest = qs;
    while rest.len > 0 {
        let amp = position(rest, '&');
        let pair = if amp < 0 { rest } else { rest[..amp] };
        rest = if amp < 0 { rest[rest.len..] } else { rest[amp + 1..] };
        let eq = position(pair, '=');
        if eq >= 0 && pair[..eq] == name { return pair[eq + 1..], true; }
    }
    return qs[0..0], false;
}

// Adds a value to the request's values (see Endpoint's slots).
fn put_value(ctx: u8[>..]&, v: u8[:]) {
    let n = v.len;
    ctx.push((n & 255) as u8);
    ctx.push(((n >> 8) & 255) as u8);
    ctx.push(((n >> 16) & 255) as u8);
    ctx.push(((n >> 24) & 255) as u8);
    ctx.append(v);
}

fn put_number(ctx: u8[>..]&, n: i64) {
    var digits: u8[>..] = [];
    format(digits, n);
    put_value(ctx, digits[..]);
}

// Sends the statement and marks the client busy; the response is built when the result arrives. Without a usable Postgres
// connection the endpoint's db_error (or a 500) is the answer at once.
fn start(w: W&, sp: Pool&, client: u32, close: bool, ei: u32, off: u32, len: u32, pbuf: u8[:], poffs: i32[:]) {
    let job = Job.Ep { ep: ei, off: off, len: len };
    if submit(w, sp, client, close, job, pbuf, poffs) { w.clients[client].busy = true; return; }
    answer(w, sp, Job.Ep { ep: ei, off: off, len: len }, 0, true, close);
    release(sp, job);
}

// Handles a request: finds its endpoint, checks the token and takes and checks its values in order (token, path, query
// string, body), and sends the statement. Anything wrong is answered here.
fn handle(w: W&, sp: Pool&, client: u32, req: http::Request) {
    var path = req.target;
    var qs = path[0..0];
    let q = position(path, '?');
    if q >= 0 { qs = path[q + 1..]; path = path[..q]; }
    var ei: i64 = -1;
    for k in w.eps.len {
        let e .= w.eps[k];
        if req.method == w.lit[e.method_off..e.method_off + e.method_len] && route_matches(w, e, path) { ei = k; break; }
    }
    if ei < 0 { error(w, 404, "not found", req.close); return; }
    let e = w.eps[ei];

    var ctx: u8[>..] = [];                  // the request's values: [4-byte length][bytes] for each of the endpoint's slots
    var text: u8[>..] = [];
    var payload: u8[>..] = [];
    var pbuf: u8[>..] = [];                 // the statement's parameters, NUL-terminated
    var poffs: i32[>..] = [];

    // the token: checked, and its claims taken, before anything else (they are the endpoint's first slots)
    if e.auth {
        let token, why = auth::verify(req.authorization, w.now, payload);
        if why.len > 0 { error(w, 401, why, req.close); return; }
        var claims: jsn::Want[8];
        var names: const u8[:][8];
        let nclaims = w.claims.len;
        for k in nclaims { let c = w.claims[k]; names[k] = w.lit[c.name_off..c.name_off + c.name_len]; }
        if !jsn::jdoc(token, names[..nclaims], claims[..nclaims]) { error(w, 401, auth::INVALID, req.close); return; }
        for k in nclaims {
            let s = w.slots[e.slot_off + k];
            let span = claims[s.index];
            if span.start < 0 || token[span.start] != '"' { error(w, 401, auth::BAD_PAYLOAD, req.close); return; }
            text.clear();
            var at = span.start;
            let n, ok = jsn::jstr(token[..span.end], at, text, 1024);
            if !ok || n > 1024 { error(w, 401, auth::BAD_PAYLOAD, req.close); return; }
            if s.kind == T_ID {
                var id: i64 = 0;
                if n == 0 || n > 15 { error(w, 401, auth::BAD_PAYLOAD, req.close); return; }
                for digit in text[..] {
                    if digit < '0' || digit > '9' { error(w, 401, auth::BAD_PAYLOAD, req.close); return; }
                    id = id * 10 + (digit - '0');
                }
                if id <= 0 { error(w, 401, auth::BAD_PAYLOAD, req.close); return; }
            }
            put_value(ctx, text[..]);
        }
    }

    // the rest of the values
    var body_found: jsn::Want[8];
    var body_parsed = false;
    let first_body = e.slot_n - e.body_n;
    for k in e.slot_n {
        let s = w.slots[e.slot_off + k];
        match s.src {
            S_JWT => {},                                    // taken above
            S_PATH => {
                let seg = segment(path, s.index);
                if s.kind == T_ID {
                    let id = http::parse_id(seg);
                    if id < 0 { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                    put_number(ctx, id);
                } else if s.kind == T_INT {
                    let n, ok = parse_int(seg);
                    if !ok { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                    put_number(ctx, n);
                } else { put_value(ctx, seg); }
            },
            S_QUERY => {
                let v, found = query_value(qs, w.lit[s.key_off..s.key_off + s.key_len]);
                if !found && !s.has_default { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                let value = if found { v } else { w.lit[s.def_off..s.def_off + s.def_len] };
                if s.kind == T_ID {
                    let id = http::parse_id(value);
                    if id < 0 { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                    put_number(ctx, id);
                } else if s.kind == T_INT {
                    let n, ok = parse_int(value);
                    if !ok { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                    put_number(ctx, n);
                } else { put_value(ctx, value); }
            },
            _ => {                                          // S_BODY
                if !body_parsed {
                    body_parsed = true;
                    var keys: const u8[:][8];
                    for b in e.body_n { let bs = w.slots[e.slot_off + first_body + b]; keys[b] = w.lit[bs.key_off..bs.key_off + bs.key_len]; }
                    if !jsn::jdoc(req.body, keys[..e.body_n], body_found[..e.body_n]) {
                        error(w, 400, w.lit[e.malformed_off..e.malformed_off + e.malformed_len], req.close);
                        return;
                    }
                }
                let span = body_found[k - first_body];
                if span.start < 0 && !s.required { put_value(ctx, req.body[0..0]); continue; }
                if span.start < 0 || (s.kind == T_TEXT && req.body[span.start] != '"') {
                    if s.err_len > 0 { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                    error(w, 400, w.lit[e.malformed_off..e.malformed_off + e.malformed_len], req.close);
                    return;
                }
                if s.kind == T_TEXT {
                            text.clear();
                            var at = span.start;
                    let n, ok = jsn::jstr(req.body[..span.end], at, text, http::MAX_REQUEST);
                    if !ok || n > http::MAX_REQUEST { error(w, 400, w.lit[e.malformed_off..e.malformed_off + e.malformed_len], req.close); return; }
                    let text = if s.trim { wire::jstrim(text[..]) } else { text[..] };
                    if text.len == 0 && s.required { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                    if s.max_chars > 0 {
                        var chars = 0;
                        for byte in text { if (byte & 0xc0) != 0x80 { chars++; } }        // characters, not bytes
                        if chars > s.max_chars { error(w, 400, w.lit[s.max_off..s.max_off + s.max_len], req.close); return; }
                    }
                    put_value(ctx, text);
                } else {
                    let n, ok = parse_int(req.body[span.start..span.end]);
                    if !ok { error(w, 400, w.lit[s.err_off..s.err_off + s.err_len], req.close); return; }
                    put_number(ctx, n);
                }
            },
        }
    }

    // the statement's parameters, as NUL-terminated text, and the values kept for the response
    pbuf.push(0);
    for j in e.param_n {
        poffs.push(pbuf.len as i32);
        pbuf.append(ctx_value(ctx[..], w.params[e.param_off + j] as i64));
        pbuf.push(0);
    }
    if e.param_n == 0 { poffs.push(0); }
    ctx.push(0);                                          // never empty
    let kept = sp.alloc_copy(ctx[..]);
    start(w, sp, client, req.close, ei as u32, sp.items.range_index(kept) as u32, ctx.len as u32, pbuf[..], poffs[..]);
}

// Serves the complete requests at the front of buf, until one waits for Postgres. Returns the bytes consumed, or -1
// when the connection must be dropped.
fn serve(w: W&, sp: Pool&, client: u32, buf: u8[:]) -> i64 {
    var used = 0;
    while used < buf.len {
        let c .= w.clients[client];
        if !c.open || c.busy || c.closing || c.sending { break; }
        let req, parsed = http::parse(buf[used..]);
        match parsed {
            Complete => {},
            Incomplete => { break; },
            ExpectContinue => {
                io::send(client, "HTTP/1.1 100 Continue\r\n\r\n"[0], 25, false);
                break;
            },
            Malformed => { return -1; },
        }
        used += req.size;
        w.gzip_ok = req.gzip;
        handle(w, sp, client, req);
        if !c.busy { send_resp(w, sp, client, req.close); }
    }
    return used;
}

// Serves data and carries over whatever is left of an incomplete request.
fn process(w: W&, sp: Pool&, client: u32, data: u8[:]) {
    let used = serve(w, sp, client, data);
    let c .= w.clients[client];
    if !c.open { return; }                      // closed while serving (Connection: close)
    if used < 0 { close_client(w, sp, client); return; }
    if !c.closing && used < data.len { add_carry(sp, c, data[used..]); }
}

// Serves the bytes carried over from earlier receives (nothing was in flight until now).
fn process_carry(w: W&, sp: Pool&, client: u32) {
    let c .= w.clients[client];
    let n = c.in_len;
    while RB.len < n { RB.push(0); }
    copy_into(RB[..n], run(sp, c.in_off, c.in_len));
    drop_carry(sp, c);
    process(w, sp, client, RB[..n]);
}

fn on_received(w: W&, sp: Pool&, client: u32, data: u8[:]) {
    let c .= w.clients[client];
    if c.in_len == 0 && !c.busy && !c.sending { process(w, sp, client, data); return; }       // the usual case
    // Bytes are already waiting, or the previous request is still in flight: these go behind them.
    if !add_carry(sp, c, data) { close_client(w, sp, client); return; }
    if !c.busy && !c.sending { process_carry(w, sp, client); }
}

// The response is out (res > 0), or the connection failed.
fn on_sent(w: W&, sp: Pool&, client: u32, res: i32) {
    let c .= w.clients[client];
    if res < 0 { close_client(w, sp, client); return; }
    c.sending = false;
    if c.closing { close_client(w, sp, client); return; }
    if c.in_len > 0 && !c.busy { process_carry(w, sp, client); }
}

// ---- the loop ----

fn dispatch(w: W&, sp: Pool&, ev: io::Event) {
    let id = ev.id as i64;
    if ev.kind == io::POLL {
        let p .= w.pgs[id];
        if p.c == 0 { return; }
        let ready = ev.res as u32;
        if (ready & (io::POLLIN | io::POLLERR | io::POLLHUP)) != 0 { pg_read(w, sp, id); }
        if p.c != 0 {
            if (ready & io::POLLOUT) != 0 { p.dirty = true; }                          // libpq can flush more now
            pg_watch(w, id);
        }
    } else if ev.kind == io::ACCEPT {
        while w.clients.len <= id { w.clients.push(Client { .. }); }
        let c .= w.clients[id];
        c = Client { gen: c.gen + 1, open: true, .. };
    } else if id < w.clients.len && w.clients[id].open {
        if ev.kind == io::RECV {
            if ev.res <= 0 { close_client(w, sp, id as u32); return; }
            let off = ev.off as i64;
            on_received(w, sp, id as u32, INBOX[off..off + (ev.res as i64)]);
        } else if ev.kind == io::SEND { on_sent(w, sp, id as u32, ev.res); }
    }
}

fn env_or(name: const u8[:], fallback: const u8[:]) -> u8[>..] {
    var v: u8[>..] = [];
    if !env(name, v) { v.append(fallback); }
    return v;
}

thread_fn worker(started: i64, npg: i64) {
    var sp = Pool {};
    var w = W { npg: npg, started: started, now: started, .. };
    INBOX.resize(INBOX_SIZE, 0);
    w.db_url.append(env_or("DATABASE_URL", ""));
    w.db_url.push(0);
    let config_path = env_or("API_CONFIG", "openapi.json");
    if !load_config(w, config_path[..]) { exit(1); }

    var host = env_or("HOST", "0.0.0.0");
    host.push(0);
    let port, ok = parse_int(env_or("PORT", "3000"));
    let extra, has_extra = parse_int(env_or("EXTRA_PORT", ""));
    let rc = io::start(host[0], port as i32, if has_extra { extra as i32 } else { -1 });
    if rc < 0 { write_stderr(str("cannot start the event loop: error ", -rc, "\n")); exit(1); }
    w.pgs.resize(npg, Pg { .. });
    for i in npg { pg_connect(w, i); }

    var busy = false;
    loop {
        // After a busy iteration let events pile up for a moment, so one wake-up serves many requests and one Sync covers
        // many reads; after an idle one, wake on the first event.
        var min_events: i32 = 1;
        var timeout_us: i64 = IDLE_US;
        if busy { min_events = BATCH_MIN; timeout_us = BATCH_US; }
        let n = io::wait(w.events[0], MAX_EVENTS, INBOX[0], INBOX.len, min_events, timeout_us);
        w.now = time() as i64;
        for k in n { dispatch(w, sp, w.events[k]); }
        for i in w.npg {                        // one Sync for this iteration's reads, then send everything
            let p .= w.pgs[i];
            if p.c == 0 || !p.dirty { continue; }
            if p.unsynced { pg_sync(w, i); }
            p.dirty = false;
            p.batch = 0;
            pg_flush(w, sp, i);
        }
        busy = n > 0;
    }
}

fn main() {
    var secret: u8[>..] = [];
    if !env("JWT_SECRET", secret) || env_or("DATABASE_URL", "").len == 0 {
        write_stderr("DATABASE_URL and JWT_SECRET are required\n");
        exit(1);
    }
    auth::set_secret(secret[..]);
    net::init();
    let threads = net::cpus() as i64;
    let npg = max(2, 64 / threads);             // about 64 in all; reads and writes need one each
    write_stderr(str(threads, " threads, ", npg, " Postgres connections each\n"));
    var ids: i64[>..] = [];
    for i in threads { ids.push(thread_spawn(worker, time() as i64, npg)); }
    for id in ids { thread_wait(id); }
}
