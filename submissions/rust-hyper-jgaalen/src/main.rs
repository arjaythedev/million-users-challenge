#[cfg(feature = "gzip")]
mod compression;
#[cfg(feature = "custom-http")]
mod custom_http;
#[cfg(feature = "fixed-json")]
mod fixed_json;
#[cfg(feature = "custom-pg")]
mod wire;
#[cfg(not(feature = "custom-pg"))]
use tokio_postgres as pg;
#[cfg(feature = "custom-pg")]
use wire as pg;
mod auth;
#[cfg(all(feature = "shared-idle", target_os = "linux"))]
mod idle;
mod writes;

use bytes::Bytes;
use chrono::{DateTime, Datelike, SecondsFormat, Timelike, Utc};
use http_body_util::{BodyExt, Full, Limited};
use hyper::{Request, Response};
#[cfg(not(feature = "custom-http"))]
use hyper::{server::conn::http1, service::service_fn};
#[cfg(not(feature = "custom-http"))]
use hyper_util::rt::TokioIo;
use pg::{Client, NoTls, Row, Statement};
#[cfg(not(feature = "fixed-json"))]
use serde::ser::SerializeSeq;
use serde::{Serialize, Serializer};
use socket2::{Domain, Protocol, Socket, Type};
use std::{
    cell::Cell,
    convert::Infallible,
    env,
    net::ToSocketAddrs,
    rc::Rc,
    sync::Arc,
    time::{Duration, Instant},
};
use tokio::{
    net::TcpListener,
    sync::{Semaphore, mpsc},
};

#[cfg(not(all(feature = "shared-idle", target_os = "linux")))]
use tokio::net::TcpStream;

type Reply = Response<Full<Bytes>>;
const SELECT: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id";

