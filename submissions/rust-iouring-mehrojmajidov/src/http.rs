//! HTTP/1.1: requests parsed in place from the receive buffer, and the response heads.

pub const MAX_HEAD: usize = 16 * 1024;
pub const MAX_BODY: usize = 64 * 1024;

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Method {
    Get,
    Post,
    Other,
}

pub struct Request<'a> {
    pub method: Method,
    /// The request target without its query string.
    pub path: &'a [u8],
    pub authorization: Option<&'a [u8]>,
    pub body: &'a [u8],
    pub gzip: bool,
    pub close: bool,
}

pub enum Parsed<'a> {
    /// More bytes are needed. `wants_continue`: the client waits for `100 Continue` before sending its body.
    Partial { wants_continue: bool },
    /// Not a request this server can frame. The connection cannot be reused.
    Bad,
    TooLarge,
    /// A whole request and the number of bytes it took.
    Complete(Request<'a>, usize),
}

fn trim(mut v: &[u8]) -> &[u8] {
    while let [b' ' | b'\t', rest @ ..] = v {
        v = rest;
    }
    while let [rest @ .., b' ' | b'\t'] = v {
        v = rest;
    }
    v
}

fn contains_token(value: &[u8], token: &[u8]) -> bool {
    value.split(|&b| b == b',').any(|t| trim(t).eq_ignore_ascii_case(token))
}

/// `gzip` is listed and not switched off with `q=0`.
fn accepts_gzip(value: &[u8]) -> bool {
    value.split(|&b| b == b',').any(|item| {
        let mut parts = item.split(|&b| b == b';');
        let coding = trim(parts.next().unwrap_or(b""));
        if !coding.eq_ignore_ascii_case(b"gzip") && coding != b"*" {
            return false;
        }
        !parts.any(|p| {
            let p = trim(p);
            p.len() >= 3 && p[..2].eq_ignore_ascii_case(b"q=") && p[2..].iter().all(|&c| c == b'0' || c == b'.')
        })
    })
}

pub fn parse(buf: &[u8]) -> Parsed<'_> {
    let Some(head_len) = buf.windows(4).position(|w| w == b"\r\n\r\n") else {
        return if buf.len() > MAX_HEAD { Parsed::Bad } else { Parsed::Partial { wants_continue: false } };
    };
    if head_len > MAX_HEAD {
        return Parsed::Bad;
    }
    let mut lines = buf[..head_len].split(|&b| b == b'\n').map(|l| l.strip_suffix(b"\r").unwrap_or(l));

    let mut first = lines.next().unwrap_or(b"").splitn(3, |&b| b == b' ');
    let (Some(method), Some(target), Some(version)) = (first.next(), first.next(), first.next()) else {
        return Parsed::Bad;
    };
    if !version.starts_with(b"HTTP/1.") || target.is_empty() {
        return Parsed::Bad;
    }
    let method = match method {
        b"GET" => Method::Get,
        b"POST" => Method::Post,
        _ => Method::Other,
    };
    let path = target.split(|&b| b == b'?').next().unwrap_or(target);

    let (mut authorization, mut gzip, mut body_len) = (None, false, 0usize);
    let (mut close, mut keep_alive, mut wants_continue) = (false, false, false);
    for line in lines {
        let Some(colon) = line.iter().position(|&b| b == b':') else {
            return Parsed::Bad;
        };
        let (name, value) = (&line[..colon], trim(&line[colon + 1..]));
        if name.eq_ignore_ascii_case(b"content-length") {
            if value.is_empty() || !value.iter().all(u8::is_ascii_digit) {
                return Parsed::Bad;
            }
            if value.len() > 9 {
                return Parsed::TooLarge;
            }
            body_len = value.iter().fold(0, |a, &d| a * 10 + (d - b'0') as usize);
        } else if name.eq_ignore_ascii_case(b"authorization") {
            authorization = Some(value);
        } else if name.eq_ignore_ascii_case(b"accept-encoding") {
            gzip = accepts_gzip(value);
        } else if name.eq_ignore_ascii_case(b"connection") {
            close |= contains_token(value, b"close");
            keep_alive |= contains_token(value, b"keep-alive");
        } else if name.eq_ignore_ascii_case(b"expect") {
            wants_continue = value.eq_ignore_ascii_case(b"100-continue");
        } else if name.eq_ignore_ascii_case(b"transfer-encoding") {
            return Parsed::Bad;
        }
    }
    if body_len > MAX_BODY {
        return Parsed::TooLarge;
    }
    let body_at = head_len + 4;
    let total = body_at + body_len;
    if buf.len() < total {
        return Parsed::Partial { wants_continue: wants_continue && buf.len() == body_at };
    }
    let close = close || (version == b"HTTP/1.0" && !keep_alive);
    Parsed::Complete(Request { method, path, authorization, body: &buf[body_at..total], gzip, close }, total)
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Route<'a> {
    Health,
    Feed,
    Post(&'a [u8]),
    Like(&'a [u8]),
    Create,
    NotFound,
}

pub fn route<'a>(method: Method, path: &'a [u8]) -> Route<'a> {
    let segment = |s: &'a [u8]| (!s.is_empty() && !s.contains(&b'/')).then_some(s);
    match (method, path) {
        (Method::Get, b"/feed") => Route::Feed,
        (Method::Get, b"/health") => Route::Health,
        (Method::Post, b"/posts") => Route::Create,
        (Method::Get, [b'/', b'p', b'o', b's', b't', b's', b'/', id @ ..]) => segment(id).map_or(Route::NotFound, Route::Post),
        (Method::Post, [b'/', b'p', b'o', b's', b't', b's', b'/', rest @ ..]) => {
            rest.strip_suffix(b"/like").and_then(segment).map_or(Route::NotFound, Route::Like)
        }
        _ => Route::NotFound,
    }
}

