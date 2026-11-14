use crate::jwt::{b64_decode, b64_encode};
use crate::sha256::{digest, HmacKey};
use std::ffi::CString;

pub struct PgCfg {
    pub addr: Vec<u8>,
    pub user: String,
    pub pass: String,
    pub db: String,
}

fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::new();
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

impl PgCfg {
    pub fn from_url(url: &str) -> Result<PgCfg, String> {
        let rest = url
            .strip_prefix("postgres://")
            .or_else(|| url.strip_prefix("postgresql://"))
            .ok_or("DATABASE_URL must start with postgres://")?;
        let rest = rest.split('?').next().unwrap_or("");
        let (auth, hostpart) = match rest.rfind('@') {
            Some(i) => (&rest[..i], &rest[i + 1..]),
            None => ("", rest),
        };
        let (user, pass) = match auth.find(':') {
            Some(i) => (pct_decode(&auth[..i]), pct_decode(&auth[i + 1..])),
            None => (pct_decode(auth), String::new()),
        };
        let (hp, db) = match hostpart.find('/') {
            Some(i) => (&hostpart[..i], pct_decode(&hostpart[i + 1..])),
            None => (hostpart, String::new()),
        };
        let (host, port) = if let Some(h) = hp.strip_prefix('[') {
            let e = h.find(']').ok_or("bad host")?;
            let port = h[e + 1..].strip_prefix(':').unwrap_or("5432");
            (h[..e].to_string(), port.to_string())
        } else {
            match hp.rfind(':') {
                Some(i) => (hp[..i].to_string(), hp[i + 1..].to_string()),
                None => (hp.to_string(), "5432".to_string()),
            }
        };
        let user = if user.is_empty() { "postgres".to_string() } else { user };
        let db = if db.is_empty() { user.clone() } else { db };
        let addr = resolve(if host.is_empty() { "localhost" } else { &host }, &port)?;
        Ok(PgCfg { addr, user, pass, db })
    }
}

fn resolve(host: &str, port: &str) -> Result<Vec<u8>, String> {
    let h = CString::new(host).map_err(|e| e.to_string())?;
    let p = CString::new(port).map_err(|e| e.to_string())?;
    unsafe {
        let mut hints: libc::addrinfo = std::mem::zeroed();
        hints.ai_family = libc::AF_UNSPEC;
        hints.ai_socktype = libc::SOCK_STREAM;
        let mut res: *mut libc::addrinfo = std::ptr::null_mut();
        let rc = libc::getaddrinfo(h.as_ptr(), p.as_ptr(), &hints, &mut res);
        if rc != 0 || res.is_null() {
            return Err(format!("cannot resolve {}", host));
        }
        let mut ai = res;
        let mut chosen = res;
        while !ai.is_null() {
            if (*ai).ai_family == libc::AF_INET {
                chosen = ai;
                break;
            }
            ai = (*ai).ai_next;
        }
        let a = std::slice::from_raw_parts((*chosen).ai_addr as *const u8, (*chosen).ai_addrlen as usize).to_vec();
        libc::freeaddrinfo(res);
        Ok(a)
    }
}

const POST_SELECT: &str = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id";

pub const SET_SQL: &str = "SET plan_cache_mode = 'force_generic_plan'; SET max_parallel_workers_per_gather = 0; SET client_min_messages = 'error'";

