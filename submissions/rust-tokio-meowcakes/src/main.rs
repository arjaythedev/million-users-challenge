mod db;
mod gzip;
mod http;
mod json;
mod jwt;
mod pg;
mod uring;

use std::cell::RefCell;
use std::net::{SocketAddr, ToSocketAddrs};
use std::str::FromStr;
use std::time::Instant;

pub struct Config {
    pub pg: pg::ConnectParams,
    pub jwt: jwt::Key,
    pub listen: Vec<SocketAddr>,
    pub threads: usize,
    pub read_conns: usize,
    pub write_conns: usize,
    pub gzip_min: usize,
    pub io_uring: bool,
    pub batch_wait: std::time::Duration,
    pub batch_min: usize,
    pub started: Instant,
}

pub struct Worker {
    pub cfg: &'static Config,
    pub db: db::Db,
    pub ring: Option<&'static uring::Ring>,
    pub rbuf: RefCell<Vec<u8>>,
    pub out: RefCell<Vec<u8>>,
    pub gzip: RefCell<(gzip::Gzip, Vec<u8>)>,
    pool: RefCell<Vec<Vec<u8>>>,
}

impl Worker {
    pub fn take_buf(&self) -> Vec<u8> {
        self.pool
            .borrow_mut()
            .pop()
            .unwrap_or_else(|| Vec::with_capacity(1024))
    }

    pub fn give_buf(&self, mut b: Vec<u8>) {
        let mut pool = self.pool.borrow_mut();
        if pool.len() < 4096 && b.capacity() > 0 && b.capacity() <= 64 * 1024 {
            b.clear();
            pool.push(b);
        }
    }
}

fn env<T: FromStr>(name: &str) -> Option<T> {
    std::env::var(name).ok().and_then(|v| v.parse().ok())
}

fn required(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| {
        eprintln!("{name} is not set");
        std::process::exit(1)
    })
}

fn main() {
    let pg = pg::ConnectParams::from_url(&required("DATABASE_URL")).unwrap_or_else(|e| {
        eprintln!("DATABASE_URL: {e}");
        std::process::exit(1)
    });
    let jwt = jwt::Key::new(required("JWT_SECRET").as_bytes());
    let host = std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into());
    let mut ports = vec![env::<u16>("PORT").unwrap_or(3000)];
    if let Some(p) = env::<u16>("EXTRA_PORT") {
        ports.push(p);
    }
    let listen: Vec<SocketAddr> = ports
        .iter()
        .map(|&p| {
            (host.as_str(), p)
                .to_socket_addrs()
                .ok()
                .and_then(|mut a| a.next())
                .unwrap_or_else(|| {
                    eprintln!("cannot resolve {host}:{p}");
                    std::process::exit(1)
                })
        })
        .collect();

    let threads = env("THREADS")
        .unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get()));
    let read_conns = env("PG_READ_CONNS").unwrap_or(96usize.div_ceil(threads).max(2));
    let write_conns = env("PG_WRITE_CONNS").unwrap_or(32usize.div_ceil(threads).max(1));
    assert!(
        threads * (read_conns + write_conns) <= 512,
        "more than 512 Postgres connections"
    );

    let cfg: &'static Config = Box::leak(Box::new(Config {
        pg,
        jwt,
        listen,
        threads,
        read_conns,
        write_conns,
        gzip_min: env("GZIP_MIN").unwrap_or(1024),
        io_uring: env("IO_URING").unwrap_or(1) != 0,
        batch_wait: std::time::Duration::from_micros(env("BATCH_WAIT_US").unwrap_or(5000)),
        batch_min: env("BATCH_MIN").unwrap_or(16),
        started: Instant::now(),
    }));
    eprintln!(
        "feed: {} threads, {} read + {} write Postgres connections each, {} on {:?}",
        cfg.threads,
        cfg.read_conns,
        cfg.write_conns,
        if cfg.io_uring {
            format!(
                "io_uring (batch: {} completions or {:?})",
                cfg.batch_min, cfg.batch_wait
            )
        } else {
            "epoll".into()
        },
        cfg.listen
    );

    let handles: Vec<_> = (0..cfg.threads)
        .map(|i| {
            std::thread::Builder::new()
                .name(format!("w{i}"))
                .spawn(move || run_worker(cfg))
                .unwrap()
        })
        .collect();
    for h in handles {
        let _ = h.join();
    }
}

fn run_worker(cfg: &'static Config) {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_io()
        .enable_time()
        .build()
        .unwrap();
    let local = tokio::task::LocalSet::new();
    let listeners: Vec<std::net::TcpListener> = cfg
        .listen
        .iter()
        .map(|&addr| {
            http::bind(addr).unwrap_or_else(|e| {
                eprintln!("listen on {addr}: {e}");
                std::process::exit(1)
            })
        })
        .collect();
    let ring = cfg
        .io_uring
        .then(|| uring::Ring::new(cfg))
        .and_then(|r| match r {
            Ok(r) => Some(&*Box::leak(Box::new(r))),
            Err(e) => {
                eprintln!("io_uring unavailable ({e}): using epoll");
                None
            }
        });
    local.block_on(&rt, async move {
        let w: &'static Worker = Box::leak(Box::new(Worker {
            cfg,
            db: db::Db::new(cfg),
            ring,
            rbuf: RefCell::new(vec![0; 64 * 1024]),
            out: RefCell::new(Vec::with_capacity(64 * 1024)),
            gzip: RefCell::new((gzip::Gzip::new(), Vec::with_capacity(16 * 1024))),
            pool: RefCell::new(Vec::new()),
        }));
        db::start(w);
        match ring {
            Some(r) => uring::start(w, r, listeners),
            None => {
                for l in listeners {
                    let l = tokio::net::TcpListener::from_std(l).expect("register listener");
                    tokio::task::spawn_local(http::accept_loop(l, w));
                }
            }
        }
        std::future::pending::<()>().await
    });
}
