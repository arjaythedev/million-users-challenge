//! The five endpoints (SPEC.md): routing and validation, the one query each request runs, and the JSON bodies.

use serde_json::Value;
use tokio_postgres::{Row, error::SqlState};

use crate::{
    Ctx,
    db::{self, Ts},
    json,
};

const MAX_POST_CHARS: usize = 500;

/// What a request needs done, decided before any I/O.
pub enum Job {
    /// Answered without the database (bad id, auth and validation errors, unknown paths).
    Done(Reply),
    Health,
    Feed,
    Post(i64),
    Create { user_id: i64, author: String, body: String },
    Like { user_id: i64, post_id: i64 },
}

pub enum Reply {
    /// A fixed status and JSON body.
    Fixed(u16, &'static str),
    Health { uptime_s: u64, error: Option<String> },
    Feed(Vec<Row>),
    Post(Row),
    Created { id: i64, created_at: i64, body: String, author: String },
    Liked { inserted: bool, post_id: i64 },
}

pub const NOT_FOUND: Reply = Reply::Fixed(404, r#"{"error":"not found"}"#);
pub const INVALID_POST_ID: Reply = Reply::Fixed(400, r#"{"error":"invalid post id"}"#);
pub const POST_NOT_FOUND: Reply = Reply::Fixed(404, r#"{"error":"post not found"}"#);
pub const MISSING_TOKEN: Reply = Reply::Fixed(401, r#"{"error":"missing bearer token"}"#);
pub const INVALID_TOKEN: Reply = Reply::Fixed(401, r#"{"error":"invalid or expired token"}"#);
pub const INVALID_PAYLOAD: Reply = Reply::Fixed(401, r#"{"error":"invalid token payload"}"#);
pub const MALFORMED_JSON: Reply = Reply::Fixed(400, r#"{"error":"malformed JSON body"}"#);
pub const BODY_REQUIRED: Reply = Reply::Fixed(400, r#"{"error":"body is required"}"#);
pub const BODY_TOO_LONG: Reply = Reply::Fixed(400, r#"{"error":"body must be at most 500 characters"}"#);
pub const INTERNAL: Reply = Reply::Fixed(500, r#"{"error":"internal server error"}"#);
// HTTP-level errors (the connection is closed after them).
pub const BAD_REQUEST: Reply = Reply::Fixed(400, r#"{"error":"bad request"}"#);
pub const LENGTH_REQUIRED: Reply = Reply::Fixed(411, r#"{"error":"length required"}"#);
pub const PAYLOAD_TOO_LARGE: Reply = Reply::Fixed(413, r#"{"error":"payload too large"}"#);
pub const HEADERS_TOO_LARGE: Reply = Reply::Fixed(431, r#"{"error":"request headers too large"}"#);

pub fn route(method: &str, target: &str, auth: Option<&[u8]>, body: &[u8], ctx: &Ctx) -> Job {
    let path = target.split_once('?').map_or(target, |(path, _)| path);
    match (method, path) {
        ("GET", "/feed") => return Job::Feed,
        ("GET", "/health") => return Job::Health,
        ("POST", "/posts") => return create(auth, body, ctx).unwrap_or_else(Job::Done),
        _ => {}
    }
    let Some(rest) = path.strip_prefix("/posts/") else {
        return Job::Done(NOT_FOUND);
    };
    match method {
        "GET" if !rest.contains('/') => match positive_int(rest) {
            Some(id) => Job::Post(id),
            None => Job::Done(INVALID_POST_ID),
        },
        "POST" => match rest.strip_suffix("/like") {
            Some(id) if !id.contains('/') => like(auth, id, ctx).unwrap_or_else(Job::Done),
            _ => Job::Done(NOT_FOUND),
        },
        _ => Job::Done(NOT_FOUND),
    }
}

fn create(auth: Option<&[u8]>, body: &[u8], ctx: &Ctx) -> Result<Job, Reply> {
    let user = ctx.cfg.jwt.verify(auth)?;
    Ok(Job::Create { user_id: user.id, author: user.name, body: post_body(body)? })
}

/// Auth first, then the id (SPEC.md).
fn like(auth: Option<&[u8]>, id: &str, ctx: &Ctx) -> Result<Job, Reply> {
    let user = ctx.cfg.jwt.verify(auth)?;
    let post_id = positive_int(id).ok_or(INVALID_POST_ID)?;
    Ok(Job::Like { user_id: user.id, post_id })
}

/// `{"body":"..."}`, parsed like Express's `express.json()`: an empty body has no fields, and the top level
/// must be an object or an array. Returns the trimmed body.
fn post_body(raw: &[u8]) -> Result<String, Reply> {
    match raw.iter().find(|b| !b.is_ascii_whitespace()) {
        None => return Err(BODY_REQUIRED),
        Some(b'{' | b'[') => {}
        Some(_) => return Err(MALFORMED_JSON),
    }
    let value: Value = serde_json::from_slice(raw).map_err(|_| MALFORMED_JSON)?;
    let body = value.get("body").and_then(Value::as_str).map_or("", str::trim);
    if body.is_empty() {
        return Err(BODY_REQUIRED);
    }
    // Characters, as Postgres's char_length() counts them in the CHECK constraint.
    if body.chars().count() > MAX_POST_CHARS {
        return Err(BODY_TOO_LONG);
    }
    Ok(body.to_owned())
}

/// A positive integer in plain decimal digits; `None` for "0", "-1", "abc", "1.5", "".
pub fn positive_int(s: &str) -> Option<i64> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok().filter(|&n: &i64| n > 0)
}

/// Runs the request's one query (SPEC.md "Endpoints").
pub async fn execute(job: Job, ctx: &'static Ctx) -> Reply {
    match run(job, ctx).await {
        Ok(reply) => reply,
        Err(e) => {
            eprintln!("[error] {}", db::describe(&e));
            INTERNAL
        }
    }
}

async fn run(job: Job, ctx: &'static Ctx) -> Result<Reply, tokio_postgres::Error> {
    let db = &ctx.db;
    Ok(match job {
        Job::Done(reply) => reply,
        Job::Health => {
            let error = match db.read() {
                Some(c) => c.client.query(&c.st.ping, &[]).await.err().map(|e| db::describe(&e)),
                None => Some("no database connection".into()),
            };
            Reply::Health { uptime_s: ctx.cfg.started.elapsed().as_secs(), error }
        }
        Job::Feed => {
            let Some(c) = db.read() else { return Ok(no_connection()) };
            Reply::Feed(c.client.query(&c.st.feed, &[]).await?)
        }
        Job::Post(id) => {
            let Some(c) = db.read() else { return Ok(no_connection()) };
            match c.client.query_opt(&c.st.post, &[&id]).await? {
                Some(row) => Reply::Post(row),
                None => POST_NOT_FOUND,
            }
        }
        Job::Create { user_id, author, body } => {
            let Some(c) = db.write() else { return Ok(no_connection()) };
            let row = c.client.query_one(&c.st.create, &[&user_id, &body]).await?;
            let Ts(created_at) = row.try_get(1)?;
            Reply::Created { id: row.try_get(0)?, created_at, body, author }
        }
        Job::Like { user_id, post_id } => {
            let Some(c) = db.write() else { return Ok(no_connection()) };
            match c.client.execute(&c.st.like, &[&user_id, &post_id]).await {
                Ok(inserted) => Reply::Liked { inserted: inserted > 0, post_id },
                // The foreign key: the post doesn't exist.
                Err(e) if e.code() == Some(&SqlState::FOREIGN_KEY_VIOLATION) => POST_NOT_FOUND,
                Err(e) => return Err(e),
            }
        }
    })
}

fn no_connection() -> Reply {
    eprintln!("[error] no database connection");
    INTERNAL
}

/// Writes the JSON body; returns the status.
pub fn write_body(reply: &Reply, out: &mut Vec<u8>) -> u16 {
    match try_write_body(reply, out) {
        Ok(status) => status,
        Err(e) => {
            eprintln!("[error] {}", db::describe(&e));
            out.clear();
            try_write_body(&INTERNAL, out).unwrap_or(500)
        }
    }
}

fn try_write_body(reply: &Reply, out: &mut Vec<u8>) -> Result<u16, tokio_postgres::Error> {
    Ok(match reply {
        Reply::Fixed(status, body) => {
            out.extend_from_slice(body.as_bytes());
            *status
        }
        Reply::Health { uptime_s, error: None } => {
            out.extend_from_slice(br#"{"status":"ok","db":"ok","uptime_s":"#);
            json::int(out, *uptime_s);
            out.push(b'}');
            200
        }
        Reply::Health { error: Some(error), .. } => {
            out.extend_from_slice(br#"{"status":"degraded","db":"unreachable","error":"#);
            json::string(out, error);
            out.push(b'}');
            503
        }
        Reply::Feed(rows) => {
            out.extend_from_slice(br#"{"posts":["#);
            for (i, row) in rows.iter().enumerate() {
                if i > 0 {
                    out.push(b',');
                }
                post_row(out, row)?;
            }
            out.extend_from_slice(b"]}");
            200
        }
        Reply::Post(row) => {
            out.extend_from_slice(br#"{"post":"#);
            post_row(out, row)?;
            out.push(b'}');
            200
        }
        Reply::Created { id, created_at, body, author } => {
            out.extend_from_slice(br#"{"post":"#);
            post(out, *id, body, *created_at, author, 0);
            out.push(b'}');
            201
        }
        Reply::Liked { inserted, post_id } => {
            out.extend_from_slice(if *inserted {
                br#"{"liked":true,"already_liked":false,"post_id":"#.as_slice()
            } else {
                br#"{"liked":true,"already_liked":true,"post_id":"#
            });
            json::int(out, *post_id);
            out.push(b'}');
            if *inserted { 201 } else { 200 }
        }
    })
}

/// A row of the post SELECT: id, body, created_at, username, like_count.
fn post_row(out: &mut Vec<u8>, row: &Row) -> Result<(), tokio_postgres::Error> {
    let Ts(created_at) = row.try_get(2)?;
    post(out, row.try_get(0)?, row.try_get(1)?, created_at, row.try_get(3)?, row.try_get(4)?);
    Ok(())
}

/// The post object, keys in SPEC order.
fn post(out: &mut Vec<u8>, id: i64, body: &str, created_at: i64, author: &str, like_count: i32) {
    out.extend_from_slice(br#"{"id":"#);
    json::int(out, id);
    out.extend_from_slice(br#","body":"#);
    json::string(out, body);
    out.extend_from_slice(br#","created_at":""#);
    json::timestamp(out, created_at);
    out.extend_from_slice(br#"","author":"#);
    json::string(out, author);
    out.extend_from_slice(br#","like_count":"#);
    json::int(out, like_count);
    out.push(b'}');
}