struct Db {
    #[cfg(feature = "least-loaded-db")]
    pending: Cell<usize>,
    client: Client,
    feed: Statement,
    post: Statement,
}
// Bound head-of-line amplification: a slow connection must not collect every
// round-robin turn while the other connections have already drained.
#[cfg(feature = "least-loaded-db")]
fn least_loaded(n: usize, start: usize, pending: impl Fn(usize) -> usize) -> usize {
    let mut best = start;
    let mut load = usize::MAX;
    for distance in 0..n {
        let i = (start + distance) % n;
        let p = pending(i);
        if p < load {
            best = i;
            load = p;
        }
        if p == 0 {
            break;
        }
    }
    best
}
#[cfg(feature = "least-loaded-db")]
struct PendingRead<'a>(&'a Cell<usize>);
#[cfg(feature = "least-loaded-db")]
impl Drop for PendingRead<'_> {
    fn drop(&mut self) {
        self.0.set(self.0.get() - 1);
    }
}
#[derive(Default)]
struct RouteMetrics {
    counts: [Cell<u64>; 2],
    queue_us: [Cell<u64>; 2],
    operation_us: [Cell<u64>; 2],
    max_queue_us: [Cell<u64>; 2],
    max_operation_us: [Cell<u64>; 2],
}
struct OperationTimer<'a> {
    app: &'a App,
    kind: usize,
    start: Instant,
}
impl Drop for OperationTimer<'_> {
    fn drop(&mut self) {
        if !self.app.diagnostics {
            return;
        }
        let us = self.start.elapsed().as_micros() as u64;
        let m = &self.app.route_metrics;
        m.counts[self.kind].set(m.counts[self.kind].get() + 1);
        m.operation_us[self.kind].set(m.operation_us[self.kind].get() + us);
        m.max_operation_us[self.kind].set(m.max_operation_us[self.kind].get().max(us));
    }
}
struct App {
    worker: usize,
    diagnostics: bool,
    route_metrics: RouteMetrics,
    completed: Cell<u64>,
    queue_timeouts: Cell<u64>,
    db_timeouts: Cell<u64>,
    writer: writes::Writer,
    db: Vec<Db>,
    next: Cell<usize>,
    permits: Semaphore,
    #[cfg(feature = "isolated-writes")]
    write_permits: Semaphore,
    secret: Vec<u8>,
    started: Instant,
}
#[derive(Serialize)]
struct Post {
    id: i64,
    body: String,
    created_at: String,
    author: String,
    like_count: i32,
}
#[derive(Serialize)]
struct One {
    post: Post,
}
#[cfg(not(feature = "fixed-json"))]
#[derive(Serialize)]
struct Feed<'a> {
    posts: Posts<'a>,
}
#[cfg(not(feature = "fixed-json"))]
struct Posts<'a>(&'a [Row]);
#[cfg(not(feature = "fixed-json"))]
impl Serialize for Posts<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut seq = serializer.serialize_seq(Some(self.0.len()))?;
        for row in self.0 {
            seq.serialize_element(&post_view(row))?;
        }
        seq.end()
    }
}
#[derive(Serialize)]
struct PostView<'a> {
    id: i64,
    body: &'a str,
    created_at: Timestamp,
    author: &'a str,
    like_count: i32,
}
#[cfg(any(not(feature = "fixed-json"), test))]
#[derive(Serialize)]
struct OneView<'a> {
    post: PostView<'a>,
}
struct Timestamp(DateTime<Utc>);
impl Serialize for Timestamp {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let t = self.0;
        let year = t.year();
        if !(0..=9999).contains(&year) {
            return serializer.serialize_str(&timestamp(t));
        }
        let mut out = *b"0000-00-00T00:00:00.000Z";
        fn digits(out: &mut [u8], mut n: u32) {
            for v in out.iter_mut().rev() {
                *v = b'0' + (n % 10) as u8;
                n /= 10;
            }
        }
        digits(&mut out[0..4], year as u32);
        digits(&mut out[5..7], t.month());
        digits(&mut out[8..10], t.day());
        digits(&mut out[11..13], t.hour());
        digits(&mut out[14..16], t.minute());
        // Preserve chrono's leap-second formatting through the general path.
        if t.nanosecond() >= 1_000_000_000 {
            return serializer.serialize_str(&timestamp(t));
        }
        digits(&mut out[17..19], t.second());
        digits(&mut out[20..23], t.nanosecond() / 1_000_000);
        serializer.serialize_str(std::str::from_utf8(&out).expect("ASCII timestamp"))
    }
}
fn post_view(row: &Row) -> PostView<'_> {
    PostView {
        id: row.get(0),
        body: row.get(1),
        created_at: Timestamp(row.get(2)),
        author: row.get(3),
        like_count: row.get(4),
    }
}
#[derive(Serialize)]
struct Like {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}
#[derive(Serialize)]
struct Health<'a> {
    status: &'a str,
    db: &'a str,
    uptime_s: u64,
}
#[derive(Serialize)]
struct Degraded {
    status: &'static str,
    db: &'static str,
    error: String,
}

fn json(status: u16, value: &impl Serialize) -> Reply {
    json_sized(status, value, 512)
}
fn json_sized(status: u16, value: &impl Serialize, capacity: usize) -> Reply {
    #[cfg(feature = "json-prealloc")]
    let body = {
        // Capacity is an allocation hint, not a size limit or cached response.
        let mut bytes = Vec::with_capacity(capacity);
        serde_json::to_writer(&mut bytes, value).expect("JSON serialization");
        bytes
    };
    #[cfg(not(feature = "json-prealloc"))]
    let body = {
        let _ = capacity;
        serde_json::to_vec(value).expect("JSON serialization")
    };
    json_bytes(status, body)
}
fn json_bytes(status: u16, body: Vec<u8>) -> Reply {
    Response::builder()
        .status(status)
        .header("content-type", "application/json")
        .body(Full::new(Bytes::from(body)))
        .unwrap()
}
fn error(status: u16, message: &str) -> Reply {
    json(status, &serde_json::json!({"error":message}))
}
fn timestamp(t: DateTime<Utc>) -> String {
    t.to_rfc3339_opts(SecondsFormat::Millis, true)
}