fn statements() -> [(&'static [u8], String, &'static [u32]); 5] {
    [
        (b"f\0", format!("{} ORDER BY p.created_at DESC, p.id DESC LIMIT 20", POST_SELECT), &[]),
        (b"p\0", format!("{} WHERE p.id = $1", POST_SELECT), &[20]),
        (b"c\0", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at".to_string(), &[20, 25]),
        (
            b"l\0",
            "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING".to_string(),
            &[20, 20],
        ),
        (b"h\0", "SELECT 1".to_string(), &[]),
    ]
}

#[inline]
fn begin(tx: &mut Vec<u8>, t: u8) -> usize {
    tx.push(t);
    let p = tx.len();
    tx.extend_from_slice(&[0, 0, 0, 0]);
    p
}

#[inline]
fn end(tx: &mut Vec<u8>, p: usize) {
    let n = (tx.len() - p) as u32;
    tx[p..p + 4].copy_from_slice(&n.to_be_bytes());
}

const EXEC: [u8; 10] = [b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0];
const SYNC: [u8; 5] = [b'S', 0, 0, 0, 4];

const FEED_BIND: [u8; 24] = [
    b'B', 0, 0, 0, 23, 0, b'f', 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
];
const HEALTH_BIND: [u8; 14] = [b'B', 0, 0, 0, 13, 0, b'h', 0, 0, 0, 0, 0, 0, 0];

pub fn put_feed(tx: &mut Vec<u8>) {
    tx.extend_from_slice(&FEED_BIND);
    tx.extend_from_slice(&EXEC);
}

pub fn put_health(tx: &mut Vec<u8>) {
    tx.extend_from_slice(&HEALTH_BIND);
    tx.extend_from_slice(&EXEC);
}

pub fn put_post(tx: &mut Vec<u8>, id: i64) {
    tx.extend_from_slice(&[b'B', 0, 0, 0, 37, 0, b'p', 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 8]);
    tx.extend_from_slice(&id.to_be_bytes());
    tx.extend_from_slice(&[0, 5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0]);
    tx.extend_from_slice(&EXEC);
}

pub fn put_like(tx: &mut Vec<u8>, user: i64, post: i64) {
    let p = begin(tx, b'B');
    tx.extend_from_slice(b"\0l\0");
    tx.extend_from_slice(&[0, 1, 0, 1, 0, 2]);
    tx.extend_from_slice(&[0, 0, 0, 8]);
    tx.extend_from_slice(&user.to_be_bytes());
    tx.extend_from_slice(&[0, 0, 0, 8]);
    tx.extend_from_slice(&post.to_be_bytes());
    tx.extend_from_slice(&[0, 0]);
    end(tx, p);
    tx.extend_from_slice(&EXEC);
}

pub fn put_create(tx: &mut Vec<u8>, user: i64, body: &[u8]) {
    let p = begin(tx, b'B');
    tx.extend_from_slice(b"\0c\0");
    tx.extend_from_slice(&[0, 2, 0, 1, 0, 0, 0, 2]);
    tx.extend_from_slice(&[0, 0, 0, 8]);
    tx.extend_from_slice(&user.to_be_bytes());
    tx.extend_from_slice(&(body.len() as u32).to_be_bytes());
    tx.extend_from_slice(body);
    tx.extend_from_slice(&[0, 2, 0, 1, 0, 1]);
    end(tx, p);
    tx.extend_from_slice(&EXEC);
}

pub fn put_sync(tx: &mut Vec<u8>) {
    tx.extend_from_slice(&SYNC);
}

pub fn sqlstate(body: &[u8]) -> [u8; 5] {
    let mut i = 0;
    while i < body.len() && body[i] != 0 {
        let t = body[i];
        let s = i + 1;
        let e = s + body[s..].iter().position(|&c| c == 0).unwrap_or(body.len() - s);
        if t == b'C' && e - s == 5 {
            let mut c = [0u8; 5];
            c.copy_from_slice(&body[s..e]);
            return c;
        }
        i = e + 1;
    }
    *b"XX000"
}

pub fn error_text(body: &[u8]) -> String {
    let mut i = 0;
    while i < body.len() && body[i] != 0 {
        let t = body[i];
        let s = i + 1;
        let e = s + body[s..].iter().position(|&c| c == 0).unwrap_or(body.len() - s);
        if t == b'M' {
            return String::from_utf8_lossy(&body[s..e]).into_owned();
        }
        i = e + 1;
    }
    "postgres error".to_string()
}

struct Blocking {
    fd: i32,
    buf: Vec<u8>,
    off: usize,
}

impl Blocking {
    fn send(&mut self, data: &[u8]) -> Result<(), String> {
        let mut o = 0;
        while o < data.len() {
            let n = unsafe {
                libc::send(self.fd, data[o..].as_ptr() as *const _, data.len() - o, libc::MSG_NOSIGNAL)
            };
            if n <= 0 {
                return Err(format!("send: {}", std::io::Error::last_os_error()));
            }
            o += n as usize;
        }
        Ok(())
    }

    fn msg(&mut self) -> Result<(u8, Vec<u8>), String> {
        loop {
            let avail = self.buf.len() - self.off;
            if avail >= 5 {
                let len = u32::from_be_bytes(self.buf[self.off + 1..self.off + 5].try_into().unwrap()) as usize;
                if avail >= 1 + len {
                    let t = self.buf[self.off];
                    let body = self.buf[self.off + 5..self.off + 1 + len].to_vec();
                    self.off += 1 + len;
                    return Ok((t, body));
                }
            }
            let mut tmp = [0u8; 8192];
            let n = unsafe { libc::recv(self.fd, tmp.as_mut_ptr() as *mut _, tmp.len(), 0) };
            if n <= 0 {
                return Err(format!("recv: {}", if n == 0 { "eof".to_string() } else { std::io::Error::last_os_error().to_string() }));
            }
            self.buf.drain(..self.off);
            self.off = 0;
            self.buf.extend_from_slice(&tmp[..n as usize]);
        }
    }
}

fn random_bytes(out: &mut [u8]) {
    let n = unsafe { libc::getrandom(out.as_mut_ptr() as *mut _, out.len(), 0) };
    if n != out.len() as isize {
        let t = crate::sys::mono_ns();
        for (i, b) in out.iter_mut().enumerate() {
            *b = (t >> ((i % 8) * 8)) as u8 ^ i as u8;
        }
    }
}

fn scram(c: &mut Blocking, cfg: &PgCfg, mechs: &[u8]) -> Result<(), String> {
    if !mechs.split(|&b| b == 0).any(|m| m == b"SCRAM-SHA-256") {
        return Err("server does not offer SCRAM-SHA-256".into());
    }
    let mut raw = [0u8; 18];
    random_bytes(&mut raw);
    let mut nonce = Vec::new();
    b64_encode(&raw, &mut nonce);
    let client_first_bare = [b"n=,r=".as_slice(), &nonce].concat();
    let client_first = [b"n,,".as_slice(), &client_first_bare].concat();
    let mut m = vec![b'p', 0, 0, 0, 0];
    m.extend_from_slice(b"SCRAM-SHA-256\0");
    m.extend_from_slice(&(client_first.len() as u32).to_be_bytes());
    m.extend_from_slice(&client_first);
    end(&mut m, 1);
    c.send(&m)?;
    let (t, body) = c.msg()?;
    if t == b'E' {
        return Err(error_text(&body));
    }
    if t != b'R' || body.len() < 4 || u32::from_be_bytes(body[..4].try_into().unwrap()) != 11 {
        return Err("unexpected SASL continue".into());
    }
    let server_first = body[4..].to_vec();
    let mut snonce: &[u8] = b"";
    let mut salt: &[u8] = b"";
    let mut iters = 0u32;
    for part in server_first.split(|&b| b == b',') {
        if let Some(v) = part.strip_prefix(b"r=") {
            snonce = v;
        } else if let Some(v) = part.strip_prefix(b"s=") {
            salt = v;
        } else if let Some(v) = part.strip_prefix(b"i=") {
            iters = std::str::from_utf8(v).ok().and_then(|s| s.parse().ok()).unwrap_or(0);
        }
    }
    if !snonce.starts_with(&nonce) || iters == 0 {
        return Err("bad SCRAM server-first".into());
    }
    let mut saltb = vec![0u8; salt.len()];
    let sn = b64_decode(salt, &mut saltb).ok_or("bad salt")?;
    saltb.truncate(sn);
    let pk = HmacKey::new(cfg.pass.as_bytes());
    let mut u = {
        let mut s = pk.start();
        s.update(&saltb);
        s.update(&[0, 0, 0, 1]);
        pk.finish(s)
    };
    let mut salted = u;
    for _ in 1..iters {
        u = pk.mac(&u);
        for k in 0..32 {
            salted[k] ^= u[k];
        }
    }
    let sk = HmacKey::new(&salted);
    let client_key = sk.mac(b"Client Key");
    let stored = digest(&client_key);
    let without_proof = [b"c=biws,r=".as_slice(), snonce].concat();
    let auth_msg = [client_first_bare.as_slice(), b",", &server_first, b",", &without_proof].concat();
    let sig = HmacKey::new(&stored).mac(&auth_msg);
    let mut proof = [0u8; 32];
    for k in 0..32 {
        proof[k] = client_key[k] ^ sig[k];
    }
    let mut fin = without_proof.clone();
    fin.extend_from_slice(b",p=");
    b64_encode(&proof, &mut fin);
    let mut m = vec![b'p', 0, 0, 0, 0];
    m.extend_from_slice(&fin);
    end(&mut m, 1);
    c.send(&m)
}

pub fn connect(cfg: &PgCfg) -> Result<i32, String> {
    unsafe {
        let fam = u16::from_ne_bytes([cfg.addr[0], cfg.addr[1]]) as i32;
        let fd = libc::socket(fam, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0);
        if fd < 0 {
            return Err("socket failed".into());
        }
        let tv = libc::timeval { tv_sec: 5, tv_usec: 0 };
        libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_RCVTIMEO, &tv as *const _ as *const _, std::mem::size_of::<libc::timeval>() as u32);
        libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_SNDTIMEO, &tv as *const _ as *const _, std::mem::size_of::<libc::timeval>() as u32);
        if libc::connect(fd, cfg.addr.as_ptr() as *const libc::sockaddr, cfg.addr.len() as u32) != 0 {
            let e = std::io::Error::last_os_error();
            libc::close(fd);
            return Err(format!("connect: {}", e));
        }
        crate::sys::setopt(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, 1);
        crate::sys::setopt(fd, libc::IPPROTO_TCP, libc::TCP_THIN_LINEAR_TIMEOUTS, 1);
        let mut c = Blocking { fd, buf: Vec::new(), off: 0 };
        match handshake(&mut c, cfg) {
            Ok(()) => {
                let zero = libc::timeval { tv_sec: 0, tv_usec: 0 };
                libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_RCVTIMEO, &zero as *const _ as *const _, std::mem::size_of::<libc::timeval>() as u32);
                libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_SNDTIMEO, &zero as *const _ as *const _, std::mem::size_of::<libc::timeval>() as u32);
                crate::sys::set_nonblock(fd);
                Ok(fd)
            }
            Err(e) => {
                libc::close(fd);
                Err(e)
            }
        }
    }
}

