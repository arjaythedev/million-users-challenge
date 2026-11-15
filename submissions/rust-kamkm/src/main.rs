mod fast;
mod transport;
use std::env;
use std::net::{IpAddr, ToSocketAddrs};
use std::sync::Arc;
use std::time::Instant;

use deadpool_postgres::{GenericClient, Manager, ManagerConfig, Pool, RecyclingMethod, Runtime};
use fast::PgMicros;
use jsonwebtoken::{decode, Algorithm, DecodingKey, Validation};
use serde_json::Value;
use socket2::{Domain, Protocol, Socket, Type};
use tokio::net::TcpListener;
use tokio::sync::{mpsc, oneshot};
use tokio::time::{timeout, Duration};
use tokio_postgres::{error::SqlState, NoTls, Row, Statement};
use transport::{Req, ReqMethod};

// Requests allocate many short-lived buffers; mimalloc's per-thread free
// lists make that much cheaper than glibc malloc on the executor threads.
#[global_allocator]
static GLOBAL: mimalloc::MiMalloc = mimalloc::MiMalloc;

const FEED_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const POST_BY_ID_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1";
const INSERT_POST_SQL: &str =
    "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
const INSERT_LIKE_SQL: &str = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
const MAX_REQUEST_BODY: usize = 16 * 1024;

/// A response: status code and JSON body.
type Reply = (u16, Vec<u8>);

struct AppState {
    pool: Pool,
    writes: WriteBatcher,
    reads: ReadBatcher,
    jwt_key: DecodingKey,
    jwt_validation: Validation,
    started: Instant,
}

#[derive(Clone)]
struct ReadBatcher {
    tx: mpsc::Sender<ReadJob>,
}
struct ReadJob {
    id: Option<i64>,
    reply: oneshot::Sender<Result<Vec<Row>, ()>>,
}
impl ReadBatcher {
    /// `slots` bounds the read batches in flight. Each batch waits for all of
    /// its results before its slot frees, so too few slots leave requests
    /// queued while the database is idle.
    fn new(pool: Pool, slots: usize) -> Self {
        let (tx, mut rx) = mpsc::channel::<ReadJob>(4096);
        tokio::spawn(async move {
            let slots = Arc::new(tokio::sync::Semaphore::new(slots));
            loop {
                let Ok(permit) = Arc::clone(&slots).acquire_owned().await else {
                    break;
                };
                let Some(first) = rx.recv().await else { break };
                let mut jobs = Vec::with_capacity(64);
                jobs.push(first);
                while jobs.len() < 64 {
                    match rx.try_recv() {
                        Ok(job) => jobs.push(job),
                        Err(_) => break,
                    }
                }
                let pool = pool.clone();
                tokio::spawn(async move {
                    process_read_batch(&pool, jobs).await;
                    drop(permit);
                });
            }
        });
        Self { tx }
    }
    async fn submit(&self, id: Option<i64>) -> Result<Vec<Row>, ()> {
        timeout(Duration::from_secs(5), async {
            let (reply, result) = oneshot::channel();
            self.tx.send(ReadJob { id, reply }).await.map_err(|_| ())?;
            result.await.map_err(|_| ())?
        })
        .await
        .map_err(|_| ())?
    }
}
/// Rows are handed back as received; each handler writes its JSON from them,
/// so encoding runs in the requests' own tasks rather than the batch's.
async fn process_read_batch(pool: &Pool, jobs: Vec<ReadJob>) {
    let queries = async {
        let client = pool_client(pool).await.map_err(|_| ())?;
        let feed = client.prepare_cached(FEED_SQL).await.map_err(|_| ())?;
        let post = client
            .prepare_cached(POST_BY_ID_SQL)
            .await
            .map_err(|_| ())?;
        let results = futures_util::future::join_all(jobs.iter().map(|job| {
            let (client, feed, post) = (&client, &feed, &post);
            async move {
                match job.id {
                    Some(id) => client.query(post, &[&id]).await,
                    None => client.query(feed, &[]).await,
                }
                .map_err(|_| ())
            }
        }))
        .await;
        Ok::<_, ()>(results)
    };
    match timeout(Duration::from_secs(5), queries).await {
        Ok(Ok(results)) => {
            for (job, result) in jobs.into_iter().zip(results) {
                let _ = job.reply.send(result);
            }
        }
        _ => {
            for job in jobs {
                let _ = job.reply.send(Err(()));
            }
        }
    }
}

