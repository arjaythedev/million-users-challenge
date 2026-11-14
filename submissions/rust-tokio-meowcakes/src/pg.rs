use std::net::{SocketAddr, ToSocketAddrs};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpSocket, TcpStream};

const RCVBUF: u32 = 4 << 20;

pub const FEED: &str = "f";
pub const POST: &str = "p";
pub const CREATE: &str = "c";
pub const LIKE: &str = "l";
pub const HEALTH: &str = "h";

const INT8: u32 = 20;
const TEXT: u32 = 25;

const STATEMENTS: [(&str, &str, &[u32]); 5] = [
    (
        FEED,
        "SELECT p.id, p.body, p.created_at, u.username, \
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
         FROM posts p JOIN users u ON u.id = p.user_id \
         ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
        &[],
    ),
    (
        POST,
        "SELECT p.id, p.body, p.created_at, u.username, \
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
         FROM posts p JOIN users u ON u.id = p.user_id \
         WHERE p.id = $1",
        &[INT8],
    ),
    (
        CREATE,
        "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
        &[INT8, TEXT],
    ),
    (
        LIKE,
        "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
        &[INT8, INT8],
    ),
    (HEALTH, "SELECT 1", &[]),
];

pub struct ConnectParams {
    pub addr: SocketAddr,
    pub user: String,
    pub password: String,
    pub database: String,
}

impl ConnectParams {
    pub fn from_url(url: &str) -> Result<Self, String> {
        let rest = url
            .strip_prefix("postgres://")
            .or_else(|| url.strip_prefix("postgresql://"))
            .ok_or("expected postgres://")?;
        let rest = rest.split('?').next().unwrap();
        let (authority, database) = rest.split_once('/').unwrap_or((rest, ""));
        let (userinfo, hostport) = authority.rsplit_once('@').unwrap_or(("", authority));
        let (user, password) = userinfo.split_once(':').unwrap_or((userinfo, ""));
        let (host, port) = if let Some(h) = hostport.strip_prefix('[') {
            let (h, p) = h.split_once(']').ok_or("bad IPv6 host")?;
            (h, p.strip_prefix(':').unwrap_or(""))
        } else {
            hostport.rsplit_once(':').unwrap_or((hostport, ""))
        };
        let port: u16 = if port.is_empty() {
            5432
        } else {
            port.parse().map_err(|_| "bad port")?
        };
        let host = if host.is_empty() { "localhost" } else { host };
        let user = percent_decode(user);
        let addr = (host, port)
            .to_socket_addrs()
            .map_err(|e| format!("{host}: {e}"))?
            .next()
            .ok_or("no address")?;
        let database = if database.is_empty() {
            user.clone()
        } else {
            percent_decode(database)
        };
        Ok(Self {
            addr,
            user,
            password: percent_decode(password),
            database,
        })
    }
}

