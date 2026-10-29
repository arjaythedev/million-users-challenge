//! Feed API for the million users challenge (SPEC.md).
//!
//! Thread-per-core: every core gets a thread with its own current-thread tokio runtime, its own SO_REUSEPORT
//! listeners on both ports and its own Postgres connections, so threads share nothing. Each connection is a
//! small task that holds no buffers while idle: a million idle keep-alive connections cost the socket, a
//! task and an epoll registration each.

mod auth;
mod db;
mod http;
mod json;

use std::{
    cell::RefCell,
    env, io,
    net::{SocketAddr, ToSocketAddrs},
    rc::Rc,
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc,
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use hmac::{Hmac, Mac};
use sha2::Sha256;
use tokio::net::{TcpListener, TcpStream};

use http::{Request, Route};

struct Config {
    database_url: String,
    jwt_secret: String,
    addrs: Vec<SocketAddr>,
    threads: usize,
    reads_per_thread: usize,
    writes_per_thread: usize,
    render: http::Render,
    started: Instant,
}

/// Per thread.
struct Ctx {
    cfg: Arc<Config>,
    db: db::Pool,
    mac: Hmac<Sha256>,
}

fn env_num(key: &str, default: usize) -> usize {
    match env::var(key) {
        Ok(v) if !v.is_empty() => v.parse().unwrap_or_else(|_| panic!("{key} must be a number")),
        _ => default,
    }
}

fn main() {
    let threads = env_num("THREADS", std::thread::available_parallelism().map_or(1, |n| n.get()));
    let host = env::var("HOST").unwrap_or_else(|_| "127.0.0.1".into());
    let mut addrs = Vec::new();
    for key in ["PORT", "EXTRA_PORT"] {
        let Ok(port) = env::var(key) else { continue };
        if port.is_empty() {
            continue;
        }
        let port: u16 = port.parse().unwrap_or_else(|_| panic!("{key} must be a port number"));
        addrs.push((host.as_str(), port).to_socket_addrs().expect("HOST").next().expect("HOST resolves"));
    }
    if addrs.is_empty() {
        addrs.push((host.as_str(), 3000).to_socket_addrs().expect("HOST").next().expect("HOST resolves"));
    }
    // Totals across all threads (rule 5 allows 512); each thread gets an equal share, at least one of each.
    let per_thread = |total: usize| total.div_ceil(threads).max(1);
    let cfg = Arc::new(Config {
        database_url: env::var("DATABASE_URL").expect("DATABASE_URL must be set"),
        jwt_secret: env::var("JWT_SECRET").expect("JWT_SECRET must be set"),
        threads,
        reads_per_thread: per_thread(env_num("DB_READ_CONNS", 64)),
        writes_per_thread: per_thread(env_num("DB_WRITE_CONNS", 64)),
        render: http::Render {
            gzip_level: env_num("GZIP_LEVEL", 1) as i32,
            gzip_min: env_num("GZIP_MIN", 512),
        },
        addrs,
        started: Instant::now(),
    });
    assert!(
        cfg.threads * (cfg.reads_per_thread + cfg.writes_per_thread) <= 512,
        "more than 512 Postgres connections: lower DB_READ_CONNS / DB_WRITE_CONNS"
    );
    eprintln!(
        "feed-server: {} threads, listening on {:?}, postgres connections per thread: {} read + {} write, gzip level {} for bodies >= {} bytes",
        cfg.threads, cfg.addrs, cfg.reads_per_thread, cfg.writes_per_thread, cfg.render.gzip_level, cfg.render.gzip_min
    );

    let workers: Vec<_> = (0..threads)
        .map(|i| {
            let cfg = cfg.clone();
            std::thread::Builder::new().name(format!("worker-{i}")).spawn(move || worker(cfg)).expect("spawn worker")
        })
        .collect();
    for w in workers {
        // A worker only returns if it could not start; the process can't serve its share without it.
        if w.join().is_err() {
            std::process::exit(1);
        }
    }
}

fn worker(cfg: Arc<Config>) {
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio runtime");
    let local = tokio::task::LocalSet::new();
    local.block_on(&rt, async move {
        let db = db::Pool::start(cfg.database_url.as_str().into(), cfg.reads_per_thread, cfg.writes_per_thread).await;
        let mac = Hmac::<Sha256>::new_from_slice(cfg.jwt_secret.as_bytes()).expect("any key length works");
        let ctx = Rc::new(Ctx { cfg: cfg.clone(), db, mac });
        // Bind only once the database is connected, so the kernel never hands this thread a
        // connection it can't serve yet.
        for &addr in &cfg.addrs {
            let listener = listen(addr).unwrap_or_else(|e| panic!("listen on {addr}: {e}"));
            tokio::task::spawn_local(accept_loop(listener, ctx.clone()));
        }
        std::future::pending::<()>().await;
    });
}

fn listen(addr: SocketAddr) -> io::Result<TcpListener> {
    use socket2::{Domain, Protocol, Socket, Type};
    let s = Socket::new(Domain::for_address(addr), Type::STREAM, Some(Protocol::TCP))?;
    s.set_reuse_address(true)?;
    // Every thread listens on the same ports; the kernel spreads new connections over them.
    s.set_reuse_port(true)?;
    s.set_nonblocking(true)?;
    s.bind(&addr.into())?;
    // The kernel caps this at net.core.somaxconn (65535 on the box).
    s.listen(65535)?;
    TcpListener::from_std(s.into())
}

async fn accept_loop(listener: TcpListener, ctx: Rc<Ctx>) {
    loop {
        match listener.accept().await {
            Ok((stream, _)) => {
                let _ = stream.set_nodelay(true);
                tokio::task::spawn_local(connection(stream, ctx.clone()));
            }
            Err(e) => {
                // EMFILE/ENFILE/ENOMEM: back off briefly rather than spin.
                log_error(format_args!("accept: {e}"));
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        }
    }
}

thread_local! {
    /// Every read lands here first; only an incomplete request is copied into the connection's own buffer.
    static SCRATCH: RefCell<Vec<u8>> = RefCell::new(vec![0; http::MAX_REQUEST]);
}

enum Read {
    Again,
    Closed,
    Got(http::Parsed),
}

/// One keep-alive connection. Kept small on purpose: this future's size is paid by every idle connection,
/// so the request handling (which awaits Postgres) lives in its own boxed future.
async fn connection(stream: TcpStream, ctx: Rc<Ctx>) {
    let mut pending: Vec<u8> = Vec::new();
    let mut sent_continue = false;
    loop {
        if stream.readable().await.is_err() {
            return;
        }
        let now = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0.0, |d| d.as_secs_f64());
        let auth = http::AuthCtx { mac: &ctx.mac, now };
        let read = SCRATCH.with_borrow_mut(|buf| {
            let n = match stream.try_read(buf) {
                Ok(0) => return Read::Closed,
                Ok(n) => n,
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => return Read::Again,
                Err(_) => return Read::Closed,
            };
            if pending.is_empty() {
                let parsed = http::parse_all(&buf[..n], &auth);
                pending.extend_from_slice(&buf[parsed.consumed..n]);
                Read::Got(parsed)
            } else {
                pending.extend_from_slice(&buf[..n]);
                let parsed = http::parse_all(&pending, &auth);
                pending.drain(..parsed.consumed);
                Read::Got(parsed)
            }
        });
        let parsed = match read {
            Read::Again => continue,
            Read::Closed => return,
            Read::Got(p) => p,
        };
        if pending.is_empty() {
            pending = Vec::new(); // give the memory back while idle
        }

        let mut close = false;
        if !parsed.requests.is_empty() {
            sent_continue = false;
            let mut out = Vec::new();
            for req in parsed.requests {
                close |= req.meta.close;
                out.extend_from_slice(&Box::pin(respond(req, &ctx)).await);
            }
            if write_all(&stream, &out).await.is_err() {
                return;
            }
        }
        if let Some(status) = parsed.error {
            let body = json_error(if status == 413 { "request too large" } else { "bad request" });
            let _ = write_all(&stream, &http::render(status, &body, None, &ctx.cfg.render)).await;
            return;
        }
        if close {
            return;
        }
        if parsed.expects_continue && !sent_continue {
            sent_continue = true;
            if write_all(&stream, b"HTTP/1.1 100 Continue\r\n\r\n").await.is_err() {
                return;
            }
        }
    }
}

async fn write_all(stream: &TcpStream, mut buf: &[u8]) -> io::Result<()> {
    while !buf.is_empty() {
        match stream.try_write(buf) {
            Ok(n) => buf = &buf[n..],
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => stream.writable().await?,
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

async fn respond(req: Request, ctx: &Ctx) -> Vec<u8> {
    let (status, body) = handle(req.route, ctx).await;
    let body = if status == 500 { json_error("internal server error") } else { body };
    http::render(status, &body, Some(&req.meta), &ctx.cfg.render)
}

fn json_error(msg: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(16 + msg.len());
    v.extend_from_slice(b"{\"error\":");
    json::string(&mut v, msg);
    v.push(b'}');
    v
}

static ERRORS_LOGGED: AtomicU64 = AtomicU64::new(0);

/// stderr, but at most 100 lines and then every 10,000th, so a sick database can't flood the journal.
fn log_error(msg: std::fmt::Arguments) {
    let n = ERRORS_LOGGED.fetch_add(1, Ordering::Relaxed);
    if n < 100 || n % 10_000 == 0 {
        eprintln!("error #{}: {msg}", n + 1);
    }
}

const NOT_FOUND: &str = "not found";
const INVALID_ID: &str = "invalid post id";
const POST_NOT_FOUND: &str = "post not found";

async fn handle(route: Route, ctx: &Ctx) -> (u16, Vec<u8>) {
    let fail = |e: db::DbError| {
        log_error(format_args!("{e}"));
        (500, Vec::new())
    };
    match route {
        Route::NotFound => (404, json_error(NOT_FOUND)),
        Route::Health => match ctx.db.health().await {
            Ok(()) => {
                let mut b = b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":".to_vec();
                json::int(&mut b, ctx.cfg.started.elapsed().as_secs());
                b.push(b'}');
                (200, b)
            }
            Err(e) => {
                let mut b = b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":".to_vec();
                json::string(&mut b, &e.to_string());
                b.push(b'}');
                (503, b)
            }
        },
        Route::Feed => match ctx.db.feed().await {
            Ok(rows) => {
                let mut b = Vec::with_capacity(6 * 1024);
                b.extend_from_slice(b"{\"posts\":[");
                for (i, row) in rows.iter().enumerate() {
                    if i > 0 {
                        b.push(b',');
                    }
                    if let Err(e) = post_json(&mut b, row) {
                        return fail(e.into());
                    }
                }
                b.extend_from_slice(b"]}");
                (200, b)
            }
            Err(e) => fail(e),
        },
        Route::Post(None) => (400, json_error(INVALID_ID)),
        Route::Post(Some(id)) => match ctx.db.post(id).await {
            Ok(Some(row)) => {
                let mut b = Vec::with_capacity(1024);
                b.extend_from_slice(b"{\"post\":");
                if let Err(e) = post_json(&mut b, &row) {
                    return fail(e.into());
                }
                b.push(b'}');
                (200, b)
            }
            Ok(None) => (404, json_error(POST_NOT_FOUND)),
            Err(e) => fail(e),
        },
        Route::Create { user, body } => {
            let user = match user {
                Ok(u) => u,
                Err(msg) => return (401, json_error(msg)),
            };
            let text = match new_post_body(&body) {
                Ok(t) => t,
                Err(msg) => return (400, json_error(msg)),
            };
            match ctx.db.insert_post(user.id, &text).await {
                Ok((id, created)) => {
                    let mut b = Vec::with_capacity(text.len() + 160);
                    b.extend_from_slice(b"{\"post\":");
                    json::post(&mut b, id, &text, created, &user.username, 0);
                    b.push(b'}');
                    (201, b)
                }
                Err(e) => fail(e),
            }
        }
        Route::Like { user, id } => {
            let user = match user {
                Ok(u) => u,
                Err(msg) => return (401, json_error(msg)),
            };
            let Some(id) = id else { return (400, json_error(INVALID_ID)) };
            match ctx.db.insert_like(user.id, id).await {
                Ok(new) => {
                    let mut b = Vec::with_capacity(64);
                    b.extend_from_slice(if new {
                        b"{\"liked\":true,\"already_liked\":false,\"post_id\":"
                    } else {
                        b"{\"liked\":true,\"already_liked\":true,\"post_id\":"
                    });
                    json::int(&mut b, id);
                    b.push(b'}');
                    (if new { 201 } else { 200 }, b)
                }
                Err(e) if e.code() == Some(&db::SqlState::FOREIGN_KEY_VIOLATION) => (404, json_error(POST_NOT_FOUND)),
                Err(e) => fail(e),
            }
        }
    }
}

fn post_json(b: &mut Vec<u8>, row: &tokio_postgres::Row) -> Result<(), tokio_postgres::Error> {
    json::post(
        b,
        row.try_get(0)?,
        row.try_get(1)?,
        row.try_get::<_, db::Timestamp>(2)?.0,
        row.try_get(3)?,
        row.try_get(4)?,
    );
    Ok(())
}

/// The `body` of a POST /posts request, trimmed like JavaScript's String.prototype.trim.
fn new_post_body(raw: &[u8]) -> Result<String, &'static str> {
    let v: serde_json::Value = serde_json::from_slice(raw).map_err(|_| "malformed JSON body")?;
    let text = v.get("body").and_then(serde_json::Value::as_str).ok_or("body is required")?;
    let text = text.trim_matches(js_whitespace);
    if text.is_empty() {
        return Err("body is required");
    }
    // Characters = code points, like Postgres char_length() in the CHECK constraint.
    if text.chars().count() > 500 {
        return Err("body must be at most 500 characters");
    }
    Ok(text.to_owned())
}

/// ECMAScript WhiteSpace and LineTerminator.
fn js_whitespace(c: char) -> bool {
    matches!(
        c,
        '\u{9}'..='\u{D}' | ' ' | '\u{A0}' | '\u{1680}' | '\u{2000}'..='\u{200A}' | '\u{2028}' | '\u{2029}' | '\u{202F}'
            | '\u{205F}' | '\u{3000}' | '\u{FEFF}'
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn post_body() {
        assert_eq!(new_post_body(br#"{"body":"  hi \n"}"#), Ok("hi".into()));
        assert_eq!(new_post_body(br#"{"body":"\u00a0x\ufeff"}"#), Ok("x".into()));
        assert_eq!(new_post_body(br#"{"body":""}"#), Err("body is required"));
        assert_eq!(new_post_body(br#"{"body":123}"#), Err("body is required"));
        assert_eq!(new_post_body(br#"[]"#), Err("body is required"));
        assert_eq!(new_post_body(b""), Err("malformed JSON body"));
        assert_eq!(new_post_body(br#"{bad json"#), Err("malformed JSON body"));
        let long = format!(r#"{{"body":"{}"}}"#, "é".repeat(501));
        assert_eq!(new_post_body(long.as_bytes()), Err("body must be at most 500 characters"));
        let ok = format!(r#"{{"body":"{}"}}"#, "é".repeat(500));
        assert!(new_post_body(ok.as_bytes()).is_ok());
    }
}
