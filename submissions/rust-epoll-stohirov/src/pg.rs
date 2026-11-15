use crate::jwt::{b64_std_decode, b64_std_encode};
use crate::sha256::{Hmac, Sha256, pbkdf2};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::os::fd::IntoRawFd;
use std::time::Duration;

pub const FEED: u8 = 0;
pub const POST: u8 = 1;
pub const CREATE: u8 = 2;
pub const LIKE: u8 = 3;
pub const PING: u8 = 4;

const POST_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, \
     (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
     FROM posts p JOIN users u ON u.id = p.user_id";

fn statements() -> [(String, &'static [u32]); 5] {
    [
        (format!("{POST_SQL} ORDER BY p.created_at DESC, p.id DESC LIMIT 20"), &[]),
        (format!("{POST_SQL} WHERE p.id = $1"), &[20]),
        ("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at".into(), &[20, 25]),
        ("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING".into(), &[20, 20]),
        ("SELECT 1".into(), &[]),
    ]
}

#[derive(Clone, Debug)]
pub struct DbConfig {
    pub host: String,
    pub port: u16,
    pub user: String,
    pub password: String,
    pub database: String,
}

fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

impl DbConfig {
    pub fn parse(url: &str) -> Result<DbConfig, String> {
        let rest = url
            .strip_prefix("postgres://")
            .or_else(|| url.strip_prefix("postgresql://"))
            .ok_or("DATABASE_URL must be postgres://user:password@host:port/database")?;
        let rest = rest.split('?').next().unwrap_or("");
        let (auth_host, path) = match rest.find('/') {
            Some(i) => (&rest[..i], &rest[i + 1..]),
            None => (rest, ""),
        };
        let (userinfo, hostport) = match auth_host.rfind('@') {
            Some(i) => (Some(&auth_host[..i]), &auth_host[i + 1..]),
            None => (None, auth_host),
        };
        let mut user = std::env::var("USER").unwrap_or_else(|_| "postgres".into());
        let mut password = String::new();
        if let Some(ui) = userinfo {
            match ui.find(':') {
                Some(i) => {
                    user = pct_decode(&ui[..i]);
                    password = pct_decode(&ui[i + 1..]);
                }
                None => user = pct_decode(ui),
            }
        }
        let (host, port) = if let Some(h) = hostport.strip_prefix('[') {
            let end = h.find(']').ok_or("bad IPv6 host in DATABASE_URL")?;
            let port = h[end + 1..].strip_prefix(':').map(|p| p.parse::<u16>()).transpose().map_err(|e| e.to_string())?;
            (h[..end].to_string(), port.unwrap_or(5432))
        } else {
            match hostport.rfind(':') {
                Some(i) => (hostport[..i].to_string(), hostport[i + 1..].parse::<u16>().map_err(|e| e.to_string())?),
                None => (hostport.to_string(), 5432),
            }
        };
        let host = if host.is_empty() { "localhost".to_string() } else { pct_decode(&host) };
        let database = if path.is_empty() { user.clone() } else { pct_decode(path) };
        Ok(DbConfig { host, port, user, password, database })
    }
}

#[inline]
pub fn put_i16(o: &mut Vec<u8>, v: i16) {
    o.extend_from_slice(&v.to_be_bytes());
}

#[inline]
pub fn put_i32(o: &mut Vec<u8>, v: i32) {
    o.extend_from_slice(&v.to_be_bytes());
}

#[inline]
pub fn put_i64(o: &mut Vec<u8>, v: i64) {
    o.extend_from_slice(&v.to_be_bytes());
}

#[inline]
pub fn set_i32(o: &mut [u8], at: usize, v: i32) {
    o[at..at + 4].copy_from_slice(&v.to_be_bytes());
}

#[inline]
pub fn be32(b: &[u8], at: usize) -> i32 {
    i32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

#[inline]
pub fn be64(b: &[u8], at: usize) -> i64 {
    i64::from_be_bytes(b[at..at + 8].try_into().unwrap())
}

pub const EXECUTE: [u8; 10] = [b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0];
const EXECUTE_FEED: [u8; 10] = [b'E', 0, 0, 0, 9, 0, 0, 0, 0, 20];
const EXECUTE_ONE: [u8; 10] = [b'E', 0, 0, 0, 9, 0, 0, 0, 0, 1];
pub const SYNC: [u8; 5] = [b'S', 0, 0, 0, 4];

pub fn bind_no_params(op: u8) -> Vec<u8> {
    let mut o = Vec::new();
    o.push(b'B');
    put_i32(&mut o, 0);
    o.push(0);
    o.extend_from_slice(&[b'0' + op, 0]);
    put_i16(&mut o, 0);
    put_i16(&mut o, 0);
    if op == FEED {
        o.extend_from_slice(&READ_RESULT_FORMATS);
    } else {
        put_i16(&mut o, 0);
    }
    let len = o.len() as i32 - 1;
    set_i32(&mut o, 1, len);
    o.extend_from_slice(if op == FEED { &EXECUTE_FEED } else { &EXECUTE });
    o
}

pub const READ_RESULT_FORMATS: [u8; 12] = [0, 5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0];

#[inline]
pub fn bind_post(o: &mut Vec<u8>, id: i64) {
    let len = 4 + 1 + 2 + 2 + 2 + 2 + 4 + 8 + 12;
    o.push(b'B');
    put_i32(o, len);
    o.extend_from_slice(&[0, b'1', 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 8]);
    put_i64(o, id);
    o.extend_from_slice(&READ_RESULT_FORMATS);
    o.extend_from_slice(&EXECUTE_ONE);
}

#[inline]
pub fn bind_create(o: &mut Vec<u8>, user_id: i64, body: &[u8]) {
    let len = 4 + 1 + 2 + 2 + 2 + 2 + 4 + 8 + 4 + body.len() + 2 + 2;
    o.push(b'B');
    put_i32(o, len as i32);
    o.extend_from_slice(&[0, b'2', 0, 0, 1, 0, 1, 0, 2, 0, 0, 0, 8]);
    put_i64(o, user_id);
    put_i32(o, body.len() as i32);
    o.extend_from_slice(body);
    o.extend_from_slice(&[0, 1, 0, 1]);
    o.extend_from_slice(&EXECUTE);
}

#[inline]
pub fn bind_like(o: &mut Vec<u8>, user_id: i64, post_id: i64) {
    let len = 4 + 1 + 2 + 2 + 2 + 2 + 4 + 8 + 4 + 8 + 2;
    o.push(b'B');
    put_i32(o, len);
    o.extend_from_slice(&[0, b'3', 0, 0, 1, 0, 1, 0, 2, 0, 0, 0, 8]);
    put_i64(o, user_id);
    put_i32(o, 8);
    put_i64(o, post_id);
    o.extend_from_slice(&[0, 0]);
    o.extend_from_slice(&EXECUTE);
}

pub fn error_fields(b: &[u8]) -> ([u8; 5], String) {
    let mut code = [b'?'; 5];
    let mut msg = String::new();
    let mut k = 0;
    while k < b.len() && b[k] != 0 {
        let field = b[k];
        k += 1;
        let z = b[k..].iter().position(|&c| c == 0).map_or(b.len(), |p| k + p);
        if field == b'C' && z - k == 5 {
            code.copy_from_slice(&b[k..z]);
        } else if field == b'M' {
            msg = String::from_utf8_lossy(&b[k..z]).into_owned();
        }
        k = z + 1;
    }
    (code, msg)
}

struct Blocking {
    s: TcpStream,
    buf: Vec<u8>,
    at: usize,
}

impl Blocking {
    fn send(&mut self, m: &[u8]) -> Result<(), String> {
        self.s.write_all(m).map_err(|e| format!("write: {e}"))
    }

    fn next(&mut self) -> Result<(u8, Vec<u8>), String> {
        loop {
            let have = self.buf.len() - self.at;
            if have >= 5 {
                let n = be32(&self.buf, self.at + 1) as usize;
                if have >= 1 + n {
                    let t = self.buf[self.at];
                    let body = self.buf[self.at + 5..self.at + 1 + n].to_vec();
                    self.at += 1 + n;
                    return Ok((t, body));
                }
            }
            let mut tmp = [0u8; 8192];
            let r = self.s.read(&mut tmp).map_err(|e| format!("read: {e}"))?;
            if r == 0 {
                return Err("connection closed by the database".into());
            }
            self.buf.drain(..self.at);
            self.at = 0;
            self.buf.extend_from_slice(&tmp[..r]);
        }
    }
}

fn message(t: u8, body: &[u8]) -> Vec<u8> {
    let mut o = vec![t];
    put_i32(&mut o, body.len() as i32 + 4);
    o.extend_from_slice(body);
    o
}

fn cstr(o: &mut Vec<u8>, s: &str) {
    o.extend_from_slice(s.as_bytes());
    o.push(0);
}

pub fn connect(cfg: &DbConfig) -> Result<i32, String> {
    let addrs: Vec<_> = (cfg.host.as_str(), cfg.port)
        .to_socket_addrs()
        .map_err(|e| format!("resolve {}: {e}", cfg.host))?
        .collect();
    let mut last = format!("no address for {}", cfg.host);
    let mut stream = None;
    for a in addrs {
        match TcpStream::connect_timeout(&a, Duration::from_millis(2000)) {
            Ok(s) => {
                stream = Some(s);
                break;
            }
            Err(e) => last = format!("connect {a}: {e}"),
        }
    }
    let s = stream.ok_or(last)?;
    s.set_nodelay(true).ok();
    s.set_read_timeout(Some(Duration::from_millis(3000))).ok();
    s.set_write_timeout(Some(Duration::from_millis(3000))).ok();
    let mut c = Blocking { s, buf: Vec::new(), at: 0 };

    let mut m = Vec::new();
    put_i32(&mut m, 0);
    put_i32(&mut m, 196608);
    cstr(&mut m, "user");
    cstr(&mut m, &cfg.user);
    cstr(&mut m, "database");
    cstr(&mut m, &cfg.database);
    cstr(&mut m, "client_encoding");
    cstr(&mut m, "UTF8");
    cstr(&mut m, "application_name");
    cstr(&mut m, "feed");
    m.push(0);
    let n = m.len() as i32;
    set_i32(&mut m, 0, n);
    c.send(&m)?;

    let mut scram: Option<Scram> = None;
    loop {
        let (t, b) = c.next()?;
        match t {
            b'E' => {
                let (code, msg) = error_fields(&b);
                return Err(format!("{msg} ({})", String::from_utf8_lossy(&code)));
            }
            b'Z' => break,
            b'R' => {
                let auth = be32(&b, 0);
                match auth {
                    0 => {}
                    3 => {
                        let mut p = Vec::new();
                        cstr(&mut p, &cfg.password);
                        c.send(&message(b'p', &p))?;
                    }
                    5 => {
                        let inner = hex(&md5(format!("{}{}", cfg.password, cfg.user).as_bytes()));
                        let mut x = inner.into_bytes();
                        x.extend_from_slice(&b[4..8]);
                        let mut p = Vec::new();
                        cstr(&mut p, &format!("md5{}", hex(&md5(&x))));
                        c.send(&message(b'p', &p))?;
                    }
                    10 => {
                        let mechs = String::from_utf8_lossy(&b[4..]);
                        if !mechs.split('\0').any(|m| m == "SCRAM-SHA-256") {
                            return Err(format!("the database offers no supported SASL mechanism: {mechs}"));
                        }
                        let sc = Scram::new();
                        let first = sc.first();
                        let mut p = Vec::new();
                        cstr(&mut p, "SCRAM-SHA-256");
                        put_i32(&mut p, first.len() as i32);
                        p.extend_from_slice(first.as_bytes());
                        c.send(&message(b'p', &p))?;
                        scram = Some(sc);
                    }
                    11 => {
                        let sc = scram.as_mut().ok_or("SASL continue without SASL")?;
                        let last = sc.next(&String::from_utf8_lossy(&b[4..]), &cfg.password)?;
                        c.send(&message(b'p', last.as_bytes()))?;
                    }
                    12 => {
                        let sc = scram.as_ref().ok_or("SASL final without SASL")?;
                        if !sc.verify(&String::from_utf8_lossy(&b[4..])) {
                            return Err("bad SCRAM server signature".into());
                        }
                    }
                    a => return Err(format!("unsupported authentication method {a}")),
                }
            }
            _ => {}
        }
    }

    let mut m = Vec::new();
    for (i, (sql, types)) in statements().iter().enumerate() {
        let mut p = Vec::new();
        cstr(&mut p, &i.to_string());
        cstr(&mut p, sql);
        put_i16(&mut p, types.len() as i16);
        for &t in types.iter() {
            put_i32(&mut p, t as i32);
        }
        m.extend_from_slice(&message(b'P', &p));
    }
    m.extend_from_slice(&SYNC);
    c.send(&m)?;
    let mut err = None;
    loop {
        let (t, b) = c.next()?;
        if t == b'E' && err.is_none() {
            let (code, msg) = error_fields(&b);
            err = Some(format!("prepare: {msg} ({})", String::from_utf8_lossy(&code)));
        }
        if t == b'Z' {
            break;
        }
    }
    if let Some(e) = err {
        return Err(e);
    }
    if c.at != c.buf.len() {
        return Err("unexpected data after login".into());
    }
    c.s.set_read_timeout(None).ok();
    c.s.set_write_timeout(None).ok();
    c.s.set_nonblocking(true).map_err(|e| format!("nonblocking: {e}"))?;
    Ok(c.s.into_raw_fd())
}

struct Scram {
    nonce: String,
    client_first_bare: String,
    server_signature: [u8; 32],
}

impl Scram {
    fn new() -> Scram {
        let mut r = [0u8; 18];
        crate::sys::random_bytes(&mut r);
        let mut nonce = String::new();
        b64_std_encode(&r, &mut nonce);
        Scram { client_first_bare: format!("n=,r={nonce}"), nonce, server_signature: [0; 32] }
    }

    fn first(&self) -> String {
        format!("n,,{}", self.client_first_bare)
    }

    fn next(&mut self, server_first: &str, password: &str) -> Result<String, String> {
        let (mut r, mut s, mut i) = (None, None, 0u32);
        for attr in server_first.split(',') {
            if let Some(v) = attr.strip_prefix("r=") {
                r = Some(v);
            } else if let Some(v) = attr.strip_prefix("s=") {
                s = Some(v);
            } else if let Some(v) = attr.strip_prefix("i=") {
                i = v.parse().unwrap_or(0);
            }
        }
        let (r, s) = match (r, s) {
            (Some(r), Some(s)) if i > 0 && r.starts_with(&self.nonce) => (r, s),
            _ => return Err("bad SCRAM server-first-message".into()),
        };
        let salt = b64_std_decode(s.as_bytes()).ok_or("bad SCRAM salt")?;
        let salted = pbkdf2(password.as_bytes(), &salt, i);
        let client_key = Hmac::new(&salted).mac(b"Client Key");
        let stored_key = Sha256::digest(&client_key);
        let without_proof = format!("c=biws,r={r}");
        let auth = format!("{},{},{}", self.client_first_bare, server_first, without_proof);
        let mut proof = Hmac::new(&stored_key).mac(auth.as_bytes());
        for k in 0..32 {
            proof[k] ^= client_key[k];
        }
        let server_key = Hmac::new(&salted).mac(b"Server Key");
        self.server_signature = Hmac::new(&server_key).mac(auth.as_bytes());
        let mut p = String::new();
        b64_std_encode(&proof, &mut p);
        Ok(format!("{without_proof},p={p}"))
    }

    fn verify(&self, server_final: &str) -> bool {
        match server_final.trim().strip_prefix("v=") {
            Some(v) => b64_std_decode(v.as_bytes()).is_some_and(|d| d == self.server_signature),
            None => false,
        }
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn md5(data: &[u8]) -> [u8; 16] {
    const S: [u32; 64] = [
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14,
        20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6,
        10, 15, 21,
    ];
    let k: Vec<u32> = (0..64).map(|i| ((i as f64 + 1.0).sin().abs() * 4294967296.0) as u32).collect();
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&((data.len() as u64).wrapping_mul(8)).to_le_bytes());
    let (mut a0, mut b0, mut c0, mut d0) = (0x67452301u32, 0xefcdab89u32, 0x98badcfeu32, 0x10325476u32);
    for chunk in msg.chunks(64) {
        let m: Vec<u32> = (0..16).map(|i| u32::from_le_bytes(chunk[4 * i..4 * i + 4].try_into().unwrap())).collect();
        let (mut a, mut b, mut c, mut d) = (a0, b0, c0, d0);
        for i in 0..64 {
            let (f, g) = match i / 16 {
                0 => ((b & c) | (!b & d), i),
                1 => ((d & b) | (!d & c), (5 * i + 1) % 16),
                2 => (b ^ c ^ d, (3 * i + 5) % 16),
                _ => (c ^ (b | !d), (7 * i) % 16),
            };
            let f = f.wrapping_add(a).wrapping_add(k[i]).wrapping_add(m[g]);
            a = d;
            d = c;
            c = b;
            b = b.wrapping_add(f.rotate_left(S[i]));
        }
        a0 = a0.wrapping_add(a);
        b0 = b0.wrapping_add(b);
        c0 = c0.wrapping_add(c);
        d0 = d0.wrapping_add(d);
    }
    let mut out = [0u8; 16];
    out[..4].copy_from_slice(&a0.to_le_bytes());
    out[4..8].copy_from_slice(&b0.to_le_bytes());
    out[8..12].copy_from_slice(&c0.to_le_bytes());
    out[12..].copy_from_slice(&d0.to_le_bytes());
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn url() {
        let c = DbConfig::parse("postgres://feed:p%40ss@db.local:6543/feed").unwrap();
        assert_eq!((c.host.as_str(), c.port, c.user.as_str(), c.password.as_str(), c.database.as_str()),
            ("db.local", 6543, "feed", "p@ss", "feed"));
        let c = DbConfig::parse("postgresql://u@[::1]/d?sslmode=disable").unwrap();
        assert_eq!((c.host.as_str(), c.port, c.database.as_str()), ("::1", 5432, "d"));
    }

    #[test]
    fn md5_vector() {
        assert_eq!(hex(&md5(b"")), "d41d8cd98f00b204e9800998ecf8427e");
        assert_eq!(hex(&md5(b"The quick brown fox jumps over the lazy dog")), "9e107d9d372bb6826bd81d3542a419d6");
    }

    #[test]
    fn binds() {
        let mut o = Vec::new();
        bind_post(&mut o, 42);
        assert_eq!(be32(&o, 1) as usize, o.len() - 1 - EXECUTE.len());
        o.clear();
        bind_create(&mut o, 1, b"hello");
        assert_eq!(be32(&o, 1) as usize, o.len() - 1 - EXECUTE.len());
        o.clear();
        bind_like(&mut o, 1, 2);
        assert_eq!(be32(&o, 1) as usize, o.len() - 1 - EXECUTE.len());
        let f = bind_no_params(FEED);
        assert_eq!(be32(&f, 1) as usize, f.len() - 1 - EXECUTE.len());
    }
}
