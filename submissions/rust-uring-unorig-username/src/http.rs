//! HTTP/1.1 request parsing, routing, JWT checks and the small output helpers (shared with rust-tokio).
use std::time::{SystemTime, UNIX_EPOCH};

use hmac::{Hmac, Mac};
use sha2::Sha256;

use crate::json;

pub const MAX_REQ: usize = 64 * 1024; // header + body cap per request

pub enum Kind {
    Health,
    Feed,
    Post(i64),
    Create(i64, String, String),
    Like(i64, i64),
    Fail(u16, &'static str),
}

pub struct Req {
    pub kind: Kind,
    pub gzip: bool,
    pub close: bool,
}

pub enum Parse {
    Done(Req, usize),
    More(bool), // true: headers are in and the client waits for "100 Continue"
    Bad,
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

/// `2025-12-31T23:59:19.409Z`: UTC, truncated to milliseconds.
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

/// Parses one request at the start of buf. Auth and validation happen here, on the borrowed bytes,
/// so what crosses the database await is small and owned.
pub fn parse(buf: &[u8], key: &Hmac<Sha256>) -> Parse {
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
