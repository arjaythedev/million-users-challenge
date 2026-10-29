//! Feed API (SPEC.md): one thread per core, each a current-thread tokio runtime with its own SO_REUSEPORT
//! listeners on PORT and EXTRA_PORT and its own pipelined tokio-postgres connections. Hand-written HTTP/1.1.
mod json;

use std::cell::{Cell, RefCell};
use std::error::Error;
use std::io::{self, ErrorKind::WouldBlock};
use std::net::{SocketAddr, ToSocketAddrs};
use std::rc::Rc;
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use hmac::{Hmac, Mac};
use libdeflater::{CompressionLvl, Compressor};
use sha2::Sha256;
use tokio::net::{TcpListener, TcpSocket, TcpStream};
use tokio::task::JoinHandle;
use tokio_postgres::types::{FromSql, Type};
use tokio_postgres::{Client, NoTls, Row, Statement, error::SqlState};

// Postgres connections in total, split evenly over the threads (at least one of each kind per thread): sized by the
// load, not by the core count, so fewer cores still get as many commits in flight. 128 in all (rule 5: at most 512).
const PG_READS: usize = 96;
const PG_WRITES: usize = 32; // each autocommit INSERT waits for its own WAL flush: writes need connections in flight
const PACK: u32 = 16; // reads in flight on one connection before the next one is used
const MAX_REQ: usize = 64 * 1024; // header + body cap per request
const GZIP_MIN: usize = 1024; // only the feed is worth compressing

const POST_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, \
    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
    FROM posts p JOIN users u ON u.id = p.user_id";

struct Cfg {
    db_url: String,
    key: Hmac<Sha256>,
    addrs: Vec<SocketAddr>,
    started: Instant,
    reads: usize, // Postgres connections per thread
    writes: usize,
}

struct Db {
    c: Client,
    one: Statement,
    feed: Statement,
    post: Statement,
    ins: Statement,
    like: Statement,
    inflight: Cell<u32>,
}

/// Counts a request in flight on its connection until dropped.
struct Busy(Rc<Db>);
impl Drop for Busy {
    fn drop(&mut self) {
        self.0.inflight.set(self.0.inflight.get() - 1);
    }
}

struct Ctx {
    cfg: Arc<Cfg>,
    dbs: Vec<RefCell<Option<Rc<Db>>>>, // None while that slot reconnects; reads first, then writes
}

/// Per-thread scratch: requests are parsed and responses built and written here without awaiting,
/// so an idle connection owns no buffer at all.
struct Bufs {
    inp: Vec<u8>,
    body: Vec<u8>,
    out: Vec<u8>,
    z: Compressor,
}

thread_local! {
    static BUFS: RefCell<Bufs> = RefCell::new(Bufs {
        inp: vec![0; MAX_REQ],
        body: Vec::with_capacity(16384),
        out: Vec::with_capacity(16384),
        z: Compressor::new(CompressionLvl::new(1).unwrap()),
    });
}

