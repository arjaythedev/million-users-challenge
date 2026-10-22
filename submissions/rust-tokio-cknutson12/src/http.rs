//! A minimal HTTP/1.1 server side: parse requests out of a read buffer, render responses.
//!
//! Parsing copies out only what the handlers need (route, auth result, the body of POST /posts), so the read
//! buffer can be a per-thread scratch buffer: an idle keep-alive connection owns no buffer at all.

use std::cell::RefCell;

use crate::auth::{self, User};

/// Headers + body of one request may not exceed this (bodies over 16 KB are out of scope).
pub const MAX_REQUEST: usize = 64 * 1024;

pub enum Route {
    Health,
    Feed,
    Post(Option<i64>),
    Create { user: Result<User, &'static str>, body: Vec<u8> },
    Like { user: Result<User, &'static str>, id: Option<i64> },
    NotFound,
}

pub struct Request {
    pub route: Route,
    pub meta: Meta,
}

/// What the response depends on besides the route.
pub struct Meta {
    pub gzip: bool,
    pub close: bool,
    /// HTTP/1.0 client that asked for keep-alive: it needs the header back.
    pub keep_alive_10: bool,
}

pub struct Parsed {
    pub requests: Vec<Request>,
    /// Bytes of `data` used by `requests`; the rest is an incomplete request.
    pub consumed: usize,
    /// The incomplete request sent `Expect: 100-continue` and waits for us.
    pub expects_continue: bool,
    /// Unparseable request: answer 400 (or 413) and close.
    pub error: Option<u16>,
}

pub struct AuthCtx<'a> {
    pub mac: &'a hmac::Hmac<sha2::Sha256>,
    pub now: f64,
}

pub fn parse_all(data: &[u8], auth: &AuthCtx) -> Parsed {
    let mut out = Parsed { requests: Vec::new(), consumed: 0, expects_continue: false, error: None };
    while out.consumed < data.len() {
        let rest = &data[out.consumed..];
        let mut headers = [httparse::EMPTY_HEADER; 64];
        let mut req = httparse::Request::new(&mut headers);
        let head_len = match req.parse(rest) {
            Ok(httparse::Status::Complete(n)) => n,
            Ok(httparse::Status::Partial) => {
                if rest.len() > MAX_REQUEST {
                    out.error = Some(413);
                }
                break;
            }
            Err(_) => {
                out.error = Some(400);
                break;
            }
        };
        let version = req.version.unwrap_or(1);
        let (mut content_length, mut gzip, mut authorization) = (0usize, false, None);
        let (mut conn_close, mut conn_keep_alive, mut expect_continue) = (false, false, false);
        for h in req.headers.iter() {
            let name = h.name.as_bytes();
            if name.eq_ignore_ascii_case(b"content-length") {
                match std::str::from_utf8(h.value).ok().and_then(|v| v.trim().parse().ok()) {
                    Some(n) => content_length = n,
                    None => {
                        out.error = Some(400);
                        return out;
                    }
                }
            } else if name.eq_ignore_ascii_case(b"authorization") {
                authorization = Some(h.value);
            } else if name.eq_ignore_ascii_case(b"accept-encoding") {
                gzip = accepts_gzip(h.value);
            } else if name.eq_ignore_ascii_case(b"connection") {
                conn_close |= contains_token(h.value, b"close");
                conn_keep_alive |= contains_token(h.value, b"keep-alive");
            } else if name.eq_ignore_ascii_case(b"expect") {
                expect_continue = h.value.eq_ignore_ascii_case(b"100-continue");
            } else if name.eq_ignore_ascii_case(b"transfer-encoding") {
                // Chunked request bodies are not supported (nothing in the test or load sends them).
                out.error = Some(400);
                return out;
            }
        }
        if head_len + content_length > MAX_REQUEST {
            out.error = Some(413);
            break;
        }
        if rest.len() < head_len + content_length {
            out.expects_continue = expect_continue;
            break;
        }
        let body = &rest[head_len..head_len + content_length];
        let close = conn_close || (version == 0 && !conn_keep_alive);
        let route = route(req.method.unwrap_or(""), req.path.unwrap_or(""), authorization, body, auth);
        out.requests.push(Request { route, meta: Meta { gzip, close, keep_alive_10: version == 0 && !close } });
        out.consumed += head_len + content_length;
        if close {
            break;
        }
    }
    out
}

fn route(method: &str, target: &str, authorization: Option<&[u8]>, body: &[u8], a: &AuthCtx) -> Route {
    let path = target.split_once('?').map_or(target, |(p, _)| p);
    let user = || auth::verify(authorization, a.mac, a.now);
    match (method, path) {
        ("GET", "/health") => Route::Health,
        ("GET", "/feed") => Route::Feed,
        ("POST", "/posts") => Route::Create { user: user(), body: body.to_vec() },
        (_, p) => match p.strip_prefix("/posts/") {
            Some(rest) => match (method, rest.split_once('/')) {
                ("GET", None) => Route::Post(auth::positive_int(rest)),
                ("POST", Some((id, "like"))) => Route::Like { user: user(), id: auth::positive_int(id) },
                _ => Route::NotFound,
            },
            None => Route::NotFound,
        },
    }
}

fn accepts_gzip(v: &[u8]) -> bool {
    // "gzip", "gzip, deflate, br", "deflate;q=0.5, gzip"; a "gzip;q=0" refusal is treated as no.
    v.split(|&b| b == b',').any(|item| {
        let mut parts = item.split(|&b| b == b';');
        let coding = trim(parts.next().unwrap_or_default());
        (coding.eq_ignore_ascii_case(b"gzip") || coding == b"*")
            && !parts.any(|p| {
                let p = trim(p);
                p.len() >= 3 && p[..2].eq_ignore_ascii_case(b"q=") && p[2..].iter().all(|&b| b == b'0' || b == b'.')
            })
    })
}

fn contains_token(v: &[u8], token: &[u8]) -> bool {
    v.split(|&b| b == b',').any(|t| trim(t).eq_ignore_ascii_case(token))
}

fn trim(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|b| !b.is_ascii_whitespace()).unwrap_or(s.len());
    let end = s.iter().rposition(|b| !b.is_ascii_whitespace()).map_or(start, |i| i + 1);
    &s[start..end]
}

