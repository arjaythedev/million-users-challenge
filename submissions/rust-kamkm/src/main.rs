mod transport;
use std::env;
use std::net::{IpAddr, ToSocketAddrs};
use std::sync::Arc;
use std::time::Instant;

use bytes::Bytes;
use chrono::{DateTime, SecondsFormat, Utc};
use deadpool_postgres::{GenericClient, Manager, ManagerConfig, Pool, RecyclingMethod, Runtime};
use flate2::{Compress, Compression, FlushCompress, Status};
use http_body_util::{BodyExt, Full};
use hyper::header::{self, HeaderValue};
use hyper::{Method, Request, Response, StatusCode};
use jsonwebtoken::{decode, Algorithm, DecodingKey, Validation};
use serde::Serialize;
use serde_json::Value;
use socket2::{Domain, Protocol, Socket, Type};
use std::cell::RefCell;
use tokio::net::TcpListener;
use tokio::sync::{mpsc, oneshot};
use tokio::time::{timeout, Duration};
use tokio_postgres::{error::SqlState, NoTls, Row, Statement};

const FEED_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const POST_BY_ID_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1";
const INSERT_POST_SQL: &str =
    "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
const INSERT_LIKE_SQL: &str = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
const MAX_REQUEST_BODY: usize = 16 * 1024;

type HttpBody = Full<Bytes>;

#[derive(Clone)]
struct AppState {
    pool: Pool,
    writes: WriteBatcher,
    reads: ReadBatcher,
    jwt_secret: Arc<[u8]>,
    started: Instant,
}

