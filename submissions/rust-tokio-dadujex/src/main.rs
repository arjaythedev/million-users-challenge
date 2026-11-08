//! Feed API for the million users challenge (SPEC.md).
//!
//! Thread per core: every worker thread runs its own current-thread tokio runtime with its own SO_REUSEPORT
//! listeners on both ports and its own Postgres connections. Threads share nothing but the read-only config,
//! and the kernel spreads new connections over the listeners. Connections are served by a small HTTP/1.1 loop
//! (http.rs) that holds no buffer while a connection is idle, which is what a million keep-alive connections
//! need: the memory per idle connection is its tokio task and socket registration, under 1 KB.

mod api;
mod auth;
mod db;
mod http;
mod json;

use std::{
    cell::RefCell,
    env,
    net::{SocketAddr, ToSocketAddrs},
    time::{Duration, Instant},
};

use libdeflater::{CompressionLvl, Compressor};
use tokio::net::{TcpListener, TcpSocket};

#[global_allocator]
static GLOBAL: mimalloc::MiMalloc = mimalloc::MiMalloc;

/// Listen backlog (the kernel caps it at net.core.somaxconn, 65535 on the box).
const LISTEN_BACKLOG: u32 = 65535;
/// Rule 5: at most 512 connections to Postgres.
const MAX_PG_CONNECTIONS: usize = 512;

/// Read-only settings, shared by every worker thread.
pub struct Config {
    pub pg: tokio_postgres::Config,
    pub jwt: auth::Jwt,
    pub listen: Vec<SocketAddr>,
    pub started: Instant,
    pub threads: usize,
    /// Postgres connections per worker thread.
    pub read_conns: usize,
    pub write_conns: usize,
    /// libdeflate level for gzip responses; 0 turns gzip off.
    pub gzip_level: i32,
}

/// One worker thread's state. Leaked, so tasks can hold `&'static Ctx`.
pub struct Ctx {
    pub cfg: &'static Config,
    pub db: db::Pool,
    /// Scratch buffers for building responses: one per thread, so a connection keeps none of its own.
    pub body: RefCell<Vec<u8>>,
    pub out: RefCell<Vec<u8>>,
    pub gzip: Option<RefCell<(Compressor, Vec<u8>)>>,
}

fn main() {
    let cfg: &'static Config = Box::leak(Box::new(Config::from_env()));
    println!(
        "[server] {} worker threads, {} read + {} write Postgres connections each ({} total), gzip level {}",
        cfg.threads,
        cfg.read_conns,
        cfg.write_conns,
        cfg.threads * (cfg.read_conns + cfg.write_conns),
        cfg.gzip_level,
    );
    let workers: Vec<_> = (0..cfg.threads)
        .map(|i| {
            std::thread::Builder::new()
                .name(format!("worker-{i}"))
                .spawn(move || {
                    let _exit = ExitOnDrop;
                    worker(i, cfg)
                })
                .unwrap_or_else(|e| fail(format!("could not start a worker thread: {e}")))
        })
        .collect();
    for w in workers {
        let _ = w.join();
    }
}

/// A worker thread only ends by panicking: take the process down rather than serve on fewer threads.
struct ExitOnDrop;

impl Drop for ExitOnDrop {
    fn drop(&mut self) {
        eprintln!("[server] a worker thread stopped, exiting");
        std::process::exit(1);
    }
}

fn worker(index: usize, cfg: &'static Config) {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap_or_else(|e| fail(format!("could not build the tokio runtime: {e}")));
    let local = tokio::task::LocalSet::new();
    local.block_on(&rt, async move {
        let ctx: &'static Ctx = Box::leak(Box::new(Ctx::new(cfg)));
        // Listen only once this thread's Postgres connections are up, so the first requests it accepts
        // find them ready.
        ctx.db.open(cfg).await;
        for &addr in &cfg.listen {
            let listener = listen(addr);
            if index == 0 {
                println!("[server] listening on http://{addr}");
            }
            tokio::task::spawn_local(accept_loop(listener, ctx));
        }
        std::future::pending::<()>().await
    })
}