async fn handle<B>(req: Request<B>, app: Rc<App>) -> Result<Reply, Infallible>
where
    B: hyper::body::Body<Data = Bytes> + Unpin,
    B::Error: Into<Box<dyn std::error::Error + Send + Sync>>,
{
    let health = req.method() == "GET" && req.uri().path() == "/health";
    let stage = Cell::new("body");
    let result = tokio::time::timeout(Duration::from_secs(5), route(req, &app, &stage)).await;
    app.completed.set(app.completed.get() + 1);
    if result.is_err() {
        let counter = if stage.get() == "queue" {
            &app.queue_timeouts
        } else {
            &app.db_timeouts
        };
        counter.set(counter.get() + 1);
    }
    Ok(match result {
        Ok(Ok(reply)) => reply,
        Ok(Err(err)) => {
            eprintln!("request failed: {err}");
            error(500, "internal server error")
        }
        Err(_) if health => json(
            503,
            &Degraded {
                status: "degraded",
                db: "unreachable",
                error: "database deadline exceeded".into(),
            },
        ),
        Err(_) => error(500, "internal server error"),
    })
}

async fn route<B>(req: Request<B>, app: &App, stage: &Cell<&str>) -> Result<Reply, pg::Error>
where
    B: hyper::body::Body<Data = Bytes> + Unpin,
    B::Error: Into<Box<dyn std::error::Error + Send + Sync>>,
{
    let path = req.uri().path();
    let method = req.method().as_str();
    let mut path_parts = path.split('/');
    let pieces = [
        path_parts.next(),
        path_parts.next(),
        path_parts.next(),
        path_parts.next(),
        path_parts.next(),
    ];
    let get_post =
        method == "GET" && pieces[1] == Some("posts") && pieces[2].is_some() && pieces[3].is_none();
    let like = method == "POST"
        && pieces[1] == Some("posts")
        && pieces[2].is_some()
        && pieces[3] == Some("like")
        && pieces[4].is_none();
    let create = method == "POST" && path == "/posts";
    let health = method == "GET" && path == "/health";
    let feed = method == "GET" && path == "/feed";
    if !(get_post || like || create || health || feed) {
        return Ok(error(404, "not found"));
    }
    let user = if create || like {
        match auth::verify(
            req.headers()
                .get("authorization")
                .and_then(|v| v.to_str().ok()),
            &app.secret,
        ) {
            Ok(u) => Some(u),
            Err(e) => return Ok(error(401, e)),
        }
    } else {
        None
    };
    let id = if get_post || like {
        match auth::positive_id(pieces[2].unwrap()) {
            Some(v) => v,
            None => return Ok(error(400, "invalid post id")),
        }
    } else {
        0
    };
    let body = if create {
        let collected = match Limited::new(req.into_body(), 16 * 1024).collect().await {
            Ok(b) => b.to_bytes(),
            Err(_) => return Ok(error(400, "malformed JSON body")),
        };
        let value: serde_json::Value = match serde_json::from_slice(&collected) {
            Ok(v) => v,
            Err(_) => return Ok(error(400, "malformed JSON body")),
        };
        let body = value
            .get("body")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .trim();
        if body.is_empty() {
            return Ok(error(400, "body is required"));
        }
        if body.chars().count() > 500 {
            return Ok(error(400, "body must be at most 500 characters"));
        }
        body.to_owned()
    } else {
        String::new()
    };
    stage.set("queue");
    let queue_started = Instant::now();
    #[cfg(feature = "isolated-writes")]
    let admission = if health || feed || get_post {
        &app.permits
    } else {
        &app.write_permits
    };
    #[cfg(not(feature = "isolated-writes"))]
    let admission = &app.permits;
    let _permit = admission.acquire().await.expect("open semaphore");
    let kind = usize::from(!(health || feed || get_post));
    if app.diagnostics {
        let us = queue_started.elapsed().as_micros() as u64;
        let m = &app.route_metrics;
        m.queue_us[kind].set(m.queue_us[kind].get() + us);
        m.max_queue_us[kind].set(m.max_queue_us[kind].get().max(us));
    }
    let _operation_timer = OperationTimer {
        app,
        kind,
        start: Instant::now(),
    };
    stage.set("database");
    #[cfg(feature = "least-loaded-db")]
    let n = least_loaded(app.db.len(), app.next.get(), |i| app.db[i].pending.get());
    #[cfg(not(feature = "least-loaded-db"))]
    let n = app.next.get();
    app.next.set((n + 1) % app.db.len());
    let db = &app.db[n];
    #[cfg(feature = "least-loaded-db")]
    let _pending = if health || feed || get_post {
        db.pending.set(db.pending.get() + 1);
        Some(PendingRead(&db.pending))
    } else {
        None
    };
    if health {
        return Ok(match db.client.simple_query("SELECT 1").await {
            Ok(_) => json(
                200,
                &Health {
                    status: "ok",
                    db: "ok",
                    uptime_s: app.started.elapsed().as_secs(),
                },
            ),
            Err(e) => json(
                503,
                &Degraded {
                    status: "degraded",
                    db: "unreachable",
                    error: e.to_string(),
                },
            ),
        });
    }
    if feed {
        let posts = db.client.query(&db.feed, &[]).await?;
        #[cfg(feature = "fixed-json")]
        return Ok(json_bytes(
            200,
            fixed_json::feed(posts.iter().map(post_view)),
        ));
        #[cfg(not(feature = "fixed-json"))]
        return Ok(json_sized(
            200,
            &Feed {
                posts: Posts(&posts),
            },
            4096,
        ));
    }
    if get_post {
        return Ok(match db.client.query_opt(&db.post, &[&id]).await? {
            Some(row) => {
                #[cfg(feature = "fixed-json")]
                {
                    json_bytes(200, fixed_json::one(post_view(&row)))
                }
                #[cfg(not(feature = "fixed-json"))]
                {
                    json(
                        200,
                        &OneView {
                            post: post_view(&row),
                        },
                    )
                }
            }
            None => error(404, "post not found"),
        });
    }
    let user = user.expect("authenticated write");
    if create {
        let writes::Output::Post(row) = app
            .writer
            .send(writes::Input::Post(user.id, body.clone()))
            .await?
        else {
            unreachable!()
        };
        return Ok(json(
            201,
            &One {
                post: Post {
                    id: row.get(0),
                    body,
                    created_at: timestamp(row.get(1)),
                    author: user.name,
                    like_count: 0,
                },
            },
        ));
    }
    match app.writer.send(writes::Input::Like(user.id, id)).await {
        Ok(writes::Output::Like(n)) => Ok(json(
            if n == 0 { 200 } else { 201 },
            &Like {
                liked: true,
                already_liked: n == 0,
                post_id: id,
            },
        )),
        Err(e) if e.code() == Some(&pg::error::SqlState::FOREIGN_KEY_VIOLATION) => {
            Ok(error(404, "post not found"))
        }
        Ok(_) => unreachable!(),
        Err(e) => Err(e),
    }
}

