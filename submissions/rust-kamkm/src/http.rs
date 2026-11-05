//! HTTP/1.1 request parsing (with strict framing: no request smuggling), routing, the JWT check
//! and the small output helpers. Parsing works on the borrowed bytes; the only owned data that
//! crosses into the database path is a create's body text and username.
use std::time::{SystemTime, UNIX_EPOCH};

use hmac::{Hmac, Mac};
use sha2::Sha256;

use crate::json;

pub const MAX_REQ: usize = 64 * 1024; // header + body cap per request

pub enum Kind {
    Health,
    Feed,
    Post(i64),
    Create(i64, String, String), // user id, username, trimmed body
    Like(i64, i64),
    Fail(u16, &'static str),
}

pub struct Req {
    pub kind: Kind,
    pub gzip: bool,
    pub close: bool,
}

pub enum Parse {
    Done(Req, usize),  // the request and the bytes it used
    More(bool),        // need more bytes; true: headers done, the client waits for 100 Continue
    Bad,               // framing violation: answer 400 and close
}

pub fn num(b: &mut Vec<u8>, v: i64) {
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

/// `2025-12-31T23:59:19.409Z` from Postgres binary microseconds (since 2000), truncated to ms.
pub fn ts(b: &mut Vec<u8>, pg_us: i64) {
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

fn ctl(c: u8) -> bool {
    c < 0x20 || c == 0x7f
}

/// Parses one request at the start of `buf`. A chunked body is decoded into `scratch` (owned),
/// a Content-Length body is borrowed in place.
pub fn parse(buf: &[u8], scratch: &mut Vec<u8>, key: &Hmac<Sha256>) -> Parse {
    let Some(he) = memchr::memmem::find(buf, b"\r\n\r\n") else {
        return if buf.len() > MAX_REQ + 8192 { Parse::Bad } else { Parse::More(false) };
    };
    let head = &buf[..he + 2];
    let Some(le) = memchr::memchr(b'\r', head) else { return Parse::Bad };
    let mut parts = head[..le].split(|&c| c == b' ');
    let (Some(method), Some(target), Some(ver), None) = (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return Parse::Bad;
    };
    for part in [method, target, ver] {
        if part.is_empty() || part.iter().any(|&c| ctl(c)) {
            return Parse::Bad;
        }
    }
    let mut close = ver == b"HTTP/1.0";
    if !close && ver != b"HTTP/1.1" {
        return Parse::Bad;
    }
    let (mut auth, mut clen, mut gzip, mut expect) = (None, None, false, false);
    let (mut chunked, mut host) = (false, false);
    for line in head[le + 2..].split(|&c| c == b'\n') {
        let Some(line) = line.strip_suffix(b"\r") else { continue }; // the empty tail after the last \n
        if line.is_empty() {
            continue;
        }
        let Some(c) = memchr::memchr(b':', line) else { return Parse::Bad };
        let (k, v) = (&line[..c], line[c + 1..].trim_ascii());
        if k.is_empty() || k.iter().any(|&c| ctl(c)) || v.iter().any(|&c| ctl(c)) {
            return Parse::Bad;
        }
        if k.eq_ignore_ascii_case(b"host") {
            if host {
                return Parse::Bad;
            }
            host = true;
        } else if k.eq_ignore_ascii_case(b"content-length") {
            if v.is_empty() || !v.iter().all(u8::is_ascii_digit) {
                return Parse::Bad;
            }
            let Some(n) = std::str::from_utf8(v).ok().and_then(|v| v.parse::<usize>().ok()) else {
                return Parse::Bad;
            };
            if clen.is_some_and(|p| p != n) {
                return Parse::Bad; // disagreeing duplicates frame two different requests
            }
            clen = Some(n);
        } else if k.eq_ignore_ascii_case(b"transfer-encoding") {
            if chunked || !v.eq_ignore_ascii_case(b"chunked") {
                return Parse::Bad;
            }
            chunked = true;
        } else if k.eq_ignore_ascii_case(b"authorization") {
            auth = Some(v);
        } else if k.eq_ignore_ascii_case(b"accept-encoding") {
            gzip |= v.windows(4).any(|w| w.eq_ignore_ascii_case(b"gzip"));
        } else if k.eq_ignore_ascii_case(b"connection") {
            for tok in v.split(|&c| c == b',') {
                let tok = tok.trim_ascii();
                if tok.eq_ignore_ascii_case(b"close") {
                    close = true;
                } else if tok.eq_ignore_ascii_case(b"keep-alive") && ver == b"HTTP/1.0" {
                    close = false;
                }
            }
        } else if k.eq_ignore_ascii_case(b"expect") {
            expect = v.eq_ignore_ascii_case(b"100-continue");
        }
    }
    if chunked && clen.is_some() {
        return Parse::Bad;
    }
    if clen.is_some_and(|n| n > MAX_REQ) {
        return Parse::Bad;
    }
    if ver == b"HTTP/1.1" && !host {
        return Parse::Bad;
    }
    let bs = he + 4;
    if chunked {
        scratch.clear();
        let Some(used) = (match decode_chunks(&buf[bs..], scratch, MAX_REQ) {
            Ok(used) => used,
            Err(()) => return Parse::Bad,
        }) else {
            return Parse::More(expect && buf.len() == bs);
        };
        let kind = route(method, target, auth, &scratch[..], key);
        return Parse::Done(Req { kind, gzip, close }, bs + used);
    }
    let clen = clen.unwrap_or(0);
    if buf.len() < bs + clen {
        return Parse::More(expect && buf.len() == bs);
    }
    let kind = route(method, target, auth, &buf[bs..bs + clen], key);
    Parse::Done(Req { kind, gzip, close }, bs + clen)
}

/// Decodes a chunked body. `Ok(None)` wants more bytes; `Ok(Some(used))` consumed the whole
/// body including the final CRLF. Signed or oversized chunk sizes are framing violations.
fn decode_chunks(input: &[u8], out: &mut Vec<u8>, budget: usize) -> Result<Option<usize>, ()> {
    let mut off = 0;
    loop {
        let Some(end) = input[off..].windows(2).position(|w| w == b"\r\n") else {
            return Ok(None);
        };
        let size_line = input[off..off + end].split(|&c| c == b';').next().ok_or(())?;
        if size_line.is_empty() || !size_line.iter().all(u8::is_ascii_hexdigit) {
            return Err(());
        }
        let size = usize::from_str_radix(std::str::from_utf8(size_line).map_err(|_| ())?, 16).map_err(|_| ())?;
        off += end + 2;
        if size == 0 {
            if input.get(off..off + 2) == Some(b"\r\n") {
                return Ok(Some(off + 2));
            }
            return Ok(input[off..]
                .windows(4)
                .position(|w| w == b"\r\n\r\n")
                .map(|n| off + n + 4)); // trailers, then the end
        }
        if size > budget - out.len() {
            return Err(());
        }
        if input.len() - off < size + 2 {
            return Ok(None);
        }
        out.extend_from_slice(&input[off..off + size]);
        if &input[off + size..off + size + 2] != b"\r\n" {
            return Err(());
        }
        off += size + 2;
    }
}

fn route(method: &[u8], target: &[u8], auth: Option<&[u8]>, body: &[u8], key: &Hmac<Sha256>) -> Kind {
    let path = target.split(|&c| c == b'?').next().unwrap();
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
            match rest.iter().position(|&c| c == b'/') {
                // /posts/<id>: an empty id is a different route, not a bad one
                None if !rest.is_empty() => match method {
                    b"GET" => post_id(rest).map_or(Kind::Fail(400, "invalid post id"), Kind::Post),
                    _ => Kind::Fail(404, "not found"),
                },
                // /posts/<id>/like: the id must be there; auth comes before the id check
                Some(i) if i > 0 && &rest[i..] == b"/like" && method == b"POST" => match user(auth, key) {
                    Err(m) => Kind::Fail(401, m),
                    Ok((uid, _)) => post_id(&rest[..i]).map_or(Kind::Fail(400, "invalid post id"), |id| Kind::Like(uid, id)),
                },
                _ => Kind::Fail(404, "not found"),
            }
        }
        _ => Kind::Fail(404, "not found"),
    }
}

/// A positive decimal integer. Ids past i64 (out of scope) fold to i64::MAX, which is never found.
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
    Ok(t.to_owned())
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

/// Verifies `Authorization: Bearer <HS256 JWT>`: signature and expiry first (bad token), then
/// the payload shape (bad token payload).
fn user(h: Option<&[u8]>, key: &Hmac<Sha256>) -> Result<(i64, String), &'static str> {
    const BAD: &str = "invalid or expired token";
    const PAYLOAD: &str = "invalid token payload";
    let t = h.and_then(|h| h.strip_prefix(b"Bearer ")).ok_or("missing bearer token")?;
    let mut it = t.split(|&c| c == b'.');
    let (Some(h64), Some(p64), Some(s64), None) = (it.next(), it.next(), it.next(), it.next()) else { return Err(BAD) };
    let sig = b64url(s64).ok_or(BAD)?;
    let mut mac = key.clone(); // the clone has the key schedule done: one hash over the message
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
    // exp is required and checked on every request (rule 9); nbf is honored when present
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

