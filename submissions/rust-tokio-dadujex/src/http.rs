//! A small HTTP/1.1 server loop: keep-alive, pipelined requests, Content-Length bodies, `Expect: 100-continue`.
//!
//! An idle connection holds no buffer: the read buffer is freed as soon as a request is consumed, and
//! responses are built in the thread's scratch buffers and usually written in a single `write`. So an idle
//! keep-alive connection costs its task and socket registration (well under 1 KB) instead of the 16-24 KB of
//! read and write buffers a general-purpose server keeps per connection.

use std::io::{self, ErrorKind};

use tokio::{io::Interest, net::TcpStream};

use crate::{
    Ctx,
    api::{self, Job, Reply},
};

const MAX_HEAD_BYTES: usize = 16 * 1024;
const MAX_BODY_BYTES: usize = 64 * 1024;
const MAX_HEADERS: usize = 64;
/// Bodies at least this big are gzipped when the client accepts it. In practice only the feed (~5.3 KB, ~1.8 KB
/// gzipped: 2 TCP segments instead of 4); a post or a like is a few hundred bytes, where gzip saves nothing.
const GZIP_MIN_BYTES: usize = 1024;
const CONTINUE: &[u8] = b"HTTP/1.1 100 Continue\r\n\r\n";

#[derive(Clone, Copy)]
pub struct Meta {
    gzip: bool,
    close: bool,
    http10: bool,
}

enum Parsed {
    /// Not a whole request yet.
    Partial { expect_continue: bool },
    Request { job: Job, meta: Meta, len: usize },
    /// Not a request we can serve: answer and close.
    Invalid(Reply),
}

/// Serves one connection until the client closes it.
pub async fn serve(stream: TcpStream, ctx: &'static Ctx) {
    let mut buf = Vec::new();
    let mut continued = false;
    loop {
        let (job, meta, len) = match parse(&buf, ctx) {
            Parsed::Request { job, meta, len } => (job, meta, len),
            Parsed::Partial { expect_continue } => {
                if expect_continue && !continued {
                    continued = true;
                    if !write_all(&stream, CONTINUE).await {
                        return;
                    }
                }
                if !fill(&stream, &mut buf).await {
                    return;
                }
                continue;
            }
            Parsed::Invalid(reply) => {
                let meta = Meta { gzip: false, close: true, http10: false };
                (Job::Done(reply), meta, buf.len())
            }
        };
        continued = false;
        if len >= buf.len() {
            buf = Vec::new(); // the common case: nothing pipelined behind it, so hold no buffer while idle
        } else {
            buf.drain(..len);
        }
        let reply = match job {
            Job::Done(reply) => reply,
            // Boxed: the database futures are only alive during a request, so they stay out of the
            // connection's task, which is all an idle connection keeps.
            job => Box::pin(api::execute(job, ctx)).await,
        };
        if !respond(&stream, reply, meta, ctx).await || meta.close {
            return;
        }
    }
}

/// Waits for the socket to be readable and appends what arrived. False when the connection is done.
async fn fill(stream: &TcpStream, buf: &mut Vec<u8>) -> bool {
    loop {
        if stream.readable().await.is_err() {
            return false;
        }
        match read_some(stream, buf) {
            Ok(0) => return false,
            Ok(_) => return true,
            Err(e) if e.kind() == ErrorKind::WouldBlock => continue,
            Err(_) => return false,
        }
    }
}

/// Reads into a stack buffer (not part of the connection's task) and keeps only the bytes that arrived.
fn read_some(stream: &TcpStream, buf: &mut Vec<u8>) -> io::Result<usize> {
    let mut chunk = [0u8; 4096];
    let n = stream.try_read(&mut chunk)?;
    buf.extend_from_slice(&chunk[..n]);
    if n > 0 && n < chunk.len() {
        // A short read drained the socket: clear tokio's readiness now, so waiting for the next request doesn't
        // cost a read() that returns EAGAIN. Safe on a current-thread runtime: the I/O driver can't record new
        // readiness between the read above and this clear (tokio ignores a clear with an older tick), and bytes
        // that arrive later raise a new edge-triggered event. Closed states are never cleared.
        let _ = stream.try_io(Interest::READABLE, || Err::<(), _>(ErrorKind::WouldBlock.into()));
    }
    Ok(n)
}