enum Kind {
    Health,
    Feed,
    Post(i64),
    Create(i64, String, String),
    Like(i64, i64),
    Fail(u16, &'static str),
}

struct Req {
    kind: Kind,
    gzip: bool,
    close: bool,
}

enum Parse {
    Done(Req, usize),
    More(bool), // true: headers are in and the client waits for "100 Continue"
    Bad,
}

fn main() {
    let env = |k: &str| std::env::var(k).ok().filter(|v| !v.is_empty());
    let host = env("HOST").unwrap_or_else(|| "127.0.0.1".into());
    let mut addrs = Vec::new();
    for port in [env("PORT").or(Some("3000".into())), env("EXTRA_PORT")].into_iter().flatten() {
        addrs.push((host.as_str(), port.parse::<u16>().expect("bad port")).to_socket_addrs().expect("bad HOST").next().unwrap());
    }
    let secret = env("JWT_SECRET").expect("JWT_SECRET must be set");
    let n = std::thread::available_parallelism().map_or(1, |n| n.get());
    let cfg = Arc::new(Cfg {
        reads: PG_READS.div_ceil(n),
        writes: PG_WRITES.div_ceil(n),
        db_url: env("DATABASE_URL").expect("DATABASE_URL must be set"),
        key: Hmac::new_from_slice(secret.as_bytes()).unwrap(),
        addrs,
        started: Instant::now(),
    });
    eprintln!("feed: {n} threads, {} + {} Postgres connections (reads + writes), listening on {:?}", n * cfg.reads, n * cfg.writes, cfg.addrs);
    let threads: Vec<_> = (0..n)
        .map(|_| {
            let cfg = cfg.clone();
            std::thread::spawn(move || worker(cfg))
        })
        .collect();
    for t in threads {
        let _ = t.join();
    }
}

fn die(what: &str, e: impl std::fmt::Display) -> ! {
    eprintln!("{what}: {e}");
    std::process::exit(1)
}

fn worker(cfg: Arc<Cfg>) {
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    tokio::task::LocalSet::new().block_on(&rt, async move {
        let (mut dbs, mut hs) = (Vec::new(), Vec::new());
        for _ in 0..cfg.reads + cfg.writes {
            let (db, h) = connect(&cfg.db_url).await.unwrap_or_else(|e| die("postgres", e));
            dbs.push(RefCell::new(Some(Rc::new(db))));
            hs.push(h);
        }
        let ctx = Rc::new(Ctx { cfg: cfg.clone(), dbs });
        for (i, h) in hs.into_iter().enumerate() {
            tokio::task::spawn_local(watch(ctx.clone(), i, h));
        }
        for a in &cfg.addrs {
            let s = if a.is_ipv4() { TcpSocket::new_v4() } else { TcpSocket::new_v6() }.unwrap();
            s.set_reuseaddr(true).unwrap();
            s.set_reuseport(true).unwrap();
            s.bind(*a).unwrap_or_else(|e| die("bind", e));
            tokio::task::spawn_local(accept(s.listen(65535).unwrap(), ctx.clone()));
        }
        // Under light load, pause the thread for a moment between turns so ready sockets pile up: one epoll_wait
        // and one Postgres round trip then serve several requests. A busy thread (a turn of 200 us or more) never waits.
        loop {
            let turn = Instant::now();
            tokio::task::yield_now().await; // every other ready task runs before this returns
            if turn.elapsed() < Duration::from_micros(200) {
                std::thread::sleep(Duration::from_millis(2));
            }
        }
    });
}

/// Connects and prepares; the handle finishes when the connection is gone.
async fn connect(url: &str) -> Result<(Db, JoinHandle<()>), tokio_postgres::Error> {
    let (c, conn) = tokio_postgres::connect(url, NoTls).await?;
    let h = tokio::task::spawn_local(async move {
        if let Err(e) = conn.await {
            eprintln!("postgres connection lost: {e}");
        }
    });
    Ok((Db {
        one: c.prepare("SELECT 1").await?,
        feed: c.prepare(&format!("{POST_SQL} ORDER BY p.created_at DESC, p.id DESC LIMIT 20")).await?,
        post: c.prepare(&format!("{POST_SQL} WHERE p.id = $1")).await?,
        ins: c.prepare("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at").await?,
        like: c.prepare("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING").await?,
        inflight: Cell::new(0),
        c,
    }, h))
}

/// Empties slot `i` when its connection ends, then retries once a second (3 s connect timeout) until it is back.
async fn watch(ctx: Rc<Ctx>, i: usize, mut h: JoinHandle<()>) {
    loop {
        let _ = h.await;
        *ctx.dbs[i].borrow_mut() = None;
        loop {
            let t = Instant::now();
            match tokio::time::timeout(Duration::from_secs(3), connect(&ctx.cfg.db_url)).await {
                Ok(Ok((db, nh))) => {
                    *ctx.dbs[i].borrow_mut() = Some(Rc::new(db));
                    h = nh;
                    break;
                }
                Ok(Err(e)) => eprintln!("postgres reconnect: {e}"),
                Err(_) => eprintln!("postgres reconnect: timed out"),
            }
            tokio::time::sleep_until((t + Duration::from_secs(1)).into()).await;
        }
    }
}

/// Reads: the first live connection with fewer than PACK requests in flight, so the queries of a batch share one
/// write and one read on a few connections. Writes, and reads once those are full: the least loaded, so commits
/// (one WAL flush each) run side by side. Writes have their own slots, so a commit never delays a read.
fn pick(ctx: &Ctx, write: bool) -> Option<Busy> {
    let n = ctx.dbs.len();
    let r = n - ctx.cfg.writes;
    let live = || ctx.dbs[if write { r..n } else { 0..r }].iter().filter_map(|d| d.borrow().clone().filter(|d| !d.c.is_closed()));
    let packed = if write { None } else { live().find(|d| d.inflight.get() < PACK) };
    let db = packed.or_else(|| live().min_by_key(|d| d.inflight.get()))?;
    db.inflight.set(db.inflight.get() + 1);
    Some(Busy(db))
}

async fn accept(l: TcpListener, ctx: Rc<Ctx>) {
    loop {
        match l.accept().await {
            Ok((s, _)) => {
                let _ = s.set_nodelay(true);
                tokio::task::spawn_local(conn(s, ctx.clone()));
            }
            Err(_) => tokio::time::sleep(Duration::from_millis(5)).await, // EMFILE and friends: back off
        }
    }
}

async fn conn(s: TcpStream, ctx: Rc<Ctx>) {
    let mut carry: Vec<u8> = Vec::new(); // bytes of a partial or pipelined request; empty almost always
    loop {
        let req = loop {
            if !carry.is_empty() {
                match parse(&carry, &ctx.cfg.key) {
                    Parse::Done(r, used) => {
                        carry.drain(..used);
                        break r;
                    }
                    Parse::Bad => return,
                    Parse::More(cont) => {
                        if carry.len() > MAX_REQ {
                            return;
                        }
                        if cont && Box::pin(send_all(&s, b"HTTP/1.1 100 Continue\r\n\r\n".to_vec())).await.is_err() {
                            return;
                        }
                    }
                }
            }
            // poll_read_ready keeps the waker in the socket's own slot: no 152-byte readiness future per idle task
            if std::future::poll_fn(|cx| s.poll_read_ready(cx)).await.is_err() {
                return;
            }
            let got = BUFS.with_borrow_mut(|b| match s.try_read(&mut b.inp) {
                Ok(0) => Err(()),
                Ok(n) if !carry.is_empty() => {
                    carry.extend_from_slice(&b.inp[..n]);
                    Ok(None)
                }
                Ok(n) => match parse(&b.inp[..n], &ctx.cfg.key) {
                    Parse::Done(r, used) => {
                        carry.extend_from_slice(&b.inp[used..n]);
                        Ok(Some(r))
                    }
                    Parse::More(_) => {
                        carry.extend_from_slice(&b.inp[..n]);
                        Ok(None)
                    }
                    Parse::Bad => Err(()),
                },
                Err(e) if e.kind() == WouldBlock => Ok(None),
                Err(_) => Err(()),
            });
            match got {
                Ok(Some(r)) => break r,
                Ok(None) => {}
                Err(()) => return,
            }
        };
        let close = req.close;
        // Every await but the idle read is boxed: an idle connection's task then holds only that state.
        let rest = match req.kind {
            Kind::Fail(st, msg) => write(&s, |b| fail(b, st, msg), req.gzip, close),
            _ => Box::pin(query(&ctx, &s, req)).await,
        };
        match rest {
            Ok(None) => {}
            Ok(Some(v)) => {
                if Box::pin(send_all(&s, v)).await.is_err() { // rare: the socket was full
                    return;
                }
            }
            Err(_) => return,
        }
        if close {
            return;
        }
        if carry.capacity() > 4096 && carry.is_empty() {
            carry = Vec::new();
        }
    }
}

async fn query(ctx: &Ctx, s: &TcpStream, req: Req) -> io::Result<Option<Vec<u8>>> {
    let (gz, cl) = (req.gzip, req.close);
    let Some(busy) = pick(ctx, matches!(req.kind, Kind::Create(..) | Kind::Like(..))) else {
        return write(s, |b| match req.kind {
            Kind::Health => {
                b.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}");
                503
            }
            _ => fail(b, 500, "internal server error"),
        }, gz, cl);
    };
    let db = &busy.0;
    match req.kind {
        Kind::Health => {
            let r = db.c.query_one(&db.one, &[]).await;
            let up = ctx.cfg.started.elapsed().as_secs();
            write(s, |b| match r {
                Ok(_) => {
                    b.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                    num(b, up as i64);
                    b.push(b'}');
                    200
                }
                Err(e) => {
                    b.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
                    json::esc(b, e.to_string().as_bytes());
                    b.extend_from_slice(b"\"}");
                    503
                }
            }, gz, cl)
        }
        Kind::Feed => {
            let r = db.c.query(&db.feed, &[]).await;
            write(s, |b| match r {
                Ok(rows) => {
                    b.extend_from_slice(b"{\"posts\":[");
                    for (i, row) in rows.iter().enumerate() {
                        if i > 0 {
                            b.push(b',');
                        }
                        post(b, row);
                    }
                    b.extend_from_slice(b"]}");
                    200
                }
                Err(e) => internal(b, e),
            }, gz, cl)
        }
        Kind::Post(id) => {
            let r = db.c.query_opt(&db.post, &[&id]).await;
            write(s, |b| match r {
                Ok(Some(row)) => {
                    b.extend_from_slice(b"{\"post\":");
                    post(b, &row);
                    b.push(b'}');
                    200
                }
                Ok(None) => fail(b, 404, "post not found"),
                Err(e) => internal(b, e),
            }, gz, cl)
        }
        Kind::Create(uid, name, text) => {
            let r = db.c.query_one(&db.ins, &[&uid, &text]).await;
            write(s, |b| match r {
                Ok(row) => {
                    b.extend_from_slice(b"{\"post\":{\"id\":");
                    num(b, row.get(0));
                    b.extend_from_slice(b",\"body\":\"");
                    json::esc(b, text.as_bytes());
                    b.extend_from_slice(b"\",\"created_at\":\"");
                    ts(b, row.get::<_, Ts>(1).0);
                    b.extend_from_slice(b"\",\"author\":\"");
                    json::esc(b, name.as_bytes());
                    b.extend_from_slice(b"\",\"like_count\":0}}");
                    201
                }
                Err(e) => internal(b, e),
            }, gz, cl)
        }
        Kind::Like(uid, pid) => {
            let r = db.c.execute(&db.like, &[&uid, &pid]).await;
            write(s, |b| match r {
                Ok(n) => {
                    b.extend_from_slice(b"{\"liked\":true,\"already_liked\":");
                    b.extend_from_slice(if n == 1 { b"false" } else { b"true" });
                    b.extend_from_slice(b",\"post_id\":");
                    num(b, pid);
                    b.push(b'}');
                    if n == 1 { 201 } else { 200 }
                }
                Err(e) if e.code() == Some(&SqlState::FOREIGN_KEY_VIOLATION) => fail(b, 404, "post not found"),
                Err(e) => internal(b, e),
            }, gz, cl)
        }
        Kind::Fail(..) => unreachable!(),
    }
}