#[derive(Clone)]
struct WriteBatcher {
    tx: mpsc::Sender<WriteJob>,
}

enum WriteKind {
    Create {
        user_id: i64,
        username: String,
        body: String,
    },
    Like {
        user_id: i64,
        post_id: i64,
    },
}

struct WriteJob {
    kind: WriteKind,
    reply: oneshot::Sender<WriteOutcome>,
}

struct CreatedPost {
    id: i64,
    body: String,
    created_at: PgMicros,
    author: String,
}

enum WriteOutcome {
    Created(CreatedPost),
    Liked { inserted: bool, post_id: i64 },
    NotFound,
    Failed,
}

enum WriteFailure {
    NotFound,
    Other,
}

const WRITE_BATCH_LIMIT: usize = 64;

impl WriteBatcher {
    /// `slots` bounds the write batches in flight. Concurrent batches never
    /// hold the same (user, post) like: a user sends one request at a time.
    fn new(pool: Pool, slots: usize) -> Self {
        let (tx, rx) = mpsc::channel(WRITE_BATCH_LIMIT * 16);
        tokio::spawn(run_write_batches(pool, rx, slots));
        Self { tx }
    }

    async fn submit(&self, kind: WriteKind) -> WriteOutcome {
        let (reply, result) = oneshot::channel();
        if self.tx.send(WriteJob { kind, reply }).await.is_err() {
            return WriteOutcome::Failed;
        }
        result.await.unwrap_or(WriteOutcome::Failed)
    }
}

#[derive(Clone)]
struct AuthUser {
    id: i64,
    username: String,
}

#[derive(Debug)]
enum AuthError {
    Missing,
    Invalid,
    Payload,
}

impl AppState {
    async fn handle(&self, req: Req) -> Reply {
        let path = req.path.as_str();
        match req.method {
            ReqMethod::Get => {
                if path == "/health" {
                    return self.health().await;
                }
                if path == "/feed" {
                    return self.feed().await;
                }
                if let Some(id_text) = path.strip_prefix("/posts/") {
                    if !id_text.is_empty() && !id_text.contains('/') {
                        return match parse_post_id(id_text) {
                            Some(id) => self.get_post(id).await,
                            None => error_reply(400, "invalid post id"),
                        };
                    }
                }
                error_reply(404, "not found")
            }
            ReqMethod::Post => {
                if path == "/posts" {
                    let user = match self.authenticate(req.auth.as_deref()) {
                        Ok(user) => user,
                        Err(err) => return auth_error_reply(err),
                    };
                    return self.create_post(user, &req.body).await;
                }
                if let Some(id_text) = path
                    .strip_prefix("/posts/")
                    .and_then(|rest| rest.strip_suffix("/like"))
                {
                    if id_text.is_empty() || id_text.contains('/') {
                        return error_reply(404, "not found");
                    }
                    // Once the route matches, authenticate before validating the id.
                    let user = match self.authenticate(req.auth.as_deref()) {
                        Ok(user) => user,
                        Err(err) => return auth_error_reply(err),
                    };
                    return match parse_post_id(id_text) {
                        Some(id) => self.like_post(user.id, id).await,
                        None => error_reply(400, "invalid post id"),
                    };
                }
                error_reply(404, "not found")
            }
            ReqMethod::Other => error_reply(404, "not found"),
        }
    }