struct Config {
    url: String,
    secret: Vec<u8>,
    host: String,
    ports: Vec<u16>,
    workers: usize,
    connections: usize,
}
fn number(key: &str, default: usize) -> usize {
    env::var(key)
        .map(|v| v.parse().expect(key))
        .unwrap_or(default)
}
fn configured_ports() -> Vec<u16> {
    vec![
        u16::try_from(number("PORT", 80)).unwrap(),
        u16::try_from(number("EXTRA_PORT", 81)).unwrap(),
    ]
}

fn main() {
    let io_interval = number("WORKER_IO_INTERVAL", 61);
    assert!((1..=1024).contains(&io_interval));
    let workers = number(
        "WORKERS",
        std::thread::available_parallelism().unwrap().get(),
    );
    let connections = number("DB_CONNECTIONS", workers.saturating_mul(8).min(512));
    assert!(
        workers > 0 && workers * 2 <= connections && connections <= 512,
        "require 1 <= WORKERS <= DB_CONNECTIONS <= 512"
    );
    let config = Arc::new(Config {
        url: env::var("DATABASE_URL").expect("DATABASE_URL"),
        secret: env::var("JWT_SECRET").expect("JWT_SECRET").into_bytes(),
        host: env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into()),
        ports: configured_ports(),
        workers,
        connections,
    });
    assert!(!config.secret.is_empty());
    assert_ne!(config.ports[0], config.ports[1]);
    let mut threads = Vec::new();
    let mut senders = Vec::new();
    for index in 0..workers {
        let cfg = config.clone();
        let (tx, rx) = mpsc::channel(1024);
        senders.push(tx);
        threads.push(
            std::thread::Builder::new()
                .name(format!("http-{index}"))
                .spawn(move || {
                    // A worker panic must fail the process rather than silently reducing capacity.
                    let outcome = std::panic::catch_unwind(|| {
                        let runtime = tokio::runtime::Builder::new_current_thread()
                            .event_interval(io_interval as u32)
                            .enable_all()
                            .build()
                            .unwrap();
                        tokio::task::LocalSet::new().block_on(&runtime, worker(index, cfg, rx));
                    });
                    if outcome.is_err() {
                        std::process::exit(1);
                    }
                })
                .unwrap(),
        );
    }
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    tokio::task::LocalSet::new().block_on(&runtime, accept_connections(config, senders));
}
async fn worker(index: usize, cfg: Arc<Config>, mut incoming: mpsc::Receiver<std::net::TcpStream>) {
    let count = cfg.connections / cfg.workers + usize::from(index < cfg.connections % cfg.workers);
    let mut db = Vec::new();
    let mut writer = None;
    for index in 0..count {
        let (client, connection) = pg::connect(&cfg.url, NoTls).await.expect("DB connection");
        tokio::task::spawn_local(async move {
            if let Err(e) = connection.await {
                eprintln!("database disconnected: {e}");
            }
        });
        let feed = client
            .prepare(&format!(
                "{SELECT} ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
            ))
            .await
            .unwrap();
        let post = client
            .prepare(&format!("{SELECT} WHERE p.id = $1"))
            .await
            .unwrap();
        #[cfg(feature = "grouped-read-sync")]
        let (feed, post) = (feed.read_only(), post.read_only());
        let create = client
            .prepare("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at")
            .await
            .unwrap();
        let like = client.prepare("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING").await.unwrap();
        if index == 0 {
            let size = number("WRITE_BATCH_SIZE", 32);
            assert!((1..=64).contains(&size));
            let delay = number("WRITE_BATCH_MS", 5);
            assert!(delay <= 20);
            writer = Some(writes::Writer::start(
                client,
                create,
                like,
                size,
                Duration::from_millis(delay as u64),
            ));
        } else {
            db.push(Db {
                client,
                feed,
                post,
                #[cfg(feature = "least-loaded-db")]
                pending: Cell::new(0),
            });
        }
    }
    let read_depth = number("READ_PIPELINE_DEPTH", 8);
    assert!((1..=64).contains(&read_depth));
    let app = Rc::new(App {
        worker: index,
        diagnostics: env::var_os("DIAGNOSTICS").is_some(),
        route_metrics: RouteMetrics::default(),
        completed: Cell::new(0),
        queue_timeouts: Cell::new(0),
        db_timeouts: Cell::new(0),
        writer: writer.unwrap(),
        db,
        next: Cell::new(0),
        permits: Semaphore::new(count * read_depth),
        #[cfg(feature = "isolated-writes")]
        write_permits: Semaphore::new(256),
        secret: cfg.secret.clone(),
        started: Instant::now(),
    });
    if env::var_os("DIAGNOSTICS").is_some() {
        let metrics = app.clone();
        tokio::task::spawn_local(async move {
            loop {
                tokio::time::sleep(Duration::from_secs(5)).await;
                let (transactions, begin_us, statements_us, commit_us) =
                    metrics.writer.stage_metrics();
                eprintln!(
                    "write_stages time_ms={} worker={} transactions={} begin_us={} statements_us={} commit_us={}",
                    std::time::SystemTime::now()
                        .duration_since(std::time::UNIX_EPOCH)
                        .unwrap()
                        .as_millis(),
                    metrics.worker,
                    transactions,
                    begin_us,
                    statements_us,
                    commit_us
                );
                let (groups, jobs, max_batch, rollbacks) = metrics.writer.metrics();
                for (kind, name) in ["read", "write"].iter().enumerate() {
                    let m = &metrics.route_metrics;
                    eprintln!(
                        "route_metrics worker={} kind={} count={} queue_us={} operation_us={} max_queue_us={} max_operation_us={}",
                        metrics.worker,
                        name,
                        m.counts[kind].get(),
                        m.queue_us[kind].get(),
                        m.operation_us[kind].get(),
                        m.max_queue_us[kind].replace(0),
                        m.max_operation_us[kind].replace(0)
                    );
                }
                eprintln!(
                    "metrics worker={} completed={} queue_timeouts={} db_timeouts={} free_permits={} write_groups={} write_jobs={} max_batch={} rollbacks={}",
                    metrics.worker,
                    metrics.completed.get(),
                    metrics.queue_timeouts.get(),
                    metrics.db_timeouts.get(),
                    metrics.permits.available_permits(),
                    groups,
                    jobs,
                    max_batch,
                    rollbacks
                );
            }
        });
    }
    eprintln!("worker={index} db_connections={count} ready; dispatcher=round_robin");
    #[cfg(all(feature = "shared-idle", target_os = "linux"))]
    {
        let idle = idle::Idle::start(app).expect("idle reactor");
        while let Some(stream) = incoming.recv().await {
            if let Err(e) = stream.set_nodelay(true) {
                eprintln!("configure socket: {e}");
                continue;
            }
            if let Err(e) = idle.insert(stream) {
                eprintln!("register idle socket: {e}");
            }
        }
    }
    #[cfg(not(all(feature = "shared-idle", target_os = "linux")))]
    {
        while let Some(stream) = incoming.recv().await {
            let stream = match TcpStream::from_std(stream) {
                Ok(s) => s,
                Err(e) => {
                    eprintln!("register socket: {e}");
                    continue;
                }
            };
            let _ = stream.set_nodelay(true);
            let app = app.clone();
            // The detached task already owns the connection future and drops its
            // result. Avoid an extra async state machine retaining moved arguments.
            #[cfg(feature = "direct-http-task")]
            tokio::task::spawn_local(custom_http::serve(stream, app));
            #[cfg(not(feature = "direct-http-task"))]
            tokio::task::spawn_local(async move {
                #[cfg(feature = "custom-http")]
                {
                    let _ = custom_http::serve(stream, app).await;
                }
                #[cfg(not(feature = "custom-http"))]
                {
                    let service = service_fn(move |req| handle(req, app.clone()));
                    let _ = http1::Builder::new()
                        .keep_alive(true)
                        .max_buf_size(8192)
                        .serve_connection(TokioIo::new(stream), service)
                        .await;
                }
            });
        }
    }
}

