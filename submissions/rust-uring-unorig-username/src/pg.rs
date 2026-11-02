//! The few pieces of the Postgres v3 wire protocol this server needs: startup with trust, cleartext or
//! SCRAM-SHA-256 auth, Parse of the named statements once per connection, then Bind/Execute/Sync per request
//! with binary results. Startup is blocking (once per connection); the request path only appends bytes to a
//! buffer and walks the replies in place.
use std::io::{self, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::os::fd::{AsRawFd, IntoRawFd};
use std::time::Duration;

use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};

pub struct Url {
    user: String,
    pass: String,
    host: String,
    port: u16,
    db: String,
}

fn pct(s: &str) -> String {
    let b = s.as_bytes();
    let mut o = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let Some(v) = s.get(i + 1..i + 3).and_then(|h| u8::from_str_radix(h, 16).ok()) {
                o.push(v);
                i += 3;
                continue;
            }
        }
        o.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&o).into_owned()
}

/// postgres://user:pass@host:port/db (query parameters are ignored: no TLS).
pub fn parse_url(u: &str) -> Option<Url> {
    let r = u.strip_prefix("postgres://").or_else(|| u.strip_prefix("postgresql://"))?;
    let r = r.split('?').next()?;
    let (auth, r) = r.rsplit_once('@').unwrap_or(("", r));
    let (user, pass) = auth.split_once(':').unwrap_or((auth, ""));
    let (hp, db) = r.split_once('/').unwrap_or((r, ""));
    let (host, port) = match hp.rsplit_once(':') {
        Some((h, p)) if !h.ends_with(']') || hp.starts_with('[') => (h, p.parse().ok()?),
        _ => (hp, 5432),
    };
    let host = host.trim_start_matches('[').trim_end_matches(']');
    let user = if user.is_empty() { std::env::var("USER").unwrap_or_default() } else { pct(user) };
    let db = if db.is_empty() { user.clone() } else { pct(db) };
    Some(Url { user, pass: pct(pass), host: if host.is_empty() { "localhost".into() } else { host.into() }, port, db })
}

fn be32(b: &[u8]) -> usize {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]]) as usize
}

fn msg(out: &mut Vec<u8>, t: u8, f: impl FnOnce(&mut Vec<u8>)) {
    out.push(t);
    let at = out.len();
    out.extend_from_slice(&[0; 4]);
    f(out);
    let n = (out.len() - at) as u32;
    out[at..at + 4].copy_from_slice(&n.to_be_bytes());
}

/// Reads one message: (type, payload).
fn read_msg(s: &mut TcpStream) -> io::Result<(u8, Vec<u8>)> {
    let mut h = [0u8; 5];
    s.read_exact(&mut h)?;
    let n = be32(&h[1..]);
    if !(4..=1 << 24).contains(&n) {
        return Err(io::Error::other("bad message length"));
    }
    let mut p = vec![0; n - 4];
    s.read_exact(&mut p)?;
    Ok((h[0], p))
}

/// One field of an ErrorResponse (empty if absent).
fn field(p: &[u8], want: u8) -> &[u8] {
    let mut i = 0;
    while i < p.len() && p[i] != 0 {
        let t = p[i];
        let e = i + 1 + p[i + 1..].iter().position(|&x| x == 0).unwrap_or(p.len() - i - 1);
        if t == want {
            return &p[i + 1..e];
        }
        i = e + 1;
    }
    &[]
}

/// The 'M' (message) field of an ErrorResponse, and its SQLSTATE.
pub fn err_fields(p: &[u8]) -> (&[u8], &[u8]) {
    (field(p, b'M'), field(p, b'C'))
}

/// FATAL or PANIC: the server is ending the session, so the error belongs to no request.
pub fn fatal(p: &[u8]) -> bool {
    matches!(field(p, b'V'), b"FATAL" | b"PANIC")
}

fn b64(s: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut o = String::new();
    for c in s.chunks(3) {
        let v = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            o.push(if i <= c.len() { A[(v >> (18 - 6 * i) & 63) as usize] as char } else { '=' });
        }
    }
    o
}

