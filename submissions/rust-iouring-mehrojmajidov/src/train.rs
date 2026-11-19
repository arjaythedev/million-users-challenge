//! The training run of the profile-guided build (`build.sh`). Compiled only with `--features train`; the binary
//! that serves does not contain it.
//!
//! The real server runs on loopback against a stand-in for Postgres, which answers the five statements with rows
//! shaped like the load test's, and clients send the load test's mix of requests. After a few seconds the process
//! exits, which writes the profile.

use crate::crypto::{b64_encode, Hmac};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::time::{Duration, Instant};

const CLIENT_THREADS: usize = 4;
const CONNECTIONS: usize = 48;

/// Starts the stand-in and returns the `DATABASE_URL` that reaches it.
pub fn postgres() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").expect("training: no loopback port");
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            std::thread::spawn(move || {
                let _ = backend(stream);
            });
        }
    });
    format!("postgres://train:train@127.0.0.1:{port}/train")
}

fn message(out: &mut Vec<u8>, kind: u8, payload: &[u8]) {
    out.push(kind);
    out.extend_from_slice(&(payload.len() as u32 + 4).to_be_bytes());
    out.extend_from_slice(payload);
}

fn row(out: &mut Vec<u8>, columns: &[&[u8]]) {
    let mut payload = (columns.len() as u16).to_be_bytes().to_vec();
    for c in columns {
        payload.extend_from_slice(&(c.len() as u32).to_be_bytes());
        payload.extend_from_slice(c);
    }
    message(out, b'D', &payload);
}

/// A post as the load test leaves them in the table: id and like count as text, the timestamp binary.
fn post(out: &mut Vec<u8>, n: u64) {
    let name = format!("clever_nebula_{}", 1 + n * 7919 % 20_000);
    let body = format!("{name} says hi at 2026-11-14T05:26:{:02}.{:03}Z (VU {}, iter {})", n % 60, n % 1000, 1 + n % 1_480_000, n % 14);
    let created = (9_814i64 * 86_400 + 19_565) * 1_000_000 + (n as i64 % 60) * 1_000_000 + 229_000;
    row(out, &[(500_000 + n).to_string().as_bytes(), body.as_bytes(), &created.to_be_bytes(), name.as_bytes(), (n % 3).to_string().as_bytes()]);
}

fn backend(mut s: TcpStream) -> std::io::Result<()> {
    s.set_nodelay(true)?;
    let mut len = [0u8; 4];
    s.read_exact(&mut len)?;
    let mut startup = vec![0; u32::from_be_bytes(len) as usize - 4];
    s.read_exact(&mut startup)?;
    let mut out = Vec::new();
    message(&mut out, b'R', &0u32.to_be_bytes());
    message(&mut out, b'Z', b"I");
    s.write_all(&out)?;

    let (mut input, mut at, mut statement, mut n) = (Vec::<u8>::new(), 0usize, 0u8, 0u64);
    let mut chunk = vec![0u8; 64 * 1024];
    loop {
        out.clear();
        while input.len() - at >= 5 {
            let size = u32::from_be_bytes(input[at + 1..at + 5].try_into().unwrap()) as usize + 1;
            if input.len() - at < size {
                break;
            }
            let (kind, payload) = (input[at], &input[at + 5..at + size]);
            at += size;
            match kind {
                b'P' => message(&mut out, b'1', b""),
                b'B' => {
                    // unnamed portal, then the statement's one-letter name
                    statement = payload.get(1).copied().unwrap_or(0);
                    message(&mut out, b'2', b"");
                }
                b'E' => {
                    n += 1;
                    // a reply that fills the Execute's row limit ends with PortalSuspended, as from Postgres
                    let limit = payload.get(1..5).map_or(0, |b| u32::from_be_bytes(b.try_into().unwrap()));
                    match statement {
                        b'f' => {
                            for k in 0..20 {
                                post(&mut out, n * 20 + k);
                            }
                            if limit == 20 { message(&mut out, b's', b"") } else { message(&mut out, b'C', b"SELECT 20\0") }
                        }
                        b'p' => {
                            post(&mut out, n);
                            if limit == 1 { message(&mut out, b's', b"") } else { message(&mut out, b'C', b"SELECT 1\0") }
                        }
                        b'c' => {
                            row(&mut out, &[(700_000 + n).to_string().as_bytes(), &(848_000_000_000_000i64 + n as i64).to_be_bytes()]);
                            message(&mut out, b'C', b"INSERT 0 1\0");
                        }
                        b'l' => message(&mut out, b'C', if n % 4 == 0 { b"INSERT 0 0\0" } else { b"INSERT 0 1\0" }),
                        _ => {
                            row(&mut out, &[b"1"]);
                            message(&mut out, b'C', b"SELECT 1\0");
                        }
                    }
                }
                b'S' => message(&mut out, b'Z', b"I"),
                b'X' => return Ok(()),
                _ => {}
            }
        }
        if !out.is_empty() {
            s.write_all(&out)?;
        }
        if at == input.len() {
            input.clear();
            at = 0;
        }
        let got = s.read(&mut chunk)?;
        if got == 0 {
            return Ok(());
        }
        input.extend_from_slice(&chunk[..got]);
    }
}