    async fn health(&self) -> Reply {
        match timeout(Duration::from_secs(5), self.pool.get()).await {
            Ok(Ok(client)) => match client.query_one("SELECT 1", &[]).await {
                Ok(_) => {
                    let mut body = br#"{"status":"ok","db":"ok","uptime_s":"#.to_vec();
                    fast::push_int(&mut body, self.started.elapsed().as_secs());
                    body.push(b'}');
                    (200, body)
                }
                Err(err) => degraded_reply(&err.to_string()),
            },
            Ok(Err(err)) => degraded_reply(&err.to_string()),
            Err(_) => degraded_reply("database pool timeout"),
        }
    }

    async fn feed(&self) -> Reply {
        let Ok(rows) = self.reads.submit(None).await else {
            return internal_error();
        };
        let mut body = Vec::with_capacity(16 + 320 * rows.len());
        body.extend_from_slice(br#"{"posts":["#);
        for (i, row) in rows.iter().enumerate() {
            if i > 0 {
                body.push(b',');
            }
            if fast::push_post_row(&mut body, row).is_none() {
                return internal_error();
            }
        }
        body.extend_from_slice(b"]}");
        (200, body)
    }

    async fn get_post(&self, id: i64) -> Reply {
        let Ok(rows) = self.reads.submit(Some(id)).await else {
            return internal_error();
        };
        // The lookup is by primary key, so there is at most one row.
        let Some(row) = rows.last() else {
            return error_reply(404, "post not found");
        };
        let mut body = Vec::with_capacity(384);
        body.extend_from_slice(br#"{"post":"#);
        if fast::push_post_row(&mut body, row).is_none() {
            return internal_error();
        }
        body.push(b'}');
        (200, body)
    }

    async fn create_post(&self, user: AuthUser, body: &[u8]) -> Reply {
        let body = match parse_post_body(body) {
            Ok(body) => body,
            Err(message) => return error_reply(400, message),
        };
        match self
            .writes
            .submit(WriteKind::Create {
                user_id: user.id,
                username: user.username,
                body,
            })
            .await
        {
            WriteOutcome::Created(post) => {
                let mut out = Vec::with_capacity(160 + post.body.len() + post.author.len());
                out.extend_from_slice(br#"{"post":"#);
                if fast::push_post(
                    &mut out,
                    post.id,
                    &post.body,
                    post.created_at,
                    &post.author,
                    0,
                )
                .is_none()
                {
                    return internal_error();
                }
                out.push(b'}');
                (201, out)
            }
            _ => internal_error(),
        }
    }

    async fn like_post(&self, user_id: i64, post_id: i64) -> Reply {
        match self
            .writes
            .submit(WriteKind::Like { user_id, post_id })
            .await
        {
            WriteOutcome::Liked { inserted, post_id } => {
                let mut out = br#"{"liked":true,"already_liked":"#.to_vec();
                out.extend_from_slice(if inserted { b"false" } else { b"true" });
                out.extend_from_slice(br#","post_id":"#);
                fast::push_int(&mut out, post_id);
                out.push(b'}');
                (if inserted { 201 } else { 200 }, out)
            }
            WriteOutcome::NotFound => error_reply(404, "post not found"),
            _ => internal_error(),
        }
    }

    fn authenticate(&self, header: Option<&str>) -> Result<AuthUser, AuthError> {
        let Some(value) = header else {
            return Err(AuthError::Missing);
        };
        let Some(token) = value.strip_prefix("Bearer ") else {
            return Err(AuthError::Missing);
        };
        if token.is_empty() {
            return Err(AuthError::Invalid);
        }
        // Signature, algorithm and exp are checked on every call; only the key
        // and the validation rules are built once.
        let data = decode::<Value>(token, &self.jwt_key, &self.jwt_validation)
            .map_err(|_| AuthError::Invalid)?;
        let claims = data.claims.as_object().ok_or(AuthError::Payload)?;
        let sub = claims
            .get("sub")
            .and_then(Value::as_str)
            .ok_or(AuthError::Payload)?;
        let id = parse_post_id(sub).ok_or(AuthError::Payload)?;
        let username = claims
            .get("username")
            .and_then(Value::as_str)
            .ok_or(AuthError::Payload)?;
        Ok(AuthUser {
            id,
            username: username.to_owned(),
        })
    }
}

async fn run_write_batches(pool: Pool, mut rx: mpsc::Receiver<WriteJob>, slots: usize) {
    // While earlier batches commit, the next one keeps collecting writes.
    let slots = Arc::new(tokio::sync::Semaphore::new(slots));
    loop {
        let Ok(permit) = Arc::clone(&slots).acquire_owned().await else {
            break;
        };
        let Some(first) = rx.recv().await else { break };
        let mut batch = Vec::with_capacity(WRITE_BATCH_LIMIT);
        batch.push(first);
        let deadline = tokio::time::Instant::now() + Duration::from_millis(1);
        while batch.len() < WRITE_BATCH_LIMIT {
            match tokio::time::timeout_at(deadline, rx.recv()).await {
                Ok(Some(job)) => batch.push(job),
                _ => break,
            }
        }
        let pool = pool.clone();
        tokio::spawn(async move {
            process_write_batch(&pool, batch).await;
            drop(permit);
        });
    }
}

async fn process_write_batch(pool: &Pool, batch: Vec<WriteJob>) {
    let mut client = match pool_client(pool).await {
        Ok(client) => client,
        Err(_) => {
            for job in batch {
                process_one_write(pool, job).await;
            }
            return;
        }
    };
    let insert_statement = match client.prepare_cached(INSERT_POST_SQL).await {
        Ok(statement) => statement,
        Err(_) => {
            for job in batch {
                process_one_write(pool, job).await;
            }
            return;
        }
    };
    let like_statement = match client.prepare_cached(INSERT_LIKE_SQL).await {
        Ok(statement) => statement,
        Err(_) => {
            for job in batch {
                process_one_write(pool, job).await;
            }
            return;
        }
    };
    let tx = match client.transaction().await {
        Ok(tx) => tx,
        Err(_) => {
            for job in batch {
                process_one_write(pool, job).await;
            }
            return;
        }
    };
    // Poll all queries together so tokio-postgres can pipeline them. Responses
    // stay private until the transaction has committed successfully.
    let outcomes = futures_util::future::join_all(
        batch
            .iter()
            .map(|job| execute_write(&tx, &job.kind, &insert_statement, &like_statement)),
    )
    .await;
    if outcomes.iter().all(Result::is_ok) {
        if tx.commit().await.is_ok() {
            for (job, result) in batch.into_iter().zip(outcomes) {
                if let Ok(result) = result {
                    let _ = job.reply.send(result);
                }
            }
            return;
        }
        for job in batch {
            let _ = job.reply.send(WriteOutcome::Failed);
        }
        return;
    }
    let _ = tx.rollback().await;
    drop(client);
    for job in batch {
        process_one_write(pool, job).await;
    }
}

async fn process_one_write(pool: &Pool, job: WriteJob) {
    let outcome = async {
        let mut client = pool_client(pool).await.map_err(|_| WriteFailure::Other)?;
        let insert_statement = client
            .prepare_cached(INSERT_POST_SQL)
            .await
            .map_err(|_| WriteFailure::Other)?;
        let like_statement = client
            .prepare_cached(INSERT_LIKE_SQL)
            .await
            .map_err(|_| WriteFailure::Other)?;
        let tx = client
            .transaction()
            .await
            .map_err(|_| WriteFailure::Other)?;
        let result = execute_write(&tx, &job.kind, &insert_statement, &like_statement).await;
        match result {
            Ok(outcome) => {
                tx.commit().await.map_err(|_| WriteFailure::Other)?;
                Ok(outcome)
            }
            Err(error) => {
                let _ = tx.rollback().await;
                Err(error)
            }
        }
    }
    .await;
    let _ = job.reply.send(match outcome {
        Ok(value) => value,
        Err(WriteFailure::NotFound) => WriteOutcome::NotFound,
        Err(WriteFailure::Other) => WriteOutcome::Failed,
    });
}

async fn execute_write(
    tx: &tokio_postgres::Transaction<'_>,
    kind: &WriteKind,
    insert_statement: &Statement,
    like_statement: &Statement,
) -> Result<WriteOutcome, WriteFailure> {
    match kind {
        WriteKind::Create {
            user_id,
            username,
            body,
        } => {
            let row = tx
                .query_one(insert_statement, &[user_id, body])
                .await
                .map_err(|_| WriteFailure::Other)?;
            Ok(WriteOutcome::Created(CreatedPost {
                id: row.try_get(0).map_err(|_| WriteFailure::Other)?,
                body: body.clone(),
                created_at: row.try_get(1).map_err(|_| WriteFailure::Other)?,
                author: username.clone(),
            }))
        }
        WriteKind::Like { user_id, post_id } => {
            let inserted = tx
                .execute(like_statement, &[user_id, post_id])
                .await
                .map_err(|err| {
                    if err.as_db_error().is_some_and(|db| {
                        db.code() == &SqlState::FOREIGN_KEY_VIOLATION
                            && db.constraint() == Some("likes_post_id_fkey")
                    }) {
                        WriteFailure::NotFound
                    } else {
                        WriteFailure::Other
                    }
                })?;
            Ok(WriteOutcome::Liked {
                inserted: inserted != 0,
                post_id: *post_id,
            })
        }
    }
}

async fn pool_client(pool: &Pool) -> Result<deadpool_postgres::Client, ()> {
    timeout(Duration::from_secs(5), pool.get())
        .await
        .map_err(|_| ())?
        .map_err(|_| ())
}

fn parse_post_id(value: &str) -> Option<i64> {
    if value.is_empty() || !value.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let id = value.parse::<i64>().ok()?;
    (id > 0).then_some(id)
}

fn parse_post_body(body: &[u8]) -> Result<String, &'static str> {
    let value: Value = serde_json::from_slice(body).map_err(|_| "malformed JSON body")?;
    let text = value
        .as_object()
        .and_then(|obj| obj.get("body"))
        .and_then(Value::as_str)
        .map(str::trim)
        .ok_or("body is required")?;
    if text.is_empty() {
        return Err("body is required");
    }
    if text.chars().count() > 500 {
        return Err("body must be at most 500 characters");
    }
    Ok(text.to_owned())
}

fn auth_error_reply(error: AuthError) -> Reply {
    match error {
        AuthError::Missing => error_reply(401, "missing bearer token"),
        AuthError::Invalid => error_reply(401, "invalid or expired token"),
        AuthError::Payload => error_reply(401, "invalid token payload"),
    }
}

fn error_reply(status: u16, message: &str) -> Reply {
    let mut body = br#"{"error":"#.to_vec();
    fast::push_str(&mut body, message);
    body.push(b'}');
    (status, body)
}

fn internal_error() -> Reply {
    error_reply(500, "internal server error")
}

fn degraded_reply(error: &str) -> Reply {
    let mut body = br#"{"status":"degraded","db":"unreachable","error":"#.to_vec();
    fast::push_str(&mut body, error);
    body.push(b'}');
    (503, body)
}

fn bind_listener(host: &str, port: u16) -> std::io::Result<TcpListener> {
    let host = if host == "*" || host.is_empty() {
        "0.0.0.0"
    } else {
        host
    };
    let addr = (host, port).to_socket_addrs()?.next().ok_or_else(|| {
        std::io::Error::new(std::io::ErrorKind::AddrNotAvailable, "host has no address")
    })?;
    let domain = match addr.ip() {
        IpAddr::V4(_) => Domain::IPV4,
        IpAddr::V6(_) => Domain::IPV6,
    };
    let socket = Socket::new(domain, Type::STREAM, Some(Protocol::TCP))?;
    socket.set_reuse_address(true)?;
    socket.set_nonblocking(true)?;
    socket.bind(&addr.into())?;
    socket.listen(65_535)?;
    let listener = std::net::TcpListener::from(socket);
    TcpListener::from_std(listener)
}

async fn serve_listener(listener: TcpListener, state: Arc<AppState>) -> std::io::Result<()> {
    loop {
        let (stream, _) = listener.accept().await?;
        let _ = stream.set_nodelay(true);
        let state = Arc::clone(&state);
        tokio::spawn(async move {
            let _ = transport::serve(stream, state).await;
        });
    }
}

fn env_required(name: &str) -> Result<String, Box<dyn std::error::Error + Send + Sync>> {
    env::var(name).map_err(|_| format!("missing required environment variable {name}").into())
}

fn env_count(name: &str, default: usize) -> usize {
    match env::var(name) {
        Ok(value) => value
            .parse::<usize>()
            .ok()
            .filter(|&n| (1..=512).contains(&n))
            .unwrap_or_else(|| panic!("{name} must be an integer from 1 through 512")),
        Err(_) => default,
    }
}

#[tokio::main(flavor = "multi_thread")]
async fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let database_url = env_required("DATABASE_URL")?;
    let jwt_secret = env_required("JWT_SECRET")?;
    let host = env::var("HOST").unwrap_or_else(|_| "0.0.0.0".to_owned());
    let port = env::var("PORT")
        .unwrap_or_else(|_| "3000".to_owned())
        .parse::<u16>()?;
    let extra_port = env::var("EXTRA_PORT")
        .ok()
        .filter(|s| !s.is_empty())
        .map(|s| s.parse::<u16>())
        .transpose()?;

    let pg_config: tokio_postgres::Config = database_url.parse()?;
    let mut pg_config = pg_config;
    pg_config.options("-c jit=off -c plan_cache_mode=force_generic_plan");
    let manager = Manager::from_config(
        pg_config,
        NoTls,
        ManagerConfig {
            recycling_method: RecyclingMethod::Fast,
        },
    );
    let cores = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    // DB_POOL is the upper limit shared by read batches, writes and health
    // checks.
    let pool_size = env_count("DB_POOL", 128);
    // Read batches in flight; each holds one pooled connection.
    let read_batches = env_count("READ_BATCHES", 48).min(pool_size);
    let pool = Pool::builder(manager)
        .max_size(pool_size)
        .runtime(Runtime::Tokio1)
        .build()?;

    let mut jwt_validation = Validation::new(Algorithm::HS256);
    jwt_validation.leeway = 0;
    jwt_validation.validate_exp = true;
    jwt_validation.validate_aud = false;
    jwt_validation.required_spec_claims.insert("exp".to_owned());

    // Write batches in flight; each holds one pooled connection.
    let write_batches = env_count("WRITE_BATCHES", 4).min(pool_size);
    let writes = WriteBatcher::new(pool.clone(), write_batches);
    let reads = ReadBatcher::new(pool.clone(), read_batches);
    let state = Arc::new(AppState {
        pool,
        writes,
        reads,
        jwt_key: DecodingKey::from_secret(jwt_secret.as_bytes()),
        jwt_validation,
        started: Instant::now(),
    });
    let listener = bind_listener(&host, port)?;
    let extra_listener = extra_port
        .map(|extra| bind_listener(&host, extra))
        .transpose()?;
    eprintln!(
        "rust-luna listening on {host}:{port}{} pool={pool_size} read_batches={read_batches} write_batches={write_batches} workers={cores}",
        extra_port.map_or_else(String::new, |p| format!(" and {host}:{p}"))
    );

    let primary_task = tokio::spawn(serve_listener(listener, Arc::clone(&state)));
    let extra_task =
        extra_listener.map(|listener| tokio::spawn(serve_listener(listener, Arc::clone(&state))));
    tokio::select! {
        result = primary_task => { result??; }
        result = async {
            if let Some(task) = extra_task {
                task.await??;
            } else {
                std::future::pending::<()>().await;
            }
            Ok::<(), Box<dyn std::error::Error + Send + Sync>>(())
        } => { result?; }
        _ = tokio::signal::ctrl_c() => {}
    }
    Ok(())
}