/// A post id: digits only, greater than zero, at most 2^53 - 1.
pub fn post_id(segment: &[u8]) -> Option<u64> {
    if segment.is_empty() || !segment.iter().all(u8::is_ascii_digit) {
        return None;
    }
    let digits = &segment[segment.iter().take_while(|&&d| d == b'0').count()..];
    if digits.is_empty() || digits.len() > 16 {
        return None;
    }
    let id = digits.iter().fold(0u64, |a, &d| a * 10 + (d - b'0') as u64);
    (id <= (1 << 53) - 1).then_some(id)
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Status {
    Ok,
    Created,
    BadRequest,
    Unauthorized,
    NotFound,
    TooLarge,
    Internal,
    Unavailable,
}

impl Status {
    fn line(self) -> &'static [u8] {
        match self {
            Status::Ok => b"HTTP/1.1 200 OK\r\n",
            Status::Created => b"HTTP/1.1 201 Created\r\n",
            Status::BadRequest => b"HTTP/1.1 400 Bad Request\r\n",
            Status::Unauthorized => b"HTTP/1.1 401 Unauthorized\r\n",
            Status::NotFound => b"HTTP/1.1 404 Not Found\r\n",
            Status::TooLarge => b"HTTP/1.1 413 Content Too Large\r\n",
            Status::Internal => b"HTTP/1.1 500 Internal Server Error\r\n",
            Status::Unavailable => b"HTTP/1.1 503 Service Unavailable\r\n",
        }
    }
}

pub const CONTINUE: &[u8] = b"HTTP/1.1 100 Continue\r\n\r\n";