#[derive(Clone)]
struct ReadBatcher {
    tx: mpsc::Sender<ReadJob>,
}
struct ReadJob {
    id: Option<i64>,
    reply: oneshot::Sender<Result<Vec<PostBody>, ()>>,
}
impl ReadBatcher {
    fn new(pool: Pool) -> Self {
        let (tx, mut rx) = mpsc::channel::<ReadJob>(1024);
        tokio::spawn(async move {
            let slots = Arc::new(tokio::sync::Semaphore::new(8));
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
    async fn submit(&self, id: Option<i64>) -> Result<Vec<PostBody>, ()> {
        timeout(Duration::from_secs(5), async {
            let (reply, result) = oneshot::channel();
            self.tx.send(ReadJob { id, reply }).await.map_err(|_| ())?;
            result.await.map_err(|_| ())?
        })
        .await
        .map_err(|_| ())?
    }
}
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
                let rows = match job.id {
                    Some(id) => client.query(post, &[&id]).await,
                    None => client.query(feed, &[]).await,
                }
                .map_err(|_| ())?;
                rows.iter()
                    .map(|row| post_from_row(row).ok_or(()))
                    .collect()
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

enum WriteOutcome {
    Created(PostBody),
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
    fn new(pool: Pool) -> Self {
        let (tx, rx) = mpsc::channel(WRITE_BATCH_LIMIT * 4);
        tokio::spawn(run_write_batches(pool, rx));
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

#[derive(Serialize)]
struct ErrorBody<'a> {
    error: &'a str,
}

#[derive(Serialize)]
struct PostBody {
    id: i64,
    body: String,
    created_at: String,
    author: String,
    like_count: i32,
}

#[derive(Serialize)]
struct FeedBody {
    posts: Vec<PostBody>,
}

#[derive(Serialize)]
struct PostResponse {
    post: PostBody,
}

#[derive(Serialize)]
struct LikeResponse {
    liked: bool,
    already_liked: bool,
    post_id: i64,
}

#[derive(Serialize)]
struct HealthBody {
    status: &'static str,
    db: &'static str,
    uptime_s: u64,
}

#[derive(Serialize)]
struct DegradedBody<'a> {
    status: &'static str,
    db: &'static str,
    error: &'a str,
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
    async fn handle(&self, req: Request<Full<Bytes>>) -> Response<HttpBody> {
        let method = req.method().clone();
        let path = req.uri().path();
        let wants_gzip = accepts_gzip(req.headers());

        if method == Method::GET && path == "/health" {
            return self.health(wants_gzip).await;
        }
        if method == Method::GET && path == "/feed" {
            return self.feed(wants_gzip).await;
        }
        if method == Method::GET {
            if let Some(id_text) = path.strip_prefix("/posts/") {
                if !id_text.is_empty() && !id_text.contains('/') {
                    let id = match parse_post_id(id_text) {
                        Some(id) => id,
                        None => {
                            return error_response(
                                StatusCode::BAD_REQUEST,
                                "invalid post id",
                                wants_gzip,
                            )
                        }
                    };
                    return self.get_post(id, wants_gzip).await;
                }
            }
            return error_response(StatusCode::NOT_FOUND, "not found", wants_gzip);
        }
        if method == Method::POST && path == "/posts" {
            let user = match authenticate(req.headers(), &self.jwt_secret) {
                Ok(user) => user,
                Err(err) => return auth_error_response(err, wants_gzip),
            };
            let body = match read_body(req).await {
                Ok(body) => body,
                Err(BodyError::TooLarge) => {
                    return error_response(
                        StatusCode::BAD_REQUEST,
                        "malformed JSON body",
                        wants_gzip,
                    )
                }
                Err(BodyError::Read) => {
                    return error_response(
                        StatusCode::INTERNAL_SERVER_ERROR,
                        "internal server error",
                        wants_gzip,
                    )
                }
            };
            return self.create_post(user, &body, wants_gzip).await;
        }
        if method == Method::POST {
            if let Some(id_text) = path.strip_prefix("/posts/") {
                if let Some(id_text) = id_text.strip_suffix("/like") {
                    // Authentication deliberately precedes id validation, as required by the API contract.
                    let user = match authenticate(req.headers(), &self.jwt_secret) {
                        Ok(user) => user,
                        Err(err) => return auth_error_response(err, wants_gzip),
                    };
                    if id_text.is_empty() || id_text.contains('/') {
                        return error_response(StatusCode::NOT_FOUND, "not found", wants_gzip);
                    }
                    let id = match parse_post_id(id_text) {
                        Some(id) => id,
                        None => {
                            return error_response(
                                StatusCode::BAD_REQUEST,
                                "invalid post id",
                                wants_gzip,
                            )
                        }
                    };
                    return self.like_post(user.id, id, wants_gzip).await;
                }
            }
            return error_response(StatusCode::NOT_FOUND, "not found", wants_gzip);
        }
        error_response(StatusCode::NOT_FOUND, "not found", wants_gzip)
    }

    async fn health(&self, gzip: bool) -> Response<HttpBody> {
        match timeout(Duration::from_secs(5), self.pool.get()).await {
            Ok(Ok(client)) => match client.query_one("SELECT 1", &[]).await {
                Ok(_) => json_response(
                    StatusCode::OK,
                    &HealthBody {
                        status: "ok",
                        db: "ok",
                        uptime_s: self.started.elapsed().as_secs(),
                    },
                    gzip,
                ),
                Err(err) => degraded_response(&err.to_string(), gzip),
            },
            Ok(Err(err)) => degraded_response(&err.to_string(), gzip),
            Err(_) => degraded_response("database pool timeout", gzip),
        }
    }

    async fn feed(&self, gzip: bool) -> Response<HttpBody> {
        match self.reads.submit(None).await {
            Ok(posts) => json_response(StatusCode::OK, &FeedBody { posts }, gzip),
            Err(()) => error_response(
                StatusCode::INTERNAL_SERVER_ERROR,
                "internal server error",
                gzip,
            ),
        }
    }
    async fn get_post(&self, id: i64, gzip: bool) -> Response<HttpBody> {
        match self.reads.submit(Some(id)).await {
            Ok(mut posts) => match posts.pop() {
                Some(post) => json_response(StatusCode::OK, &PostResponse { post }, gzip),
                None => error_response(StatusCode::NOT_FOUND, "post not found", gzip),
            },
            Err(()) => error_response(
                StatusCode::INTERNAL_SERVER_ERROR,
                "internal server error",
                gzip,
            ),
        }
    }

    async fn create_post(&self, user: AuthUser, body: &[u8], gzip: bool) -> Response<HttpBody> {
        let body = match parse_post_body(body) {
            Ok(body) => body,
            Err(message) => return error_response(StatusCode::BAD_REQUEST, message, gzip),
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
                json_response(StatusCode::CREATED, &PostResponse { post }, gzip)
            }
            _ => error_response(
                StatusCode::INTERNAL_SERVER_ERROR,
                "internal server error",
                gzip,
            ),
        }
    }

    async fn like_post(&self, user_id: i64, post_id: i64, gzip: bool) -> Response<HttpBody> {
        match self
            .writes
            .submit(WriteKind::Like { user_id, post_id })
            .await
        {
            WriteOutcome::Liked { inserted, post_id } => {
                let status = if inserted {
                    StatusCode::CREATED
                } else {
                    StatusCode::OK
                };
                json_response(
                    status,
                    &LikeResponse {
                        liked: true,
                        already_liked: !inserted,
                        post_id,
                    },
                    gzip,
                )
            }
            WriteOutcome::NotFound => error_response(StatusCode::NOT_FOUND, "post not found", gzip),
            _ => error_response(
                StatusCode::INTERNAL_SERVER_ERROR,
                "internal server error",
                gzip,
            ),
        }
    }
}

async fn run_write_batches(pool: Pool, mut rx: mpsc::Receiver<WriteJob>) {
    while let Some(first) = rx.recv().await {
        let mut batch = Vec::with_capacity(WRITE_BATCH_LIMIT);
        batch.push(first);
        let deadline = tokio::time::Instant::now() + Duration::from_millis(1);
        while batch.len() < WRITE_BATCH_LIMIT {
            match tokio::time::timeout_at(deadline, rx.recv()).await {
                Ok(Some(job)) => batch.push(job),
                _ => break,
            }
        }
        process_write_batch(&pool, batch).await;
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
            let created_at: DateTime<Utc> = row.get(1);
            Ok(WriteOutcome::Created(PostBody {
                id: row.get(0),
                body: body.clone(),
                created_at: format_timestamp(created_at),
                author: username.clone(),
                like_count: 0,
            }))
        }
        WriteKind::Like { user_id, post_id } => {
            let inserted = tx
                .execute(like_statement, &[user_id, post_id])
                .await
                .map_err(|err| {
                    if err
                        .as_db_error()
                        .is_some_and(|db| db.code() == &SqlState::FOREIGN_KEY_VIOLATION)
                    {
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

fn post_from_row(row: &Row) -> Option<PostBody> {
    Some(PostBody {
        id: row.try_get(0).ok()?,
        body: row.try_get(1).ok()?,
        created_at: format_timestamp(row.try_get(2).ok()?),
        author: row.try_get(3).ok()?,
        like_count: row.try_get(4).ok()?,
    })
}

fn format_timestamp(ts: DateTime<Utc>) -> String {
    ts.to_rfc3339_opts(SecondsFormat::Millis, true)
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

enum BodyError {
    TooLarge,
    Read,
}

async fn read_body(req: Request<Full<Bytes>>) -> Result<Vec<u8>, BodyError> {
    if req
        .headers()
        .get(header::CONTENT_LENGTH)
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.parse::<usize>().ok())
        .is_some_and(|length| length > MAX_REQUEST_BODY)
    {
        return Err(BodyError::TooLarge);
    }
    let mut incoming = req.into_body();
    let mut bytes = Vec::with_capacity(256);
    while let Some(frame) = incoming.frame().await {
        let frame = frame.map_err(|_| BodyError::Read)?;
        if let Ok(data) = frame.into_data() {
            if bytes.len().saturating_add(data.len()) > MAX_REQUEST_BODY {
                return Err(BodyError::TooLarge);
            }
            bytes.extend_from_slice(&data);
        }
    }
    Ok(bytes)
}

fn authenticate(headers: &hyper::HeaderMap, secret: &[u8]) -> Result<AuthUser, AuthError> {
    let Some(header) = headers.get(header::AUTHORIZATION) else {
        return Err(AuthError::Missing);
    };
    let Ok(value) = header.to_str() else {
        return Err(AuthError::Missing);
    };
    let Some(token) = value.strip_prefix("Bearer ") else {
        return Err(AuthError::Missing);
    };
    if token.is_empty() {
        return Err(AuthError::Invalid);
    }

    let mut validation = Validation::new(Algorithm::HS256);
    validation.leeway = 0;
    validation.validate_exp = true;
    validation.validate_aud = false;
    validation.required_spec_claims.insert("exp".to_owned());
    let key = DecodingKey::from_secret(secret);
    let data = decode::<Value>(token, &key, &validation).map_err(|_| AuthError::Invalid)?;
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

fn auth_error_response(error: AuthError, gzip: bool) -> Response<HttpBody> {
    match error {
        AuthError::Missing => {
            error_response(StatusCode::UNAUTHORIZED, "missing bearer token", gzip)
        }
        AuthError::Invalid => {
            error_response(StatusCode::UNAUTHORIZED, "invalid or expired token", gzip)
        }
        AuthError::Payload => {
            error_response(StatusCode::UNAUTHORIZED, "invalid token payload", gzip)
        }
    }
}

fn error_response(status: StatusCode, message: &'static str, gzip: bool) -> Response<HttpBody> {
    json_response(status, &ErrorBody { error: message }, gzip)
}

fn degraded_response(error: &str, gzip: bool) -> Response<HttpBody> {
    json_response(
        StatusCode::SERVICE_UNAVAILABLE,
        &DegradedBody {
            status: "degraded",
            db: "unreachable",
            error,
        },
        gzip,
    )
}

fn json_response<T: Serialize>(
    status: StatusCode,
    value: &T,
    force_gzip: bool,
) -> Response<HttpBody> {
    let plain = match serde_json::to_vec(value) {
        Ok(bytes) => bytes,
        Err(_) => br#"{"error":"internal server error"}"#.to_vec(),
    };
    json_bytes_response(status, Bytes::from(plain), force_gzip)
}

thread_local! {
    static GZIP: RefCell<Compress> = RefCell::new(Compress::new_gzip(Compression::fast(), 15));
}

fn json_bytes_response(status: StatusCode, plain: Bytes, force_gzip: bool) -> Response<HttpBody> {
    let (body, compressed) = if force_gzip && plain.len() >= 1024 {
        // Reuse the compressor's history tables on this executor thread.
        // This synchronous section never holds the borrow across an await.
        GZIP.with(|state| {
            let mut compressor = state.borrow_mut();
            compressor.reset();
            let mut output = Vec::with_capacity(plain.len() + plain.len() / 1000 + 128);
            match compressor.compress_vec(&plain, &mut output, FlushCompress::Finish) {
                Ok(Status::StreamEnd) if compressor.total_in() == plain.len() as u64 => {
                    (output, true)
                }
                _ => (plain.to_vec(), false),
            }
        })
    } else {
        (plain.to_vec(), false)
    };
    let body_len = body.len();
    let mut response = Response::new(Full::new(Bytes::from(body)));
    *response.status_mut() = status;
    let headers = response.headers_mut();
    headers.insert(
        header::CONTENT_TYPE,
        HeaderValue::from_static("application/json"),
    );
    let length = HeaderValue::from_str(&body_len.to_string()).expect("numeric content length");
    headers.insert(header::CONTENT_LENGTH, length);
    if compressed {
        headers.insert(header::CONTENT_ENCODING, HeaderValue::from_static("gzip"));
        headers.insert(header::VARY, HeaderValue::from_static("Accept-Encoding"));
    }
    response
}

fn accepts_gzip(headers: &hyper::HeaderMap) -> bool {
    let Some(value) = headers
        .get(header::ACCEPT_ENCODING)
        .and_then(|v| v.to_str().ok())
    else {
        return false;
    };
    value.split(',').any(|part| {
        let mut segments = part.split(';');
        let coding = segments.next().map(str::trim).unwrap_or_default();
        if !coding.eq_ignore_ascii_case("gzip") {
            return false;
        }
        !segments.any(|parameter| {
            let mut kv = parameter.trim().splitn(2, '=');
            kv.next()
                .map(str::trim)
                .unwrap_or_default()
                .eq_ignore_ascii_case("q")
                && kv
                    .next()
                    .map(str::trim)
                    .is_some_and(|v| v == "0" || v == "0.0" || v == "0.00" || v == "0.000")
        })
    })
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
    // Batch concurrency bounds active read sessions. DB_POOL is the upper
    // limit shared with writes and profile lookups.
    let default_pool = 128;
    let pool_size = match env::var("DB_POOL") {
        Ok(value) => value
            .parse::<usize>()
            .ok()
            .filter(|&n| (1..=512).contains(&n))
            .expect("DB_POOL must be an integer from 1 through 512"),
        Err(_) => default_pool,
    };
    let pool = Pool::builder(manager)
        .max_size(pool_size)
        .runtime(Runtime::Tokio1)
        .build()?;

    let writes = WriteBatcher::new(pool.clone());
    let reads = ReadBatcher::new(pool.clone());
    let state = Arc::new(AppState {
        pool,
        writes,
        reads,
        jwt_secret: Arc::from(jwt_secret.into_bytes()),
        started: Instant::now(),
    });
    let listener = bind_listener(&host, port)?;
    let extra_listener = extra_port
        .map(|extra| bind_listener(&host, extra))
        .transpose()?;
    eprintln!(
        "rust-luna listening on {host}:{port}{} pool={pool_size} workers={cores}",
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