fn handshake(c: &mut Blocking, cfg: &PgCfg) -> Result<(), String> {
    let mut m = vec![0, 0, 0, 0, 0, 3, 0, 0];
    for (k, v) in [("user", cfg.user.as_str()), ("database", cfg.db.as_str()), ("client_encoding", "UTF8"), ("application_name", "pulse")] {
        m.extend_from_slice(k.as_bytes());
        m.push(0);
        m.extend_from_slice(v.as_bytes());
        m.push(0);
    }
    m.push(0);
    let n = m.len() as u32;
    m[..4].copy_from_slice(&n.to_be_bytes());
    c.send(&m)?;
    loop {
        let (t, body) = c.msg()?;
        match t {
            b'R' => {
                let code = u32::from_be_bytes(body[..4].try_into().map_err(|_| "short auth")?);
                match code {
                    0 => {}
                    3 => {
                        let mut m = vec![b'p', 0, 0, 0, 0];
                        m.extend_from_slice(cfg.pass.as_bytes());
                        m.push(0);
                        end(&mut m, 1);
                        c.send(&m)?;
                    }
                    10 => scram(c, cfg, &body[4..])?,
                    12 => {}
                    _ => return Err(format!("unsupported auth method {}", code)),
                }
            }
            b'E' => return Err(error_text(&body)),
            b'Z' => break,
            _ => {}
        }
    }
    let mut m = Vec::new();
    let p = begin(&mut m, b'Q');
    m.extend_from_slice(SET_SQL.as_bytes());
    m.push(0);
    end(&mut m, p);
    for (name, sql, oids) in statements() {
        let p = begin(&mut m, b'P');
        m.extend_from_slice(name);
        m.extend_from_slice(sql.as_bytes());
        m.push(0);
        m.extend_from_slice(&(oids.len() as u16).to_be_bytes());
        for o in oids {
            m.extend_from_slice(&o.to_be_bytes());
        }
        end(&mut m, p);
    }
    put_sync(&mut m);
    c.send(&m)?;
    let mut z = 0;
    let mut err = None;
    while z < 2 {
        let (t, body) = c.msg()?;
        match t {
            b'E' => err = Some(error_text(&body)),
            b'Z' => z += 1,
            _ => {}
        }
    }
    if let Some(e) = err {
        return Err(e);
    }
    if c.off != c.buf.len() {
        return Err("unexpected trailing data after handshake".into());
    }
    Ok(())
}