fn post(b: &mut Vec<u8>, r: &Row) {
    b.extend_from_slice(b"{\"id\":");
    num(b, r.get(0));
    b.extend_from_slice(b",\"body\":\"");
    json::esc(b, r.get::<_, &str>(1).as_bytes());
    b.extend_from_slice(b"\",\"created_at\":\"");
    ts(b, r.get::<_, Ts>(2).0);
    b.extend_from_slice(b"\",\"author\":\"");
    json::esc(b, r.get::<_, &str>(3).as_bytes());
    b.extend_from_slice(b"\",\"like_count\":");
    num(b, r.get::<_, i32>(4) as i64);
    b.push(b'}');
}

fn fail(b: &mut Vec<u8>, status: u16, msg: &str) -> u16 {
    b.extend_from_slice(b"{\"error\":\"");
    b.extend_from_slice(msg.as_bytes());
    b.extend_from_slice(b"\"}");
    status
}

fn internal(b: &mut Vec<u8>, e: tokio_postgres::Error) -> u16 {
    eprintln!("query failed: {e}");
    fail(b, 500, "internal server error")
}

/// Builds the body with `f`, frames it (gzipped when asked and worth it) and writes as much as the socket
/// takes right now. Returns the unsent tail, if any.
fn write(s: &TcpStream, f: impl FnOnce(&mut Vec<u8>) -> u16, gzip: bool, close: bool) -> io::Result<Option<Vec<u8>>> {
    BUFS.with_borrow_mut(|b| {
        let Bufs { body, out, z, .. } = b;
        body.clear();
        out.clear();
        let status = f(body);
        out.extend_from_slice(match status {
            200 => b"HTTP/1.1 200 OK\r\n".as_slice(),
            201 => b"HTTP/1.1 201 Created\r\n",
            400 => b"HTTP/1.1 400 Bad Request\r\n",
            401 => b"HTTP/1.1 401 Unauthorized\r\n",
            404 => b"HTTP/1.1 404 Not Found\r\n",
            503 => b"HTTP/1.1 503 Service Unavailable\r\n",
            _ => b"HTTP/1.1 500 Internal Server Error\r\n",
        });
        if close {
            out.extend_from_slice(b"Connection: close\r\n");
        }
        out.extend_from_slice(b"Content-Type: application/json\r\n");
        let zipped = gzip && body.len() >= GZIP_MIN;
        if zipped {
            out.extend_from_slice(b"Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
        }
        out.extend_from_slice(b"Content-Length: ");
        if zipped {
            let head = out.len();
            out.resize(head + 32 + z.gzip_compress_bound(body.len()), 0);
            let n = z.gzip_compress(body, &mut out[head + 32..]).expect("gzip bound");
            let mut h = Vec::with_capacity(32);
            num(&mut h, n as i64);
            h.extend_from_slice(b"\r\n\r\n");
            out.copy_within(head + 32..head + 32 + n, head + h.len());
            out[head..head + h.len()].copy_from_slice(&h);
            out.truncate(head + h.len() + n);
        } else {
            num(out, body.len() as i64);
            out.extend_from_slice(b"\r\n\r\n");
            out.extend_from_slice(body);
        }
        let mut off = 0;
        while off < out.len() {
            match s.try_write(&out[off..]) {
                Ok(n) => off += n,
                Err(e) if e.kind() == WouldBlock => return Ok(Some(out[off..].to_vec())),
                Err(e) => return Err(e),
            }
        }
        Ok(None)
    })
}

async fn send_all(s: &TcpStream, v: Vec<u8>) -> io::Result<()> {
    let mut off = 0;
    while off < v.len() {
        std::future::poll_fn(|cx| s.poll_write_ready(cx)).await?;
        match s.try_write(&v[off..]) {
            Ok(n) => off += n,
            Err(e) if e.kind() == WouldBlock => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn num(b: &mut Vec<u8>, v: i64) {
    let mut t = [0u8; 20];
    let mut i = t.len();
    let mut u = v.unsigned_abs();
    loop {
        i -= 1;
        t[i] = b'0' + (u % 10) as u8;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    if v < 0 {
        b.push(b'-');
    }
    b.extend_from_slice(&t[i..]);
}

/// TIMESTAMPTZ in binary format: microseconds since 2000-01-01 UTC.
struct Ts(i64);
impl<'a> FromSql<'a> for Ts {
    fn from_sql(_: &Type, raw: &'a [u8]) -> Result<Self, Box<dyn Error + Sync + Send>> {
        Ok(Ts(i64::from_be_bytes(raw.try_into()?)))
    }
    fn accepts(t: &Type) -> bool {
        *t == Type::TIMESTAMPTZ
    }
}

/// `2025-12-31T23:59:19.409Z`: UTC, truncated to milliseconds.
fn ts(b: &mut Vec<u8>, pg_us: i64) {
    let ms = pg_us.div_euclid(1000) + 946_684_800_000;
    let (days, t) = (ms.div_euclid(86_400_000), ms.rem_euclid(86_400_000));
    // civil_from_days (H. Hinnant)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + (m <= 2) as i64;
    let mut p = |v: i64, w: usize, sep: u8| {
        let mut t = [b'0'; 4];
        let mut v = v;
        for i in (0..w).rev() {
            t[i] = b'0' + (v % 10) as u8;
            v /= 10;
        }
        b.extend_from_slice(&t[..w]);
        if sep != 0 {
            b.push(sep);
        }
    };
    p(y, 4, b'-');
    p(m, 2, b'-');
    p(d, 2, b'T');
    p(t / 3_600_000, 2, b':');
    p(t / 60_000 % 60, 2, b':');
    p(t / 1000 % 60, 2, b'.');
    p(t % 1000, 3, b'Z');
}

/// Parses one request at the start of buf. Auth and validation happen here, on the borrowed bytes,
/// so what crosses the database await is small and owned.
fn parse(buf: &[u8], key: &Hmac<Sha256>) -> Parse {
    let Some(he) = memchr::memmem::find(buf, b"\r\n\r\n") else {
        return if buf.len() > MAX_REQ { Parse::Bad } else { Parse::More(false) };
    };
    let head = &buf[..he + 2];
    let le = memchr::memchr(b'\r', head).unwrap();
    let mut parts = head[..le].split(|&c| c == b' ');
    let (Some(method), Some(target), Some(ver), None) = (parts.next(), parts.next(), parts.next(), parts.next()) else {
        return Parse::Bad;
    };
    let mut close = ver == b"HTTP/1.0";
    let (mut auth, mut clen, mut gzip, mut expect) = (None, 0usize, false, false);
    for line in head[le + 2..].split(|&c| c == b'\n') {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        let Some(c) = memchr::memchr(b':', line) else { continue };
        let (k, v) = (&line[..c], line[c + 1..].trim_ascii());
        if k.eq_ignore_ascii_case(b"content-length") {
            match std::str::from_utf8(v).ok().and_then(|v| v.parse().ok()) {
                Some(n) if n <= MAX_REQ && v.iter().all(u8::is_ascii_digit) => clen = n,
                _ => return Parse::Bad,
            }
        } else if k.eq_ignore_ascii_case(b"authorization") {
            auth = Some(v);
        } else if k.eq_ignore_ascii_case(b"accept-encoding") {
            gzip = v.windows(4).any(|w| w.eq_ignore_ascii_case(b"gzip"));
        } else if k.eq_ignore_ascii_case(b"connection") {
            if v.eq_ignore_ascii_case(b"close") {
                close = true;
            } else if v.eq_ignore_ascii_case(b"keep-alive") {
                close = false;
            }
        } else if k.eq_ignore_ascii_case(b"expect") {
            expect = v.eq_ignore_ascii_case(b"100-continue");
        } else if k.eq_ignore_ascii_case(b"transfer-encoding") {
            return Parse::Bad; // chunked uploads are not supported
        }
    }
    let bs = he + 4;
    if buf.len() < bs + clen {
        return Parse::More(expect && buf.len() == bs);
    }
    let body = &buf[bs..bs + clen];
    let path = target.split(|&c| c == b'?').next().unwrap();
    let kind = route(method, path, auth, body, key);
    Parse::Done(Req { kind, gzip, close }, bs + clen)
}

fn route(method: &[u8], path: &[u8], auth: Option<&[u8]>, body: &[u8], key: &Hmac<Sha256>) -> Kind {
    match (method, path) {
        (b"GET", b"/feed") => Kind::Feed,
        (b"GET", b"/health") => Kind::Health,
        (b"POST", b"/posts") => match user(auth, key) {
            Err(m) => Kind::Fail(401, m),
            Ok((uid, name)) => match post_body(body) {
                Ok(text) => Kind::Create(uid, name, text),
                Err(m) => Kind::Fail(400, m),
            },
        },
        (_, p) if p.starts_with(b"/posts/") => {
            let rest = &p[7..];
            match (method, memchr::memchr(b'/', rest)) {
                (b"GET", None) => post_id(rest).map_or(Kind::Fail(400, "invalid post id"), Kind::Post),
                (b"POST", Some(i)) if &rest[i..] == b"/like" => match user(auth, key) {
                    Err(m) => Kind::Fail(401, m),
                    Ok((uid, _)) => post_id(&rest[..i]).map_or(Kind::Fail(400, "invalid post id"), |id| Kind::Like(uid, id)),
                },
                _ => Kind::Fail(404, "not found"),
            }
        }
        _ => Kind::Fail(404, "not found"),
    }
}

/// A positive decimal integer. Ids past i64 (out of scope) map to i64::MAX, which is never found.
fn post_id(s: &[u8]) -> Option<i64> {
    if s.is_empty() || !s.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let v = s.iter().try_fold(0i64, |v, &c| v.checked_mul(10)?.checked_add((c - b'0') as i64)).unwrap_or(i64::MAX);
    (v > 0).then_some(v)
}

fn post_body(body: &[u8]) -> Result<String, &'static str> {
    let [v] = json::doc(body, [b"body"]).ok_or("malformed JSON body")?;
    let text = v.and_then(json::string).ok_or("body is required")?;
    // JavaScript's String#trim set: Unicode White_Space without U+0085, plus U+FEFF
    let t = text.trim_matches(|c: char| (c.is_whitespace() && c != '\u{85}') || c == '\u{feff}');
    if t.is_empty() {
        return Err("body is required");
    }
    if t.chars().count() > 500 {
        return Err("body must be at most 500 characters"); // code points, as Postgres char_length()
    }
    Ok(t.to_string())
}

fn b64url(s: &[u8]) -> Option<Vec<u8>> {
    if s.len() % 4 == 1 {
        return None;
    }
    let mut o = Vec::with_capacity(s.len() * 3 / 4);
    let (mut acc, mut bits) = (0u32, 0);
    for &c in s {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'-' => 62,
            b'_' => 63,
            _ => return None,
        };
        acc = acc << 6 | v as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            o.push((acc >> bits) as u8);
        }
    }
    Some(o)
}

/// Verifies `Authorization: Bearer <HS256 JWT>` and returns (user id, username).
fn user(h: Option<&[u8]>, key: &Hmac<Sha256>) -> Result<(i64, String), &'static str> {
    const BAD: &str = "invalid or expired token";
    const PAYLOAD: &str = "invalid token payload";
    let t = h.and_then(|h| h.strip_prefix(b"Bearer ")).ok_or("missing bearer token")?;
    let mut it = t.split(|&c| c == b'.');
    let (Some(h64), Some(p64), Some(s64), None) = (it.next(), it.next(), it.next(), it.next()) else { return Err(BAD) };
    let sig = b64url(s64).ok_or(BAD)?;
    let mut mac = key.clone();
    mac.update(&t[..h64.len() + 1 + p64.len()]);
    mac.verify_slice(&sig).map_err(|_| BAD)?;
    let hdr = b64url(h64).ok_or(BAD)?;
    let [alg] = json::doc(&hdr, [b"alg"]).ok_or(BAD)?;
    if alg.and_then(json::string).as_deref() != Some("HS256") {
        return Err(BAD);
    }
    let pl = b64url(p64).ok_or(BAD)?;
    let [exp, nbf, sub, name] = json::doc(&pl, [b"exp", b"nbf", b"sub", b"username"]).ok_or(BAD)?;
    let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_secs_f64();
    let numv = |v: &[u8]| if v[0] == b'-' || v[0].is_ascii_digit() { std::str::from_utf8(v).ok()?.parse::<f64>().ok() } else { None };
    // exp is required (rule 9: checked on every request); nbf is honored when present
    match exp.map(numv) {
        Some(Some(e)) if now < e => {}
        _ => return Err(BAD),
    }
    if let Some(n) = nbf {
        if !numv(n).is_some_and(|n| now >= n) {
            return Err(BAD);
        }
    }
    let uid = sub.and_then(json::string).filter(|s| s.len() <= 18).and_then(|s| post_id(s.as_bytes())).ok_or(PAYLOAD)?;
    let name = name.and_then(json::string).ok_or(PAYLOAD)?;
    Ok((uid, name))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn timestamps() {
        let mut b = Vec::new();
        ts(&mut b, 0);
        ts(&mut b, 820_540_759_409_999); // 2025-12-31T23:59:19.409999Z
        ts(&mut b, -1);
        assert_eq!(b, b"2000-01-01T00:00:00.000Z2025-12-31T23:59:19.409Z1999-12-31T23:59:59.999Z");
    }
    #[test]
    fn ids_and_bodies() {
        assert_eq!(post_id(b"42"), Some(42));
        for bad in [&b"0"[..], b"-1", b"abc", b"1.5", b""] {
            assert_eq!(post_id(bad), None);
        }
        assert_eq!(post_body(b"{\"body\":\"  hi \\u00a0\"}"), Ok("hi".into()));
        assert_eq!(post_body(b"{\"body\":\"   \"}"), Err("body is required"));
        assert_eq!(post_body(b"{bad"), Err("malformed JSON body"));
        assert_eq!(post_body(format!("{{\"body\":\"{}\"}}", "\u{e9}".repeat(500)).as_bytes()).map(|s| s.len()), Ok(1000));
    }
}