thread_local! {
    static GZIP: RefCell<Option<libdeflater::Compressor>> = const { RefCell::new(None) };
}

pub struct Render {
    pub gzip_level: i32,
    /// Bodies shorter than this go out uncompressed even when the client accepts gzip.
    pub gzip_min: usize,
}

/// The full response: status line, headers and (maybe gzipped) body.
pub fn render(status: u16, body: &[u8], req: Option<&Meta>, opts: &Render) -> Vec<u8> {
    let reason = match status {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        401 => "Unauthorized",
        404 => "Not Found",
        413 => "Payload Too Large",
        503 => "Service Unavailable",
        _ => "Internal Server Error",
    };
    let gzip = req.is_some_and(|r| r.gzip) && body.len() >= opts.gzip_min;
    let close = req.map_or(true, |r| r.close);
    let mut out = Vec::with_capacity(160 + body.len());
    out.extend_from_slice(b"HTTP/1.1 ");
    crate::json::int(&mut out, status);
    out.push(b' ');
    out.extend_from_slice(reason.as_bytes());
    out.extend_from_slice(b"\r\nContent-Type: application/json\r\n");
    if close {
        out.extend_from_slice(b"Connection: close\r\n");
    } else if req.is_some_and(|r| r.keep_alive_10) {
        out.extend_from_slice(b"Connection: keep-alive\r\n");
    }
    if gzip {
        out.extend_from_slice(b"Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
        GZIP.with_borrow_mut(|c| {
            let c = c.get_or_insert_with(|| {
                libdeflater::Compressor::new(libdeflater::CompressionLvl::new(opts.gzip_level).unwrap_or_default())
            });
            let mut packed = vec![0; c.gzip_compress_bound(body.len())];
            let n = c.gzip_compress(body, &mut packed).expect("gzip bound is large enough");
            out.extend_from_slice(b"Content-Length: ");
            crate::json::int(&mut out, n);
            out.extend_from_slice(b"\r\n\r\n");
            out.extend_from_slice(&packed[..n]);
        });
    } else {
        out.extend_from_slice(b"Content-Length: ");
        crate::json::int(&mut out, body.len());
        out.extend_from_slice(b"\r\n\r\n");
        out.extend_from_slice(body);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gzip_header() {
        assert!(accepts_gzip(b"gzip"));
        assert!(accepts_gzip(b"deflate, gzip;q=0.8, br"));
        assert!(!accepts_gzip(b"gzip;q=0"));
        assert!(!accepts_gzip(b"identity"));
    }

    fn parse(data: &[u8]) -> Parsed {
        let mac = <hmac::Hmac<sha2::Sha256> as hmac::Mac>::new_from_slice(b"k").unwrap();
        parse_all(data, &AuthCtx { mac: &mac, now: 0.0 })
    }

    #[test]
    fn pipelined_and_partial() {
        let p = parse(b"GET /feed HTTP/1.1\r\nHost: x\r\n\r\nGET /posts/5?x=1 HTTP/1.1\r\n\r\nPOST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\n{\"bo");
        assert_eq!(p.requests.len(), 2);
        assert!(matches!(p.requests[0].route, Route::Feed));
        assert!(matches!(p.requests[1].route, Route::Post(Some(5))));
        assert_eq!(p.consumed, 60);
        assert!(p.error.is_none());
    }

    #[test]
    fn routes() {
        let r = |s: &str| parse(s.as_bytes()).requests.pop().unwrap().route;
        assert!(matches!(r("GET /posts/0 HTTP/1.1\r\n\r\n"), Route::Post(None)));
        assert!(matches!(r("GET /posts/1.5 HTTP/1.1\r\n\r\n"), Route::Post(None)));
        assert!(matches!(r("GET /posts/ HTTP/1.1\r\n\r\n"), Route::Post(None)));
        assert!(matches!(r("GET /posts/1/ HTTP/1.1\r\n\r\n"), Route::NotFound));
        assert!(matches!(r("GET /nope HTTP/1.1\r\n\r\n"), Route::NotFound));
        assert!(matches!(r("GET /feed/ HTTP/1.1\r\n\r\n"), Route::NotFound));
        assert!(matches!(
            r("POST /posts/abc/like HTTP/1.1\r\n\r\n"),
            Route::Like { user: Err(auth::MISSING), id: None }
        ));
    }
}