fn listen(addr: SocketAddr) -> TcpListener {
    let socket = if addr.is_ipv4() { TcpSocket::new_v4() } else { TcpSocket::new_v6() };
    socket
        .and_then(|s| {
            s.set_reuseaddr(true)?;
            // One listener per thread on the same port: the kernel balances new connections over them.
            s.set_reuseport(true)?;
            s.bind(addr)?;
            s.listen(LISTEN_BACKLOG)
        })
        .unwrap_or_else(|e| fail(format!("could not listen on {addr}: {e}")))
}

async fn accept_loop(listener: TcpListener, ctx: &'static Ctx) {
    loop {
        match listener.accept().await {
            Ok((stream, _)) => {
                // Responses go out in one write; don't let Nagle hold back the last segment of a feed.
                let _ = stream.set_nodelay(true);
                tokio::task::spawn_local(http::serve(stream, ctx));
            }
            // Out of file descriptors or memory: back off for a moment instead of spinning on the error.
            Err(e) => {
                eprintln!("[server] accept: {e}");
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
        }
    }
}

impl Ctx {
    fn new(cfg: &'static Config) -> Ctx {
        let gzip = (cfg.gzip_level > 0).then(|| {
            let level = CompressionLvl::new(cfg.gzip_level).unwrap_or_default();
            RefCell::new((Compressor::new(level), Vec::new()))
        });
        Ctx {
            cfg,
            db: db::Pool::new(cfg.read_conns, cfg.write_conns),
            body: RefCell::new(Vec::with_capacity(16 * 1024)),
            out: RefCell::new(Vec::with_capacity(16 * 1024)),
            gzip,
        }
    }
}

impl Config {
    /// SPEC.md's runtime contract (DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT), plus optional tuning:
    /// THREADS, DB_READ_CONNS / DB_WRITE_CONNS (totals over all threads) and GZIP_LEVEL.
    fn from_env() -> Config {
        let url = env::var("DATABASE_URL").unwrap_or_else(|_| fail("DATABASE_URL must be set"));
        let mut pg: tokio_postgres::Config =
            url.parse().unwrap_or_else(|e| fail(format!("DATABASE_URL: {e}")));
        pg.application_name("feed-server").connect_timeout(Duration::from_secs(10));
        let secret = env::var("JWT_SECRET").unwrap_or_else(|_| fail("JWT_SECRET must be set"));

        let host = var("HOST").unwrap_or_else(|| "0.0.0.0".into());
        let mut ports = vec![number("PORT").unwrap_or(3000)];
        ports.extend(number::<u16>("EXTRA_PORT"));
        let listen = ports
            .iter()
            .map(|&port| {
                (host.as_str(), port)
                    .to_socket_addrs()
                    .ok()
                    .and_then(|mut a| a.next())
                    .unwrap_or_else(|| fail(format!("could not resolve HOST {host}")))
            })
            .collect();

        // available_parallelism() follows the CPU affinity mask and cgroup quota: the cores we were given.
        let cores = std::thread::available_parallelism().map_or(1, |n| n.get());
        let threads = number("THREADS").unwrap_or(cores).max(1);
        let read_conns = number("DB_READ_CONNS").unwrap_or(128usize).div_ceil(threads).max(1);
        let write_conns = number("DB_WRITE_CONNS").unwrap_or(32usize).div_ceil(threads).max(1);
        if threads * (read_conns + write_conns) > MAX_PG_CONNECTIONS {
            fail(format!(
                "{threads} threads x {} Postgres connections is over {MAX_PG_CONNECTIONS}",
                read_conns + write_conns
            ));
        }
        Config {
            pg,
            jwt: auth::Jwt::new(secret.as_bytes()),
            listen,
            started: Instant::now(),
            threads,
            read_conns,
            write_conns,
            gzip_level: number("GZIP_LEVEL").unwrap_or(1i32).clamp(0, 12),
        }
    }
}

fn var(key: &str) -> Option<String> {
    env::var(key).ok().filter(|v| !v.trim().is_empty())
}

fn number<T: std::str::FromStr>(key: &str) -> Option<T> {
    var(key).map(|v| v.trim().parse().unwrap_or_else(|_| fail(format!("{key} must be a number, got {v:?}"))))
}

fn fail(msg: impl std::fmt::Display) -> ! {
    eprintln!("[server] {msg}");
    std::process::exit(1)
}