fn unb64(s: &str) -> Option<Vec<u8>> {
    let mut o = Vec::new();
    let (mut acc, mut bits) = (0u32, 0);
    for c in s.bytes().take_while(|&c| c != b'=') {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
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

fn hmac(k: &[u8], m: &[u8]) -> [u8; 32] {
    let mut h = Hmac::<Sha256>::new_from_slice(k).unwrap();
    h.update(m);
    h.finalize().into_bytes().into()
}

fn bad(m: impl Into<String>) -> io::Error {
    io::Error::other(m.into())
}

/// SCRAM-SHA-256 (RFC 7677) client side, channel binding off.
fn scram(s: &mut TcpStream, pass: &str) -> io::Result<()> {
    let mut raw = [0u8; 18];
    std::fs::File::open("/dev/urandom")?.read_exact(&mut raw)?;
    let nonce = b64(&raw);
    let first_bare = format!("n=,r={nonce}");
    let mut out = Vec::new();
    msg(&mut out, b'p', |o| {
        o.extend_from_slice(b"SCRAM-SHA-256\0");
        let cf = format!("n,,{first_bare}");
        o.extend_from_slice(&(cf.len() as u32).to_be_bytes());
        o.extend_from_slice(cf.as_bytes());
    });
    s.write_all(&out)?;
    let (t, p) = read_msg(s)?;
    if t == b'E' {
        return Err(bad(String::from_utf8_lossy(err_fields(&p).0)));
    }
    if t != b'R' || p.len() < 4 || be32(&p) != 11 {
        return Err(bad("unexpected SCRAM reply"));
    }
    let server_first = String::from_utf8_lossy(&p[4..]).into_owned();
    let (mut r, mut salt, mut iters) = ("", None, 0u32);
    for kv in server_first.split(',') {
        match kv.split_at_checked(2) {
            Some(("r=", v)) => r = v,
            Some(("s=", v)) => salt = unb64(v),
            Some(("i=", v)) => iters = v.parse().unwrap_or(0),
            _ => {}
        }
    }
    let salt = salt.ok_or_else(|| bad("SCRAM: no salt"))?;
    if !r.starts_with(&nonce) || iters == 0 {
        return Err(bad("SCRAM: bad server nonce"));
    }
    // PBKDF2-HMAC-SHA256, one block
    let mut u = hmac(pass.as_bytes(), &[salt.as_slice(), &1u32.to_be_bytes()].concat());
    let mut salted = u;
    for _ in 1..iters {
        u = hmac(pass.as_bytes(), &u);
        salted.iter_mut().zip(u).for_each(|(a, b)| *a ^= b);
    }
    let ckey = hmac(&salted, b"Client Key");
    let stored: [u8; 32] = Sha256::digest(ckey).into();
    let final_bare = format!("c=biws,r={r}");
    let auth_msg = format!("{first_bare},{server_first},{final_bare}");
    let sig = hmac(&stored, auth_msg.as_bytes());
    let proof: Vec<u8> = ckey.iter().zip(sig).map(|(a, b)| a ^ b).collect();
    out.clear();
    msg(&mut out, b'p', |o| o.extend_from_slice(format!("{final_bare},p={}", b64(&proof)).as_bytes()));
    s.write_all(&out)?;
    let (t, p) = read_msg(s)?;
    if t == b'E' {
        return Err(bad(String::from_utf8_lossy(err_fields(&p).0)));
    }
    let want = format!("v={}", b64(&hmac(&hmac(&salted, b"Server Key"), auth_msg.as_bytes())));
    if t != b'R' || p.len() < 4 || be32(&p) != 12 || p[4..] != *want.as_bytes() {
        return Err(bad("SCRAM: server signature mismatch"));
    }
    Ok(())
}

/// Connects, authenticates, prepares `stmts` (name, SQL, parameter type OIDs) and returns the non-blocking fd.
pub fn connect(u: &Url, stmts: &[(&str, &str, &[u32])]) -> io::Result<i32> {
    let addr = (u.host.as_str(), u.port).to_socket_addrs()?.next().ok_or_else(|| bad("no address"))?;
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(1))?;
    s.set_read_timeout(Some(Duration::from_secs(10)))?;
    s.set_nodelay(true)?;
    // A connection that goes silent (no reset) errors after 60 s, so its requests fail instead of hanging forever.
    // Not sooner: a shorter stall that then resolves answers every request, while failing early turns it into 500s.
    for (lvl, opt, v) in [
        (libc::SOL_SOCKET, libc::SO_KEEPALIVE, 1),
        (libc::IPPROTO_TCP, libc::TCP_KEEPIDLE, 30),
        (libc::IPPROTO_TCP, libc::TCP_KEEPINTVL, 5),
        (libc::IPPROTO_TCP, libc::TCP_KEEPCNT, 6),
        (libc::IPPROTO_TCP, libc::TCP_USER_TIMEOUT, 60_000),
    ] {
        unsafe { libc::setsockopt(s.as_raw_fd(), lvl, opt, &v as *const i32 as *const libc::c_void, 4) };
    }
    let mut out = Vec::new();
    out.extend_from_slice(&[0; 4]);
    out.extend_from_slice(&196608u32.to_be_bytes());
    for (k, v) in [("user", &u.user), ("database", &u.db)] {
        out.extend_from_slice(k.as_bytes());
        out.push(0);
        out.extend_from_slice(v.as_bytes());
        out.push(0);
    }
    out.push(0);
    let n = out.len() as u32;
    out[..4].copy_from_slice(&n.to_be_bytes());
    s.write_all(&out)?;
    loop {
        let (t, p) = read_msg(&mut s)?;
        match t {
            b'R' if p.len() >= 4 => match be32(&p) {
                0 => {}
                3 => {
                    out.clear();
                    msg(&mut out, b'p', |o| {
                        o.extend_from_slice(u.pass.as_bytes());
                        o.push(0);
                    });
                    s.write_all(&out)?;
                }
                10 if p.windows(14).any(|w| w == b"SCRAM-SHA-256\0") => scram(&mut s, &u.pass)?,
                a => return Err(bad(format!("unsupported auth method {a}"))),
            },
            b'E' => return Err(bad(String::from_utf8_lossy(err_fields(&p).0))),
            b'Z' => break,
            _ => {} // ParameterStatus, BackendKeyData, notices
        }
    }
    out.clear();
    for (name, sql, types) in stmts {
        msg(&mut out, b'P', |o| {
            o.extend_from_slice(name.as_bytes());
            o.push(0);
            o.extend_from_slice(sql.as_bytes());
            o.push(0);
            o.extend_from_slice(&(types.len() as u16).to_be_bytes());
            for t in *types {
                o.extend_from_slice(&t.to_be_bytes());
            }
        });
    }
    msg(&mut out, b'S', |_| {});
    s.write_all(&out)?;
    loop {
        let (t, p) = read_msg(&mut s)?;
        match t {
            b'E' => return Err(bad(String::from_utf8_lossy(err_fields(&p).0))),
            b'Z' => break,
            _ => {}
        }
    }
    s.set_nonblocking(true)?;
    Ok(s.into_raw_fd())
}

pub const SYNC: &[u8] = b"S\0\0\0\x04";

/// Appends Bind (unnamed portal, all results binary) + Execute for one request; the caller adds SYNC.
/// `params`: (bytes, binary?) per parameter.
pub fn query(out: &mut Vec<u8>, stmt: &[u8], params: &[(&[u8], bool)]) {
    msg(out, b'B', |o| {
        o.push(0);
        o.extend_from_slice(stmt);
        o.push(0);
        o.extend_from_slice(&(params.len() as u16).to_be_bytes());
        for (_, bin) in params {
            o.extend_from_slice(&(*bin as u16).to_be_bytes());
        }
        o.extend_from_slice(&(params.len() as u16).to_be_bytes());
        for (p, _) in params {
            o.extend_from_slice(&(p.len() as u32).to_be_bytes());
            o.extend_from_slice(p);
        }
        o.extend_from_slice(&[0, 1, 0, 1]);
    });
    out.extend_from_slice(b"E\0\0\0\x09\0\0\0\0\0");
}

/// Splits `buf` into whole messages: (type, payload). Stops at a partial one.
pub struct Msgs<'a> {
    pub b: &'a [u8],
    pub p: usize,
}
impl<'a> Iterator for Msgs<'a> {
    type Item = (u8, &'a [u8]);
    fn next(&mut self) -> Option<Self::Item> {
        let r = &self.b[self.p..];
        if r.len() < 5 {
            return None;
        }
        let n = be32(&r[1..]);
        if n < 4 || r.len() < 1 + n {
            return None;
        }
        self.p += 1 + n;
        Some((r[0], &r[5..1 + n]))
    }
}

/// The columns of a DataRow payload; None on a NULL or a short row.
pub fn cols<const N: usize>(p: &[u8]) -> Option<[&[u8]; N]> {
    if p.len() < 2 || u16::from_be_bytes([p[0], p[1]]) as usize != N {
        return None;
    }
    let mut out = [&[][..]; N];
    let mut i = 2;
    for c in out.iter_mut() {
        let n = i32::from_be_bytes(p.get(i..i + 4)?.try_into().ok()?);
        if n < 0 {
            return None;
        }
        *c = p.get(i + 4..i + 4 + n as usize)?;
        i += 4 + n as usize;
    }
    Some(out)
}

pub fn int(c: &[u8]) -> Option<i64> {
    match c.len() {
        8 => Some(i64::from_be_bytes(c.try_into().ok()?)),
        4 => Some(i32::from_be_bytes(c.try_into().ok()?) as i64),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn urls_and_b64() {
        let u = parse_url("postgres://feed:p%40ss@db.local:6543/feed_bench?sslmode=disable").unwrap();
        assert_eq!((u.user.as_str(), u.pass.as_str(), u.host.as_str(), u.port, u.db.as_str()), ("feed", "p@ss", "db.local", 6543, "feed_bench"));
        let u = parse_url("postgres://postgres@localhost/x").unwrap();
        assert_eq!((u.pass.as_str(), u.port), ("", 5432));
        for s in [&b""[..], b"a", b"ab", b"abc", b"abcd"] {
            assert_eq!(unb64(&b64(s)).unwrap(), s);
        }
        assert_eq!(b64(b"ab"), "YWI=");
    }
    #[test]
    fn rows() {
        let mut row = vec![0, 2, 0, 0, 0, 8];
        row.extend_from_slice(&42i64.to_be_bytes());
        row.extend_from_slice(&[0, 0, 0, 2, b'h', b'i']);
        let [a, b] = cols::<2>(&row).unwrap();
        assert_eq!((int(a), b), (Some(42), &b"hi"[..]));
        assert!(cols::<3>(&row).is_none());
        let mut out = Vec::new();
        query(&mut out, b"p", &[(&7i64.to_be_bytes(), true)]);
        out.extend_from_slice(SYNC);
        let m: Vec<_> = Msgs { b: &out, p: 0 }.map(|(t, _)| t).collect();
        assert_eq!(m, b"BES");
    }
}
