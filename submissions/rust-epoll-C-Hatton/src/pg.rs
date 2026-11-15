//! Postgres v3 wire protocol: a blocking handshake (trust, cleartext or SCRAM-SHA-256) that prepares the
//! SPEC's statements once per connection, then encoders for the per-request Bind/Execute messages that the
//! event loop pipelines over the now non-blocking socket.

use hmac::{Hmac, Mac};
use sha2::{Digest, Sha256};
use std::io::{self, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::os::fd::IntoRawFd;
use std::time::Duration;

#[derive(Clone)]
pub struct PgConfig {
    pub host: String,
    pub port: u16,
    pub user: String,
    pub password: String,
    pub db: String,
}

fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let hex = |c: u8| (c as char).to_digit(16);
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let (Some(h), Some(l)) = (hex(b[i + 1]), hex(b[i + 2])) {
                out.push((h * 16 + l) as u8);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// `postgres://user:password@host:port/db?params`
pub fn parse_url(url: &str) -> PgConfig {
    let rest = url
        .strip_prefix("postgres://")
        .or_else(|| url.strip_prefix("postgresql://"))
        .unwrap_or(url);
    let rest = rest.split('?').next().unwrap_or("");
    let (userinfo, hostpart) = match rest.rfind('@') {
        Some(i) => (&rest[..i], &rest[i + 1..]),
        None => ("", rest),
    };
    let (user, password) = match userinfo.find(':') {
        Some(i) => (pct_decode(&userinfo[..i]), pct_decode(&userinfo[i + 1..])),
        None => (pct_decode(userinfo), String::new()),
    };
    let (hostport, db) = match hostpart.find('/') {
        Some(i) => (&hostpart[..i], pct_decode(&hostpart[i + 1..])),
        None => (hostpart, String::new()),
    };
    let (host, port) = if let Some(h) = hostport.strip_prefix('[') {
        // [ipv6]:port
        let end = h.find(']').unwrap_or(h.len());
        let port = h[end..].strip_prefix("]:").and_then(|p| p.parse().ok()).unwrap_or(5432);
        (h[..end].to_string(), port)
    } else {
        match hostport.rfind(':') {
            Some(i) => (hostport[..i].to_string(), hostport[i + 1..].parse().unwrap_or(5432)),
            None => (hostport.to_string(), 5432),
        }
    };
    let host = if host.is_empty() { "localhost".into() } else { host };
    let user = if user.is_empty() { std::env::var("USER").unwrap_or_default() } else { user };
    let db = if db.is_empty() { user.clone() } else { db };
    PgConfig { host, port, user, password, db }
}

// ---- statements ----

const POST_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, \
(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
FROM posts p JOIN users u ON u.id = p.user_id";

pub const ST_FEED: u8 = b'f';
pub const ST_POST: u8 = b'p';
pub const ST_CREATE: u8 = b'c';
pub const ST_LIKE: u8 = b'l';
pub const ST_HEALTH: u8 = b'h';

const OID_INT8: u32 = 20;
const OID_TEXT: u32 = 25;

fn statements() -> Vec<(u8, String, Vec<u32>)> {
    vec![
        (ST_FEED, format!("{POST_SQL} ORDER BY p.created_at DESC, p.id DESC LIMIT 20"), vec![]),
        (ST_POST, format!("{POST_SQL} WHERE p.id = $1"), vec![OID_INT8]),
        (
            ST_CREATE,
            "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at".into(),
            vec![OID_INT8, OID_TEXT],
        ),
        (
            ST_LIKE,
            "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING".into(),
            vec![OID_INT8, OID_INT8],
        ),
        (ST_HEALTH, "SELECT 1".into(), vec![]),
    ]
}

// ---- message encoding ----

#[inline]
fn begin(w: &mut Vec<u8>, t: u8) -> usize {
    w.push(t);
    let at = w.len();
    w.extend_from_slice(&[0; 4]);
    at
}

#[inline]
fn finish(w: &mut Vec<u8>, at: usize) {
    let len = (w.len() - at) as u32;
    w[at..at + 4].copy_from_slice(&len.to_be_bytes());
}

#[inline]
fn i16be(w: &mut Vec<u8>, v: i16) {
    w.extend_from_slice(&v.to_be_bytes());
}

/// Bind (unnamed portal) + Execute. `pfmt`/`rfmt` are format codes (0 text, 1 binary).
#[inline]
fn bind_exec(w: &mut Vec<u8>, stmt: u8, pfmt: &[i16], params: &[&[u8]], rfmt: &[i16]) {
    let at = begin(w, b'B');
    w.push(0);
    w.push(stmt);
    w.push(0);
    i16be(w, pfmt.len() as i16);
    for &f in pfmt {
        i16be(w, f);
    }
    i16be(w, params.len() as i16);
    for p in params {
        w.extend_from_slice(&(p.len() as i32).to_be_bytes());
        w.extend_from_slice(p);
    }
    i16be(w, rfmt.len() as i16);
    for &f in rfmt {
        i16be(w, f);
    }
    finish(w, at);
    w.extend_from_slice(b"E\0\0\0\x09\0\0\0\0\0");
}

/// Post rows: id text, body text, created_at binary, username text, like_count text. The text integers copy
/// straight into the JSON and are shorter on the wire than their binary forms.
const POST_RFMT: [i16; 5] = [0, 0, 1, 0, 0];

pub fn enc_feed(w: &mut Vec<u8>) {
    bind_exec(w, ST_FEED, &[], &[], &POST_RFMT);
}
pub fn enc_post(w: &mut Vec<u8>, id: i64) {
    bind_exec(w, ST_POST, &[1], &[&id.to_be_bytes()], &POST_RFMT);
}
pub fn enc_health(w: &mut Vec<u8>) {
    bind_exec(w, ST_HEALTH, &[], &[], &[]);
}
/// Returns id as text, created_at binary.
pub fn enc_create(w: &mut Vec<u8>, user_id: i64, body: &[u8]) {
    bind_exec(w, ST_CREATE, &[1, 0], &[&user_id.to_be_bytes(), body], &[0, 1]);
}
pub fn enc_like(w: &mut Vec<u8>, user_id: i64, post_id: i64) {
    bind_exec(w, ST_LIKE, &[1], &[&user_id.to_be_bytes(), &post_id.to_be_bytes()], &[]);
}
#[inline]
pub fn enc_sync(w: &mut Vec<u8>) {
    w.extend_from_slice(b"S\0\0\0\x04");
}

// ---- handshake ----

fn err(msg: impl Into<String>) -> io::Error {
    io::Error::other(msg.into())
}

fn read_msg(s: &mut TcpStream) -> io::Result<(u8, Vec<u8>)> {
    let mut h = [0u8; 5];
    s.read_exact(&mut h)?;
    let len = u32::from_be_bytes([h[1], h[2], h[3], h[4]]) as usize;
    if !(4..=1 << 24).contains(&len) {
        return Err(err("bad message length"));
    }
    let mut b = vec![0u8; len - 4];
    s.read_exact(&mut b)?;
    Ok((h[0], b))
}

/// The human-readable part of an ErrorResponse.
pub fn error_text(p: &[u8]) -> String {
    let mut code = String::new();
    let mut msg = String::new();
    for f in p.split(|&c| c == 0) {
        if let Some((&t, v)) = f.split_first() {
            match t {
                b'C' => code = String::from_utf8_lossy(v).into_owned(),
                b'M' => msg = String::from_utf8_lossy(v).into_owned(),
                _ => {}
            }
        }
    }
    format!("{code} {msg}")
}

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

fn b64(data: &[u8]) -> String {
    let mut o = String::new();
    for ch in data.chunks(3) {
        let n = (ch[0] as u32) << 16 | (*ch.get(1).unwrap_or(&0) as u32) << 8 | *ch.get(2).unwrap_or(&0) as u32;
        for k in 0..4 {
            if k <= ch.len() {
                o.push(B64[(n >> (18 - 6 * k) & 63) as usize] as char);
            } else {
                o.push('=');
            }
        }
    }
    o
}

fn unb64(s: &str) -> io::Result<Vec<u8>> {
    let mut o = Vec::new();
    let mut acc = 0u32;
    let mut bits = 0;
    for c in s.bytes() {
        if c == b'=' {
            break;
        }
        let v = B64.iter().position(|&x| x == c).ok_or_else(|| err("bad base64"))? as u32;
        acc = acc << 6 | v;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            o.push((acc >> bits) as u8);
        }
    }
    Ok(o)
}

fn hmac(key: &[u8], data: &[u8]) -> [u8; 32] {
    let mut m = Hmac::<Sha256>::new_from_slice(key).expect("hmac");
    m.update(data);
    m.finalize().into_bytes().into()
}

fn send_password(s: &mut TcpStream, pw: &str) -> io::Result<()> {
    let mut w = Vec::new();
    let at = begin(&mut w, b'p');
    w.extend_from_slice(pw.as_bytes());
    w.push(0);
    finish(&mut w, at);
    s.write_all(&w)
}

fn scram(s: &mut TcpStream, password: &str) -> io::Result<()> {
    let mut raw = [0u8; 18];
    unsafe { libc::getrandom(raw.as_mut_ptr().cast(), raw.len(), 0) };
    let nonce = b64(&raw);
    let first_bare = format!("n=,r={nonce}");
    let first = format!("n,,{first_bare}");
    let mut w = Vec::new();
    let at = begin(&mut w, b'p');
    w.extend_from_slice(b"SCRAM-SHA-256\0");
    w.extend_from_slice(&(first.len() as i32).to_be_bytes());
    w.extend_from_slice(first.as_bytes());
    finish(&mut w, at);
    s.write_all(&w)?;

    let (t, b) = read_msg(s)?;
    if t == b'E' {
        return Err(err(error_text(&b)));
    }
    if t != b'R' || b.len() < 4 || u32::from_be_bytes([b[0], b[1], b[2], b[3]]) != 11 {
        return Err(err("expected SASLContinue"));
    }
    let server_first = String::from_utf8_lossy(&b[4..]).into_owned();
    let (mut r, mut salt, mut iters) = (String::new(), Vec::new(), 0u32);
    for kv in server_first.split(',') {
        if let Some(v) = kv.strip_prefix("r=") {
            r = v.into();
        } else if let Some(v) = kv.strip_prefix("s=") {
            salt = unb64(v)?;
        } else if let Some(v) = kv.strip_prefix("i=") {
            iters = v.parse().map_err(|_| err("bad iteration count"))?;
        }
    }
    if !r.starts_with(&nonce) || iters == 0 {
        return Err(err("bad SCRAM server-first"));
    }
    // PBKDF2-HMAC-SHA256
    let mac = Hmac::<Sha256>::new_from_slice(password.as_bytes()).expect("hmac");
    let mut m = mac.clone();
    m.update(&salt);
    m.update(&1u32.to_be_bytes());
    let mut u: [u8; 32] = m.finalize().into_bytes().into();
    let mut salted = u;
    for _ in 1..iters {
        let mut m = mac.clone();
        m.update(&u);
        u = m.finalize().into_bytes().into();
        for k in 0..32 {
            salted[k] ^= u[k];
        }
    }
    let client_key = hmac(&salted, b"Client Key");
    let stored_key: [u8; 32] = Sha256::digest(client_key).into();
    let final_wo_proof = format!("c=biws,r={r}");
    let auth_msg = format!("{first_bare},{server_first},{final_wo_proof}");
    let sig = hmac(&stored_key, auth_msg.as_bytes());
    let proof: Vec<u8> = client_key.iter().zip(sig.iter()).map(|(a, b)| a ^ b).collect();
    let fin = format!("{final_wo_proof},p={}", b64(&proof));
    let mut w = Vec::new();
    let at = begin(&mut w, b'p');
    w.extend_from_slice(fin.as_bytes());
    finish(&mut w, at);
    s.write_all(&w)
}

fn set_int(fd: i32, level: i32, opt: i32, v: i32) {
    unsafe {
        libc::setsockopt(fd, level, opt, (&v as *const i32).cast(), 4);
    }
}

/// Connects, authenticates and prepares every statement. Returns a non-blocking socket.
pub fn connect(cfg: &PgConfig) -> io::Result<i32> {
    let addr = (cfg.host.as_str(), cfg.port)
        .to_socket_addrs()?
        .next()
        .ok_or_else(|| err("no address for database host"))?;
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(3))?;
    s.set_nodelay(true)?;
    s.set_read_timeout(Some(Duration::from_secs(10)))?;
    s.set_write_timeout(Some(Duration::from_secs(10)))?;

    let mut w = Vec::new();
    w.extend_from_slice(&[0; 4]);
    w.extend_from_slice(&196608i32.to_be_bytes());
    for (k, v) in [("user", cfg.user.as_str()), ("database", cfg.db.as_str()), ("client_encoding", "UTF8")] {
        w.extend_from_slice(k.as_bytes());
        w.push(0);
        w.extend_from_slice(v.as_bytes());
        w.push(0);
    }
    w.push(0);
    let len = w.len() as u32;
    w[..4].copy_from_slice(&len.to_be_bytes());
    s.write_all(&w)?;

    loop {
        let (t, b) = read_msg(&mut s)?;
        match t {
            b'R' => {
                let code = u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
                match code {
                    0 | 12 => {}
                    3 => send_password(&mut s, &cfg.password)?,
                    10 => {
                        let mechs = String::from_utf8_lossy(&b[4..]);
                        if !mechs.split('\0').any(|m| m == "SCRAM-SHA-256") {
                            return Err(err("server offers no SCRAM-SHA-256"));
                        }
                        scram(&mut s, &cfg.password)?;
                    }
                    _ => return Err(err(format!("unsupported auth method {code}"))),
                }
            }
            b'E' => return Err(err(error_text(&b))),
            b'Z' => break,
            _ => {} // ParameterStatus, BackendKeyData, NoticeResponse
        }
    }

    let mut w = Vec::new();
    for (name, sql, oids) in statements() {
        let at = begin(&mut w, b'P');
        w.push(name);
        w.push(0);
        w.extend_from_slice(sql.as_bytes());
        w.push(0);
        i16be(&mut w, oids.len() as i16);
        for o in oids {
            w.extend_from_slice(&o.to_be_bytes());
        }
        finish(&mut w, at);
    }
    enc_sync(&mut w);
    s.write_all(&w)?;
    loop {
        let (t, b) = read_msg(&mut s)?;
        match t {
            b'1' | b'N' | b'S' => {}
            b'E' => return Err(err(error_text(&b))),
            b'Z' => break,
            _ => return Err(err(format!("unexpected message {:?} while preparing", t as char))),
        }
    }

    s.set_nonblocking(true)?;
    let fd = s.into_raw_fd();
    // A database link that goes silent turns into an error after a minute instead of hanging forever.
    set_int(fd, libc::SOL_SOCKET, libc::SO_KEEPALIVE, 1);
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_KEEPIDLE, 10);
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_KEEPINTVL, 5);
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_KEEPCNT, 6);
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_USER_TIMEOUT, 60_000);
    Ok(fd)
}