pub fn head(out: &mut Vec<u8>, status: Status, body_len: usize, gzip: bool, close: bool) {
    out.extend_from_slice(status.line());
    out.extend_from_slice(b"Content-Type: application/json\r\n");
    if gzip {
        out.extend_from_slice(b"Content-Encoding: gzip\r\n");
    }
    if close {
        out.extend_from_slice(b"Connection: close\r\n");
    }
    out.extend_from_slice(b"Content-Length: ");
    crate::json::push_u64(out, body_len as u64);
    out.extend_from_slice(b"\r\n\r\n");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn complete(raw: &[u8]) -> (Request<'_>, usize) {
        match parse(raw) {
            Parsed::Complete(r, n) => (r, n),
            _ => panic!("not complete: {:?}", String::from_utf8_lossy(raw)),
        }
    }

    #[test]
    fn mload_requests() {
        let raw = b"GET /feed HTTP/1.1\r\nHost: 10.0.0.2\r\nUser-Agent: mload\r\nAccept-Encoding: gzip\r\n\r\n";
        let (r, n) = complete(raw);
        assert_eq!((r.method, r.path, r.gzip, r.close, n), (Method::Get, &b"/feed"[..], true, false, raw.len()));
        assert!(r.authorization.is_none() && r.body.is_empty());

        let raw = b"POST /posts HTTP/1.1\r\nHost: h\r\nauthorization:  Bearer a.b.c \r\nContent-Type: application/json\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}GET /next";
        let (r, n) = complete(raw);
        assert_eq!((r.method, r.path, r.body), (Method::Post, &b"/posts"[..], &b"{\"body\":\"x\"}"[..]));
        assert_eq!(r.authorization, Some(&b"Bearer a.b.c"[..]));
        assert_eq!(&raw[n..], b"GET /next", "a pipelined request stays in the buffer");
    }

    #[test]
    fn partial_and_bad() {
        assert!(matches!(parse(b""), Parsed::Partial { wants_continue: false }));
        assert!(matches!(parse(b"GET /feed HTTP/1.1\r\nHost: h\r\n"), Parsed::Partial { .. }));
        assert!(matches!(parse(b"POST /posts HTTP/1.1\r\nContent-Length: 5\r\n\r\nabc"), Parsed::Partial { wants_continue: false }));
        assert!(matches!(
            parse(b"POST /posts HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n"),
            Parsed::Partial { wants_continue: true }
        ));
        assert!(matches!(parse(b"nonsense\r\n\r\n"), Parsed::Bad));
        assert!(matches!(parse(b"GET /feed SPDY/3\r\n\r\n"), Parsed::Bad));
        assert!(matches!(parse(b"GET /feed HTTP/1.1\r\nno colon here\r\n\r\n"), Parsed::Bad));
        assert!(matches!(parse(b"POST /posts HTTP/1.1\r\nContent-Length: -1\r\n\r\n"), Parsed::Bad));
        assert!(matches!(parse(b"POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"), Parsed::Bad));
        assert!(matches!(parse(b"POST /posts HTTP/1.1\r\nContent-Length: 70000\r\n\r\n"), Parsed::TooLarge));
        assert!(matches!(parse(b"POST /posts HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n"), Parsed::TooLarge));
        assert!(matches!(parse(&[b'a'; MAX_HEAD + 1]), Parsed::Bad));
        let mut long = b"GET /feed HTTP/1.1\r\nX: ".to_vec();
        long.resize(MAX_HEAD + 100, b'a');
        long.extend_from_slice(b"\r\n\r\n");
        assert!(matches!(parse(&long), Parsed::Bad), "a complete head over the limit");
    }

    #[test]
    fn connection_and_encoding() {
        assert!(complete(b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n").0.close);
        assert!(complete(b"GET / HTTP/1.0\r\n\r\n").0.close);
        assert!(!complete(b"GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n").0.close);
        assert!(!complete(b"GET / HTTP/1.1\r\n\r\n").0.gzip);
        assert!(complete(b"GET / HTTP/1.1\r\nAccept-Encoding: deflate, GZIP;q=0.5\r\n\r\n").0.gzip);
        assert!(!complete(b"GET / HTTP/1.1\r\nAccept-Encoding: gzip;q=0, br\r\n\r\n").0.gzip);
        assert!(!complete(b"GET / HTTP/1.1\r\nAccept-Encoding: identity\r\n\r\n").0.gzip);
        assert_eq!(complete(b"GET /feed?x=1&y=/2 HTTP/1.1\r\n\r\n").0.path, b"/feed");
    }

    #[test]
    fn routes() {
        use Method::*;
        assert_eq!(route(Get, b"/feed"), Route::Feed);
        assert_eq!(route(Get, b"/health"), Route::Health);
        assert_eq!(route(Post, b"/posts"), Route::Create);
        assert_eq!(route(Get, b"/posts/42"), Route::Post(b"42"));
        assert_eq!(route(Get, b"/posts/abc"), Route::Post(b"abc"));
        assert_eq!(route(Post, b"/posts/42/like"), Route::Like(b"42"));
        assert_eq!(route(Post, b"/posts/abc/like"), Route::Like(b"abc"));
        for (m, p) in [
            (Get, &b"/nope"[..]), (Get, b"/"), (Get, b"/posts"), (Get, b"/posts/"), (Get, b"/posts/1/like"), (Get, b"/posts/1/"),
            (Post, b"/feed"), (Post, b"/posts/1"), (Post, b"/posts//like"), (Post, b"/posts/1/2/like"), (Other, b"/feed"),
            (Get, b"/feed/"), (Post, b"/posts/like"),
        ] {
            assert_eq!(route(m, p), Route::NotFound, "{:?} {}", m, String::from_utf8_lossy(p));
        }
    }

    #[test]
    fn post_ids() {
        assert_eq!(post_id(b"1"), Some(1));
        assert_eq!(post_id(b"500000"), Some(500_000));
        assert_eq!(post_id(b"007"), Some(7));
        assert_eq!(post_id(b"9007199254740991"), Some(9_007_199_254_740_991));
        for bad in [&b""[..], b"0", b"000", b"-1", b"abc", b"1.5", b"1e3", b"0x10", b" 1", b"9007199254740992", b"99999999999999999999"] {
            assert_eq!(post_id(bad), None, "{}", String::from_utf8_lossy(bad));
        }
    }

    #[test]
    fn heads() {
        let mut o = Vec::new();
        head(&mut o, Status::Ok, 5102, false, false);
        assert_eq!(o, b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 5102\r\n\r\n");
        o.clear();
        head(&mut o, Status::Created, 9, true, true);
        assert_eq!(o, b"HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nConnection: close\r\nContent-Length: 9\r\n\r\n");
    }
}