async fn accept_connections(cfg: Arc<Config>, senders: Vec<mpsc::Sender<std::net::TcpStream>>) {
    let senders = Rc::new(senders);
    let next = Rc::new(Cell::new(0usize));
    let mut tasks = tokio::task::JoinSet::new();
    for (host, port) in cfg
        .host
        .split(',')
        .flat_map(|h| cfg.ports.iter().copied().map(move |p| (h, p)))
    {
        let addr = (host, port).to_socket_addrs().unwrap().next().unwrap();
        let socket =
            Socket::new(Domain::for_address(addr), Type::STREAM, Some(Protocol::TCP)).unwrap();
        socket.set_reuse_address(true).unwrap();
        // Optional local high-socket-count tuning; accepted sockets inherit these.
        if let Ok(bytes) = env::var("SOCKET_BUFFER_BYTES") {
            let bytes: usize = bytes.parse().expect("SOCKET_BUFFER_BYTES integer");
            assert!(bytes >= 8192);
            socket.set_recv_buffer_size(bytes).expect("receive buffer");
            socket.set_send_buffer_size(bytes).expect("send buffer");
        }
        socket.set_nonblocking(true).unwrap();
        socket.bind(&addr.into()).expect("bind listener");
        socket.listen(65535).unwrap();
        let listener = TcpListener::from_std(socket.into()).unwrap();
        let senders = senders.clone();
        let next = next.clone();
        tasks.spawn_local(async move {
            loop {
                let (stream, _) = match listener.accept().await {
                    Ok(v) => v,
                    Err(e) => {
                        eprintln!("accept: {e}");
                        tokio::time::sleep(Duration::from_millis(50)).await;
                        continue;
                    }
                };
                let index = next.get();
                next.set((index + 1) % senders.len());
                if let Ok(stream) = stream.into_std() {
                    senders[index].send(stream).await.expect("worker running");
                }
            }
        });
    }
    tasks
        .join_next()
        .await
        .expect("listeners")
        .expect("listener task");
}