fn percent_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    let hex = |c: u8| (c as char).to_digit(16).map(|d| d as u8);
    while i < b.len() {
        if b[i] == b'%'
            && i + 2 < b.len()
            && let (Some(h), Some(l)) = (hex(b[i + 1]), hex(b[i + 2]))
        {
            out.push(h << 4 | l);
            i += 3;
            continue;
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

pub async fn connect(p: &ConnectParams) -> Result<TcpStream, String> {
    let sock = if p.addr.is_ipv4() {
        TcpSocket::new_v4()
    } else {
        TcpSocket::new_v6()
    }
    .map_err(|e| e.to_string())?;
    let _ = sock.set_recv_buffer_size(RCVBUF);
    sock.set_nodelay(true).map_err(|e| e.to_string())?;
    let mut s = sock
        .connect(p.addr)
        .await
        .map_err(|e| format!("connect {}: {e}", p.addr))?;
    let mut rbuf = Vec::with_capacity(4096);

    let mut out = Vec::with_capacity(256);
    message(&mut out, None, |o| {
        o.extend_from_slice(&196608u32.to_be_bytes());
        for (k, v) in [
            ("user", &p.user),
            ("database", &p.database),
            ("client_encoding", &"UTF8".into()),
        ] {
            cstr(o, k);
            cstr(o, v);
        }
        o.push(0);
    });
    send(&mut s, &mut out).await?;

    let mut scram: Option<postgres_protocol::authentication::sasl::ScramSha256> = None;
    loop {
        let (ty, body) = recv(&mut s, &mut rbuf).await?;
        match ty {
            b'R' => {
                let code = u32::from_be_bytes(
                    body.get(..4)
                        .ok_or("short auth message")?
                        .try_into()
                        .unwrap(),
                );
                let data = &body[4..];
                match code {
                    0 => {}
                    3 => message(&mut out, Some(b'p'), |o| cstr(o, &p.password)),
                    5 => {
                        let salt: [u8; 4] =
                            data.get(..4).ok_or("short md5 salt")?.try_into().unwrap();
                        let hash = postgres_protocol::authentication::md5_hash(
                            p.user.as_bytes(),
                            p.password.as_bytes(),
                            salt,
                        );
                        message(&mut out, Some(b'p'), |o| cstr(o, &hash));
                    }
                    10 => {
                        if !data.split(|&b| b == 0).any(|m| m == b"SCRAM-SHA-256") {
                            return Err("server offers no SCRAM-SHA-256".into());
                        }
                        let sc = postgres_protocol::authentication::sasl::ScramSha256::new(
                            p.password.as_bytes(),
                            postgres_protocol::authentication::sasl::ChannelBinding::unsupported(),
                        );
                        message(&mut out, Some(b'p'), |o| {
                            cstr(o, "SCRAM-SHA-256");
                            o.extend_from_slice(&(sc.message().len() as i32).to_be_bytes());
                            o.extend_from_slice(sc.message());
                        });
                        scram = Some(sc);
                    }
                    11 => {
                        let sc = scram.as_mut().ok_or("unexpected SASL continue")?;
                        sc.update(data).map_err(|e| format!("SCRAM: {e}"))?;
                        message(&mut out, Some(b'p'), |o| o.extend_from_slice(sc.message()));
                    }
                    12 => {
                        let sc = scram.as_mut().ok_or("unexpected SASL final")?;
                        sc.finish(data).map_err(|e| format!("SCRAM: {e}"))?;
                    }
                    c => return Err(format!("unsupported authentication method {c}")),
                }
                if !out.is_empty() {
                    send(&mut s, &mut out).await?;
                }
            }
            b'E' => return Err(Error::parse(&body).message),
            b'Z' => break,
            _ => {}
        }
    }

    for (name, sql, types) in STATEMENTS {
        message(&mut out, Some(b'P'), |o| {
            cstr(o, name);
            cstr(o, sql);
            o.extend_from_slice(&(types.len() as u16).to_be_bytes());
            for t in types {
                o.extend_from_slice(&t.to_be_bytes());
            }
        });
    }
    sync(&mut out);
    send(&mut s, &mut out).await?;
    let mut err = None;
    loop {
        let (ty, body) = recv(&mut s, &mut rbuf).await?;
        match ty {
            b'E' => err = Some(Error::parse(&body).message),
            b'Z' => break,
            _ => {}
        }
    }
    match err {
        Some(e) => Err(format!("prepare: {e}")),
        None => Ok(s),
    }
}

async fn send(s: &mut TcpStream, out: &mut Vec<u8>) -> Result<(), String> {
    s.write_all(out).await.map_err(|e| e.to_string())?;
    out.clear();
    Ok(())
}

async fn recv(s: &mut TcpStream, rbuf: &mut Vec<u8>) -> Result<(u8, Vec<u8>), String> {
    loop {
        if rbuf.len() >= 5 {
            let len = u32::from_be_bytes(rbuf[1..5].try_into().unwrap()) as usize;
            if rbuf.len() > len {
                let ty = rbuf[0];
                let body = rbuf[5..1 + len].to_vec();
                rbuf.drain(..1 + len);
                return Ok((ty, body));
            }
        }
        if s.read_buf(rbuf).await.map_err(|e| e.to_string())? == 0 {
            return Err("server closed the connection".into());
        }
    }
}

fn message(out: &mut Vec<u8>, ty: Option<u8>, body: impl FnOnce(&mut Vec<u8>)) {
    out.extend(ty);
    let at = out.len();
    out.extend_from_slice(&[0; 4]);
    body(out);
    let len = (out.len() - at) as u32;
    out[at..at + 4].copy_from_slice(&len.to_be_bytes());
}

fn cstr(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(s.as_bytes());
    out.push(0);
}

pub fn bind(out: &mut Vec<u8>, stmt: &str, params: &[&[u8]]) {
    message(out, Some(b'B'), |o| {
        o.push(0);
        cstr(o, stmt);
        o.extend_from_slice(if params.is_empty() {
            &[0, 0]
        } else {
            &[0, 1, 0, 1]
        });
        o.extend_from_slice(&(params.len() as u16).to_be_bytes());
        for p in params {
            o.extend_from_slice(&(p.len() as i32).to_be_bytes());
            o.extend_from_slice(p);
        }
        o.extend_from_slice(&[0, 1, 0, 1]);
    });
}

pub fn execute(out: &mut Vec<u8>) {
    out.extend_from_slice(&[b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0]);
}

pub fn sync(out: &mut Vec<u8>) {
    out.extend_from_slice(&[b'S', 0, 0, 0, 4]);
}

pub fn columns<const N: usize>(body: &[u8]) -> Option<[&[u8]; N]> {
    if body.len() < 2 || u16::from_be_bytes([body[0], body[1]]) as usize != N {
        return None;
    }
    let mut cols = [&body[..0]; N];
    let mut at = 2;
    for c in cols.iter_mut() {
        let len = i32::from_be_bytes(body.get(at..at + 4)?.try_into().unwrap());
        at += 4;
        if len > 0 {
            *c = body.get(at..at + len as usize)?;
            at += len as usize;
        }
    }
    Some(cols)
}

pub fn int8(b: &[u8]) -> Option<i64> {
    Some(i64::from_be_bytes(b.try_into().ok()?))
}

pub fn int4(b: &[u8]) -> Option<i32> {
    Some(i32::from_be_bytes(b.try_into().ok()?))
}

pub struct Error {
    pub code: [u8; 5],
    pub message: String,
}

impl Error {
    pub fn parse(body: &[u8]) -> Self {
        let mut e = Error {
            code: *b"XX000",
            message: String::new(),
        };
        for field in body.split(|&b| b == 0) {
            match field.first() {
                Some(b'C') if field.len() == 6 => e.code.copy_from_slice(&field[1..]),
                Some(b'M') => e.message = String::from_utf8_lossy(&field[1..]).into_owned(),
                _ => {}
            }
        }
        e
    }
}