    fn key() -> Hmac<Sha256> {
        Hmac::new_from_slice(b"million-users-challenge").unwrap()
    }

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
        assert_eq!(post_body(b"{\"body\":\"x\"} {}"), Err("malformed JSON body"));
        assert_eq!(post_body(format!("{{\"body\":\"{}\"}}", "\u{e9}".repeat(500)).as_bytes()).map(|s| s.len()), Ok(1000));
        assert_eq!(post_body(b"[]"), Err("body is required"));
        assert_eq!(post_body(b"{\"body\":null}"), Err("body is required"));
    }

    #[test]
    fn framing() {
        let mut scratch = Vec::new();
        let k = key();
        let bad: [&[u8]; 9] = [
            &b"GET /feed HTTP/1.1\r\n\r\n"[..],                    // missing Host on 1.1
            b"GET /feed\x00 HTTP/1.1\r\nHost: a\r\n\r\n",          // control byte in target
            b"GET /feed HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",   // duplicate Host
            b"POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: 0\r\n\r\n{}",
            b"POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: +2\r\n\r\n{}",
            b"POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n",
            b"POST /posts HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n",
            b"POST /posts HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: gzip\r\n\r\n",
            b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
        ];
        for b in bad {
            assert!(matches!(parse(b, &mut scratch, &k), Parse::Bad));
        }
        // tolerant: OWS around values, identical duplicate lengths, HTTP/1.0 without Host
        let ok: [&[u8]; 4] = [
            &b"POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length:\t2 \t\r\n\r\n{}"[..],
            b"POST /posts HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
            b"GET /feed HTTP/1.0\r\n\r\n",
            b"POST /posts HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: Chunked \t\r\n\r\n0\r\n\r\n",
        ];
        for b in ok {
            assert!(matches!(parse(b, &mut scratch, &k), Parse::Done(..)));
        }
        // chunked body, decoded and routed (no Expect header: no 100 Continue wanted)
        let mut b = b"POST /posts HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n{\"bod\r\n".to_vec();
        assert!(matches!(parse(&b, &mut scratch, &k), Parse::More(false)));
        b.extend_from_slice(b"7\r\ny\":\"x\"}\r\n0\r\n\r\n");
        let Parse::Done(r, used) = parse(&b, &mut scratch, &k) else {
            panic!("chunked body should parse");
        };
        assert_eq!(used, b.len());
        assert!(matches!(r.kind, Kind::Fail(401, "missing bearer token")));
    }

    #[test]
    fn routes() {
        let k = key();
        let mut scratch = Vec::new();
        let mut get = |p: &[u8]| match parse(format!("GET {} HTTP/1.1\r\nHost: a\r\n\r\n", std::str::from_utf8(p).unwrap()).as_bytes(), &mut scratch, &k) {
            Parse::Done(r, _) => r.kind,
            _ => panic!("GET should parse"),
        };
        assert!(matches!(get(b"/posts/"), Kind::Fail(404, _)));
        assert!(matches!(get(b"/posts/1"), Kind::Post(1)));
        assert!(matches!(get(b"/posts/abc"), Kind::Fail(400, "invalid post id")));
        assert!(matches!(get(b"/posts/1/extra"), Kind::Fail(404, _)));
        let mut post = |p: &[u8]| match parse(format!("POST {} HTTP/1.1\r\nHost: a\r\n\r\n", std::str::from_utf8(p).unwrap()).as_bytes(), &mut scratch, &k) {
            Parse::Done(r, _) => r.kind,
            _ => panic!("POST should parse"),
        };
        assert!(matches!(post(b"/posts//like"), Kind::Fail(404, _)));
        assert!(matches!(post(b"/posts/1/extra/like"), Kind::Fail(404, _)));
        assert!(matches!(post(b"/posts/abc/like"), Kind::Fail(401, "missing bearer token")));
    }
}
