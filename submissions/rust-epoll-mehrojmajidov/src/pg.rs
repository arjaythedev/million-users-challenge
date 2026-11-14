//! The Postgres v3 wire protocol, as far as this server needs it: the login (trust, clear-text password or
//! SCRAM-SHA-256), preparing the statements once per connection, and the Bind/Execute/Sync messages of a request.
//! The reply stream is parsed by the event loop (`db.rs`).

use crate::crypto::{b64_decode, b64_encode, pbkdf2, sha256, Hmac};
use crate::sys;
use std::io::{self, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::os::fd::IntoRawFd;
use std::time::Duration;

// The statements of SPEC.md, one per request. `SELECT 1` is for /health only.
const POST_SELECT: &str = "SELECT p.id, p.body, p.created_at, u.username, \
     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
     FROM posts p JOIN users u ON u.id = p.user_id";
const OID_INT8: u32 = 20;
const OID_TEXT: u32 = 25;

pub struct PgConfig {
    pub addrs: Vec<SocketAddr>,
    pub user: Vec<u8>,
    pub password: Vec<u8>,
    pub database: Vec<u8>,
}

fn percent_decode(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        let hex = |c: u8| (c as char).to_digit(16);
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
    out
}

impl PgConfig {
    /// Parses `postgres://user:password@host:port/database`.
    pub fn from_url(url: &str) -> Result<PgConfig, String> {
        let rest = url
            .strip_prefix("postgres://")
            .or_else(|| url.strip_prefix("postgresql://"))
            .ok_or("DATABASE_URL must start with postgres://")?;
        let rest = rest.split(['?', '#']).next().unwrap_or(rest);
        let (authority, path) = rest.split_once('/').unwrap_or((rest, ""));
        let (userinfo, hostport) = authority.rsplit_once('@').unwrap_or(("", authority));
        let (user, password) = userinfo.split_once(':').unwrap_or((userinfo, ""));
        let (host, port) = if let Some(v6) = hostport.strip_prefix('[') {
            let (h, p) = v6.split_once(']').ok_or("bad IPv6 host in DATABASE_URL")?;
            (h, p.strip_prefix(':').unwrap_or(""))
        } else {
            hostport.rsplit_once(':').unwrap_or((hostport, ""))
        };
        let port: u16 = if port.is_empty() { 5432 } else { port.parse().map_err(|_| "bad port in DATABASE_URL")? };
        let host = if host.is_empty() { "localhost" } else { host };
        let user = if user.is_empty() { b"postgres".to_vec() } else { percent_decode(user) };
        let database = if path.is_empty() { user.clone() } else { percent_decode(path) };
        let addrs: Vec<SocketAddr> = (String::from_utf8_lossy(&percent_decode(host)).as_ref(), port)
            .to_socket_addrs()
            .map_err(|e| format!("cannot resolve the database host: {e}"))?
            .collect();
        if addrs.is_empty() {
            return Err("the database host has no address".into());
        }
        Ok(PgConfig { addrs, user, password: percent_decode(password), database })
    }
}

/// The salted password of the last SCRAM login. The server sends the same salt for every connection of a role,
/// so only a thread's first login pays for the 4,096 PBKDF2 rounds.
#[derive(Default)]
pub struct ScramCache {
    salt: Vec<u8>,
    iterations: u32,
    salted: [u8; 32],
}

fn err(msg: impl Into<String>) -> io::Error {
    io::Error::new(io::ErrorKind::Other, msg.into())
}

fn put_msg(out: &mut Vec<u8>, ty: u8, body: impl FnOnce(&mut Vec<u8>)) {
    out.push(ty);
    let at = out.len();
    out.extend_from_slice(&[0; 4]);
    body(out);
    let len = (out.len() - at) as u32;
    out[at..at + 4].copy_from_slice(&len.to_be_bytes());
}

fn cstr(out: &mut Vec<u8>, s: &[u8]) {
    out.extend_from_slice(s);
    out.push(0);
}

fn read_msg(s: &mut TcpStream, buf: &mut Vec<u8>) -> io::Result<u8> {
    let mut head = [0u8; 5];
    s.read_exact(&mut head)?;
    let len = u32::from_be_bytes([head[1], head[2], head[3], head[4]]) as usize;
    if !(4..=1 << 20).contains(&len) {
        return Err(err("bad message length from Postgres"));
    }
    buf.resize(len - 4, 0);
    s.read_exact(buf)?;
    Ok(head[0])
}

/// The fields of an ErrorResponse or NoticeResponse: (severity, SQLSTATE, message).
pub fn error_fields(body: &[u8]) -> (&[u8], &[u8], &[u8]) {
    let (mut severity, mut code, mut message): (&[u8], &[u8], &[u8]) = (b"", b"", b"");
    for field in body.split(|&b| b == 0) {
        match field.first() {
            Some(b'V') => severity = &field[1..],
            Some(b'S') if severity.is_empty() => severity = &field[1..],
            Some(b'C') => code = &field[1..],
            Some(b'M') => message = &field[1..],
            _ => {}
        }
    }
    (severity, code, message)
}

fn server_error(body: &[u8]) -> io::Error {
    let (_, code, message) = error_fields(body);
    err(format!("Postgres: {} ({})", String::from_utf8_lossy(message), String::from_utf8_lossy(code)))
}

fn scram(s: &mut TcpStream, cfg: &PgConfig, cache: &mut ScramCache, buf: &mut Vec<u8>) -> io::Result<()> {
    let mut raw = [0u8; 18];
    if unsafe { sys::getrandom(raw.as_mut_ptr().cast(), raw.len(), 0) } != raw.len() as isize {
        return Err(err("getrandom failed"));
    }
    let nonce = b64_encode(&raw);
    let mut first_bare = b"n=,r=".to_vec();
    first_bare.extend_from_slice(&nonce);

    let mut out = Vec::new();
    put_msg(&mut out, b'p', |o| {
        cstr(o, b"SCRAM-SHA-256");
        o.extend_from_slice(&(first_bare.len() as u32 + 3).to_be_bytes());
        o.extend_from_slice(b"n,,");
        o.extend_from_slice(&first_bare);
    });
    s.write_all(&out)?;

    match read_msg(s, buf)? {
        b'R' if buf.len() >= 4 && buf[..4] == 11u32.to_be_bytes() => {}
        b'E' => return Err(server_error(buf)),
        _ => return Err(err("unexpected reply during SCRAM")),
    }
    let server_first = buf[4..].to_vec();
    let (mut r, mut salt, mut iterations) = (&b""[..], None, 0u32);
    for part in server_first.split(|&b| b == b',') {
        match part {
            [b'r', b'=', v @ ..] => r = v,
            [b's', b'=', v @ ..] => salt = b64_decode(v),
            [b'i', b'=', v @ ..] => iterations = core::str::from_utf8(v).ok().and_then(|t| t.parse().ok()).unwrap_or(0),
            _ => {}
        }
    }
    let salt = salt.ok_or_else(|| err("SCRAM: no salt"))?;
    if !r.starts_with(&nonce) || iterations == 0 {
        return Err(err("SCRAM: bad server-first message"));
    }
    if cache.salt != salt || cache.iterations != iterations {
        cache.salted = pbkdf2(&cfg.password, &salt, iterations);
        cache.salt = salt;
        cache.iterations = iterations;
    }

    let mut final_bare = b"c=biws,r=".to_vec();
    final_bare.extend_from_slice(r);
    let auth_message = [&first_bare[..], b",", &server_first, b",", &final_bare];
    let salted = Hmac::new(&cache.salted);
    let client_key = salted.mac(&[b"Client Key"]);
    let signature = Hmac::new(&sha256(&client_key)).mac(&auth_message);
    let proof: Vec<u8> = client_key.iter().zip(signature).map(|(k, g)| k ^ g).collect();

    out.clear();
    put_msg(&mut out, b'p', |o| {
        o.extend_from_slice(&final_bare);
        o.extend_from_slice(b",p=");
        o.extend_from_slice(&b64_encode(&proof));
    });
    s.write_all(&out)?;

    match read_msg(s, buf)? {
        b'R' if buf.len() >= 4 && buf[..4] == 12u32.to_be_bytes() => {}
        b'E' => return Err(server_error(buf)),
        _ => return Err(err("unexpected reply during SCRAM")),
    }
    let want = b64_encode(&Hmac::new(&salted.mac(&[b"Server Key"])).mac(&auth_message));
    if buf[4..].strip_prefix(b"v=") != Some(&want[..]) {
        return Err(err("SCRAM: the server's signature does not match"));
    }
    Ok(())
}

/// Connects, logs in and prepares the statements, all blocking with a timeout. Returns a non-blocking socket
/// with no reply outstanding.
pub fn connect(cfg: &PgConfig, cache: &mut ScramCache, timeout: Duration) -> io::Result<i32> {
    let mut last = err("no address");
    let mut stream = None;
    for addr in &cfg.addrs {
        match TcpStream::connect_timeout(addr, timeout) {
            Ok(s) => {
                stream = Some(s);
                break;
            }
            Err(e) => last = e,
        }
    }
    let mut s = stream.ok_or(last)?;
    s.set_read_timeout(Some(timeout))?;
    s.set_write_timeout(Some(timeout))?;
    s.set_nodelay(true)?;

    let mut out = Vec::with_capacity(1024);
    out.extend_from_slice(&[0; 4]);
    out.extend_from_slice(&196_608u32.to_be_bytes());
    cstr(&mut out, b"user");
    cstr(&mut out, &cfg.user);
    cstr(&mut out, b"database");
    cstr(&mut out, &cfg.database);
    out.push(0);
    let len = out.len() as u32;
    out[..4].copy_from_slice(&len.to_be_bytes());
    s.write_all(&out)?;

    let mut buf = Vec::new();
    loop {
        match read_msg(&mut s, &mut buf)? {
            b'R' if buf.len() >= 4 => match u32::from_be_bytes([buf[0], buf[1], buf[2], buf[3]]) {
                0 => {}
                3 => {
                    out.clear();
                    put_msg(&mut out, b'p', |o| cstr(o, &cfg.password));
                    s.write_all(&out)?;
                }
                10 => {
                    if !buf[4..].split(|&b| b == 0).any(|m| m == b"SCRAM-SHA-256") {
                        return Err(err("Postgres offers no SCRAM-SHA-256"));
                    }
                    scram(&mut s, cfg, cache, &mut buf)?;
                }
                other => return Err(err(format!("unsupported Postgres authentication method {other}"))),
            },
            b'E' => return Err(server_error(&buf)),
            b'Z' => break,
            _ => {} // ParameterStatus, BackendKeyData, NoticeResponse
        }
    }

    out.clear();
    let feed = format!("{POST_SELECT} ORDER BY p.created_at DESC, p.id DESC LIMIT 20");
    let post = format!("{POST_SELECT} WHERE p.id = $1");
    let statements: [(&[u8], &str, &[u32]); 5] = [
        (b"f", &feed, &[]),
        (b"p", &post, &[OID_INT8]),
        (b"c", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", &[OID_INT8, OID_TEXT]),
        (
            b"l",
            "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
            &[OID_INT8, OID_INT8],
        ),
        (b"h", "SELECT 1", &[]),
    ];
    for (name, sql, types) in statements {
        put_msg(&mut out, b'P', |o| {
            cstr(o, name);
            cstr(o, sql.as_bytes());
            o.extend_from_slice(&(types.len() as u16).to_be_bytes());
            for t in types {
                o.extend_from_slice(&t.to_be_bytes());
            }
        });
    }
    put_msg(&mut out, b'S', |_| {});
    s.write_all(&out)?;
    loop {
        match read_msg(&mut s, &mut buf)? {
            b'E' => return Err(server_error(&buf)),
            b'Z' => break,
            _ => {}
        }
    }

    s.set_read_timeout(None)?;
    s.set_write_timeout(None)?;
    s.set_nonblocking(true)?;
    let fd = s.into_raw_fd();
    sys::set_opt(fd, sys::IPPROTO_TCP, sys::TCP_THIN_LINEAR_TIMEOUTS, 1);
    Ok(fd)
}

// ---- request messages ----
//
// Every Bind uses the unnamed portal and binary int8 parameters. Result columns come in whichever format is
// shorter on the wire: `id` and `like_count` as text (a few digits against 8 and 4 bytes), `created_at` binary
// (8 bytes against 29). No Describe is sent, so a reply carries no RowDescription.

fn execute(o: &mut Vec<u8>) {
    put_msg(o, b'E', |o| {
        o.push(0);
        o.extend_from_slice(&0u32.to_be_bytes());
    });
}

fn bind(o: &mut Vec<u8>, statement: u8, param_formats: &[u16], params: &[&[u8]], result_formats: &[u16]) {
    put_msg(o, b'B', |o| {
        o.push(0);
        o.extend_from_slice(&[statement, 0]);
        o.extend_from_slice(&(param_formats.len() as u16).to_be_bytes());
        for f in param_formats {
            o.extend_from_slice(&f.to_be_bytes());
        }
        o.extend_from_slice(&(params.len() as u16).to_be_bytes());
        for p in params {
            o.extend_from_slice(&(p.len() as u32).to_be_bytes());
            o.extend_from_slice(p);
        }
        o.extend_from_slice(&(result_formats.len() as u16).to_be_bytes());
        for f in result_formats {
            o.extend_from_slice(&f.to_be_bytes());
        }
    });
    execute(o);
}

const POST_COLUMNS: [u16; 5] = [0, 0, 1, 0, 0];

pub fn feed(o: &mut Vec<u8>) {
    bind(o, b'f', &[], &[], &POST_COLUMNS);
}

pub fn post(o: &mut Vec<u8>, id: u64) {
    bind(o, b'p', &[1], &[&id.to_be_bytes()], &POST_COLUMNS);
}

pub fn create(o: &mut Vec<u8>, user: u64, body: &[u8]) {
    bind(o, b'c', &[1, 0], &[&user.to_be_bytes(), body], &[0, 1]);
}

pub fn like(o: &mut Vec<u8>, user: u64, post: u64) {
    bind(o, b'l', &[1], &[&user.to_be_bytes(), &post.to_be_bytes()], &[]);
}

pub fn health(o: &mut Vec<u8>) {
    bind(o, b'h', &[], &[], &[]);
}

pub fn sync(o: &mut Vec<u8>) {
    put_msg(o, b'S', |_| {});
}

/// The columns of a DataRow. `None` if the row is malformed, has another column count, or holds a NULL.
pub fn columns<const N: usize>(row: &[u8]) -> Option<[&[u8]; N]> {
    if row.len() < 2 || u16::from_be_bytes([row[0], row[1]]) as usize != N {
        return None;
    }
    let mut at = 2;
    let mut out = [&row[..0]; N];
    for col in out.iter_mut() {
        let len = i32::from_be_bytes(row.get(at..at + 4)?.try_into().ok()?);
        at += 4;
        *col = row.get(at..at + usize::try_from(len).ok()?)?;
        at += len as usize;
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn url_forms() {
        let c = PgConfig::from_url("postgres://feed:s%40crt@127.0.0.1:5433/feed_test?sslmode=disable").unwrap();
        assert_eq!((c.user.as_slice(), c.password.as_slice(), c.database.as_slice()), (&b"feed"[..], &b"s@crt"[..], &b"feed_test"[..]));
        assert_eq!(c.addrs[0].port(), 5433);
        let c = PgConfig::from_url("postgresql://postgres@127.0.0.1/db").unwrap();
        assert_eq!((c.addrs[0].port(), c.password.len()), (5432, 0));
        let c = PgConfig::from_url("postgres://u:p@[::1]:6000/d").unwrap();
        assert!(c.addrs[0].is_ipv6() && c.addrs[0].port() == 6000);
        assert!(PgConfig::from_url("mysql://x").is_err());
    }

    #[test]
    fn message_layout() {
        let mut o = Vec::new();
        post(&mut o, 500_000);
        // Bind: portal "", statement "p", one binary parameter of 8 bytes, five result formats; then Execute
        let want_bind: &[u8] = &[
            b'B', 0, 0, 0, 37, 0, b'p', 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 8, 0, 0, 0, 0, 0, 7, 0xa1, 0x20, 0, 5, 0, 0, 0, 0, 0,
            1, 0, 0, 0, 0,
        ];
        assert_eq!(&o[..want_bind.len()], want_bind);
        assert_eq!(&o[want_bind.len()..], &[b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0]);
        o.clear();
        sync(&mut o);
        assert_eq!(o, [b'S', 0, 0, 0, 4]);
    }

    #[test]
    fn data_rows() {
        let row = [0, 2, 0, 0, 0, 3, b'a', b'b', b'c', 0, 0, 0, 0];
        let [a, b] = columns::<2>(&row).unwrap();
        assert_eq!((a, b), (&b"abc"[..], &b""[..]));
        assert!(columns::<3>(&row).is_none(), "wrong column count");
        assert!(columns::<2>(&row[..8]).is_none(), "truncated");
        assert!(columns::<1>(&[0, 1, 0xff, 0xff, 0xff, 0xff]).is_none(), "NULL");
    }

    #[test]
    fn error_response_fields() {
        let body = b"SERROR\0VERROR\0C23503\0Minsert or update violates foreign key\0\0";
        let (sev, code, msg) = error_fields(body);
        assert_eq!((sev, code), (&b"ERROR"[..], &b"23503"[..]));
        assert!(msg.starts_with(b"insert or update"));
    }
}