#[cfg(test)]
mod tests {
    #[cfg(feature = "least-loaded-db")]
    #[test]
    fn stalled_connection_does_not_accumulate_reads_and_ties_rotate() {
        assert_eq!(super::least_loaded(4, 0, |i| [100, 2, 1, 3][i]), 2);
        assert_eq!(super::least_loaded(4, 3, |i| [0, 0, 0, 0][i]), 3);
        assert_eq!(super::least_loaded(4, 2, |i| [1, 1, 1, 1][i]), 2);
        let pending = std::cell::Cell::new(1);
        {
            let _guard = super::PendingRead(&pending);
        }
        assert_eq!(pending.get(), 0);
    }

    use super::*;
    #[test]
    fn stack_timestamp_matches_chrono() {
        for value in [
            "0000-01-01T00:00:00Z",
            "2024-02-29T23:59:59.999999Z",
            "9999-12-31T12:34:56.001999Z",
            "2016-12-31T23:59:60Z",
        ] {
            let t = DateTime::parse_from_rfc3339(value)
                .unwrap()
                .with_timezone(&Utc);
            assert_eq!(
                serde_json::to_string(&Timestamp(t)).unwrap(),
                serde_json::to_string(&timestamp(t)).unwrap()
            );
        }
    }
    #[test]
    fn extended_timestamp_matches_chrono() {
        let t = chrono::NaiveDate::from_ymd_opt(10000, 1, 1)
            .unwrap()
            .and_hms_opt(0, 0, 0)
            .unwrap()
            .and_utc();
        assert_eq!(
            serde_json::to_string(&Timestamp(t)).unwrap(),
            serde_json::to_string(&timestamp(t)).unwrap()
        );
    }
    #[test]
    fn truncates_timestamp_and_orders_json() {
        let t = DateTime::parse_from_rfc3339("2025-12-31T23:59:19.999999Z")
            .unwrap()
            .with_timezone(&Utc);
        assert_eq!(timestamp(t), "2025-12-31T23:59:19.999Z");
        let p = Post {
            id: 1,
            body: "a\"✓".into(),
            created_at: timestamp(t),
            author: "u".into(),
            like_count: 0,
        };
        assert_eq!(
            serde_json::to_string(&p).unwrap(),
            "{\"id\":1,\"body\":\"a\\\"✓\",\"created_at\":\"2025-12-31T23:59:19.999Z\",\"author\":\"u\",\"like_count\":0}"
        );
    }
}