fn token(secret: &[u8], user: u64) -> String {
    let url = |bytes: &[u8]| -> String {
        let mut v = b64_encode(bytes);
        v.retain(|&b| b != b'=');
        v.iter().map(|&b| match b { b'+' => '-', b'/' => '_', x => x as char }).collect()
    };
    let head = format!(
        "{}.{}",
        url(br#"{"alg":"HS256","typ":"JWT"}"#),
        url(format!(r#"{{"sub":"{user}","username":"clever_nebula_{user}","iat":1794633757,"exp":2109993757}}"#).as_bytes())
    );
    let signature = url(&Hmac::new(secret).mac(&[head.as_bytes()]));
    format!("{head}.{signature}")
}

/// Reads one response and returns its body length. Enough of HTTP for this server's own answers.
fn response(s: &mut TcpStream, buf: &mut Vec<u8>) -> std::io::Result<()> {
    buf.clear();
    let mut chunk = [0u8; 4096];
    loop {
        if let Some(end) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = String::from_utf8_lossy(&buf[..end]).to_ascii_lowercase();
            let length: usize = head.split("content-length:").nth(1).and_then(|v| v.split("\r\n").next()).and_then(|v| v.trim().parse().ok()).unwrap_or(0);
            let mut missing = (end + 4 + length).saturating_sub(buf.len());
            while missing > 0 {
                let got = s.read(&mut chunk[..missing.min(4096)])?;
                if got == 0 {
                    return Err(std::io::ErrorKind::UnexpectedEof.into());
                }
                missing -= got;
            }
            return Ok(());
        }
        let got = s.read(&mut chunk)?;
        if got == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        buf.extend_from_slice(&chunk[..got]);
    }
}

fn client(ports: Vec<u16>, secret: Vec<u8>, seed: u64, until: Instant) -> u64 {
    let connect = |k: usize| {
        let s = TcpStream::connect(("127.0.0.1", ports[k % ports.len()])).expect("training: the server is not listening");
        s.set_nodelay(true).unwrap();
        s
    };
    let mut conns: Vec<TcpStream> = (0..CONNECTIONS).map(connect).collect();
    let (mut x, mut done, mut buf, mut req) = (seed | 1, 0u64, Vec::new(), String::new());
    while Instant::now() < until {
        // one request on every connection, then every answer: the server sees them as one pass's arrivals
        for s in conns.iter_mut() {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            let (pick, id, user) = (x % 100, 400_000 + x % 100_000, 1 + x % 20_000);
            req.clear();
            let common = "Host: 127.0.0.1\r\nUser-Agent: mload\r\nAccept-Encoding: gzip\r\n";
            // the load test's mix: a feed, a post, a like after 15% of them and a new post after 2%
            if pick < 46 {
                req += &format!("GET /feed HTTP/1.1\r\n{common}\r\n");
            } else if pick < 92 {
                req += &format!("GET /posts/{id} HTTP/1.1\r\n{common}\r\n");
            } else if pick < 99 {
                req += &format!("POST /posts/{id}/like HTTP/1.1\r\n{common}Authorization: Bearer {}\r\nContent-Length: 0\r\n\r\n", token(&secret, user));
            } else {
                let body = format!(r#"{{"body":"clever_nebula_{user} says hi at 2026-11-14T05:26:05.229Z (VU {user}, iter {})"}}"#, x % 14);
                req += &format!(
                    "POST /posts HTTP/1.1\r\n{common}Authorization: Bearer {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n\r\n{body}",
                    token(&secret, user),
                    body.len()
                );
            }
            s.write_all(req.as_bytes()).expect("training: write");
        }
        for s in conns.iter_mut() {
            response(s, &mut buf).expect("training: no answer");
            done += 1;
        }
        // some users leave and new ones arrive, as in the ramp
        let k = (x % CONNECTIONS as u64) as usize;
        conns[k] = connect(k);
    }
    done
}

/// Drives the server for `seconds` and ends the process.
pub fn run(ports: Vec<u16>, secret: Vec<u8>, seconds: u64) {
    std::thread::spawn(move || {
        let started = Instant::now();
        while TcpStream::connect(("127.0.0.1", ports[0])).is_err() {
            assert!(started.elapsed() < Duration::from_secs(10), "training: the server did not start");
            std::thread::sleep(Duration::from_millis(20));
        }
        let until = Instant::now() + Duration::from_secs(seconds);
        let threads: Vec<_> = (0..CLIENT_THREADS)
            .map(|t| {
                let (ports, secret) = (ports.clone(), secret.clone());
                std::thread::spawn(move || client(ports, secret, 0x9e37_79b9_7f4a_7c15 * (t as u64 + 1), until))
            })
            .collect();
        let requests: u64 = threads.into_iter().map(|t| t.join().expect("training: a client failed")).sum();
        eprintln!("feed: training served {requests} requests in {seconds} s");
        // `exit` runs the profile writer; a training run that served nothing is a failed one
        std::process::exit(if requests > 1000 { 0 } else { 1 });
    });
}