fn parse(buf: &[u8], ctx: &Ctx) -> Parsed {
    let mut headers = [httparse::EMPTY_HEADER; MAX_HEADERS];
    let mut req = httparse::Request::new(&mut headers);
    let head_len = match req.parse(buf) {
        Ok(httparse::Status::Complete(n)) => n,
        Ok(httparse::Status::Partial) if buf.len() <= MAX_HEAD_BYTES => {
            return Parsed::Partial { expect_continue: false };
        }
        Ok(httparse::Status::Partial) | Err(httparse::Error::TooManyHeaders) => {
            return Parsed::Invalid(api::HEADERS_TOO_LARGE);
        }
        Err(_) => return Parsed::Invalid(api::BAD_REQUEST),
    };
    let http10 = req.version == Some(0);
    let mut meta = Meta { gzip: false, close: http10, http10 };
    let (mut body_len, mut auth, mut expect_continue) = (0, None, false);
    for h in req.headers.iter() {
        let (name, value) = (h.name, h.value);
        if name.eq_ignore_ascii_case("content-length") {
            match std::str::from_utf8(value).ok().and_then(|v| v.trim().parse().ok()) {
                Some(n) => body_len = n,
                None => return Parsed::Invalid(api::BAD_REQUEST),
            }
        } else if name.eq_ignore_ascii_case("authorization") {
            auth = Some(value);
        } else if name.eq_ignore_ascii_case("accept-encoding") {
            meta.gzip = contains(value, b"gzip");
        } else if name.eq_ignore_ascii_case("connection") {
            if contains(value, b"close") {
                meta.close = true;
            } else if contains(value, b"keep-alive") {
                meta.close = false;
            }
        } else if name.eq_ignore_ascii_case("expect") {
            expect_continue = value.eq_ignore_ascii_case(b"100-continue");
        } else if name.eq_ignore_ascii_case("transfer-encoding") {
            return Parsed::Invalid(api::LENGTH_REQUIRED);
        }
    }
    if body_len > MAX_BODY_BYTES {
        return Parsed::Invalid(api::PAYLOAD_TOO_LARGE);
    }
    let len = head_len + body_len;
    if buf.len() < len {
        return Parsed::Partial { expect_continue };
    }
    let (Some(method), Some(path)) = (req.method, req.path) else {
        return Parsed::Invalid(api::BAD_REQUEST);
    };
    let job = api::route(method, path, auth, &buf[head_len..len], ctx);
    Parsed::Request { job, meta, len }
}

/// Case-insensitive substring search, for header values.
fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.windows(needle.len()).any(|w| w.eq_ignore_ascii_case(needle))
}

/// Builds the response in the thread's scratch buffer and writes it, usually in one `write` that the socket
/// takes whole. Only if the socket buffer is full does the rest get copied out and wait for writability.
async fn respond(stream: &TcpStream, reply: Reply, meta: Meta, ctx: &Ctx) -> bool {
    let rest = {
        let mut out = ctx.out.borrow_mut();
        out.clear();
        render(&reply, meta, ctx, &mut out);
        match stream.try_write(&out) {
            Ok(n) if n == out.len() => return true,
            Ok(n) => out[n..].to_vec(),
            Err(e) if e.kind() == ErrorKind::WouldBlock => out.to_vec(),
            Err(_) => return false,
        }
    };
    drop(reply);
    write_all(stream, &rest).await
}

async fn write_all(stream: &TcpStream, mut data: &[u8]) -> bool {
    while !data.is_empty() {
        match stream.try_write(data) {
            Ok(0) => return false,
            Ok(n) => data = &data[n..],
            Err(e) if e.kind() == ErrorKind::WouldBlock => {
                if stream.writable().await.is_err() {
                    return false;
                }
            }
            Err(_) => return false,
        }
    }
    true
}

fn render(reply: &Reply, meta: Meta, ctx: &Ctx, out: &mut Vec<u8>) {
    let mut body = ctx.body.borrow_mut();
    body.clear();
    let status = api::write_body(reply, &mut body);
    if meta.gzip
        && body.len() >= GZIP_MIN_BYTES
        && let Some(gzip) = &ctx.gzip
    {
        let mut gzip = gzip.borrow_mut();
        let (compressor, gz) = &mut *gzip;
        let bound = compressor.gzip_compress_bound(body.len());
        if gz.len() < bound {
            gz.resize(bound, 0);
        }
        if let Ok(n) = compressor.gzip_compress(&body, gz) {
            head(out, status, n, true, meta);
            out.extend_from_slice(&gz[..n]);
            return;
        }
    }
    head(out, status, body.len(), false, meta);
    out.extend_from_slice(&body);
}

fn head(out: &mut Vec<u8>, status: u16, len: usize, gzip: bool, meta: Meta) {
    out.extend_from_slice(match status {
        200 => b"HTTP/1.1 200 OK\r\n".as_slice(),
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        411 => b"HTTP/1.1 411 Length Required\r\n",
        413 => b"HTTP/1.1 413 Payload Too Large\r\n",
        431 => b"HTTP/1.1 431 Request Header Fields Too Large\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    });
    out.extend_from_slice(b"Content-Type: application/json\r\nContent-Length: ");
    out.extend_from_slice(itoa::Buffer::new().format(len).as_bytes());
    out.extend_from_slice(b"\r\n");
    if gzip {
        out.extend_from_slice(b"Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
    }
    if meta.close {
        out.extend_from_slice(b"Connection: close\r\n");
    } else if meta.http10 {
        out.extend_from_slice(b"Connection: keep-alive\r\n");
    }
    out.extend_from_slice(b"\r\n");
}
