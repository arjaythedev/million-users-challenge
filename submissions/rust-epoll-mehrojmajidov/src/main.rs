//! A feed API on Postgres for the Million Users Challenge: one epoll loop per CPU, its own HTTP/1.1 and its own
//! Postgres wire client. See README.md for the design and the reasons behind it.

mod crypto;
mod db;
mod gzip;
mod http;
mod json;
mod jwt;
mod pg;
mod reactor;
mod sys;
mod time;

use reactor::{Config, Loop, Table};
use std::net::{IpAddr, Ipv4Addr};

/// Rule 5 of the challenge: at most 512 open connections to Postgres in total.
const MAX_PG_CONNECTIONS: usize = 512;

fn die(message: impl AsRef<str>) -> ! {
    eprintln!("feed: {}", message.as_ref());
    std::process::exit(1)
}

fn var(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

fn number<T: std::str::FromStr>(name: &str, default: T) -> T {
    match var(name) {
        Some(v) => v.parse().unwrap_or_else(|_| die(format!("{name} is not a valid number: {v}"))),
        None => default,
    }
}

fn main() {
    let started_ns = sys::mono_ns();
    let url = var("DATABASE_URL").unwrap_or_else(|| die("DATABASE_URL is not set"));
    let pg = pg::PgConfig::from_url(&url).unwrap_or_else(|e| die(e));
    let jwt_secret = var("JWT_SECRET").unwrap_or_else(|| die("JWT_SECRET is not set")).into_bytes();
    let host: IpAddr = match var("HOST").as_deref() {
        None => Ipv4Addr::UNSPECIFIED.into(),
        Some("localhost") => Ipv4Addr::LOCALHOST.into(),
        Some(h) => h.parse().unwrap_or_else(|_| die(format!("HOST is not an IP address: {h}"))),
    };
    let mut ports = vec![number("PORT", 3000u16), number("EXTRA_PORT", 3001u16)];
    ports.dedup();

    // Every connection is a file descriptor; the connection table is indexed by it.
    let mut limit = sys::Rlimit { cur: 0, max: 0 };
    unsafe {
        sys::getrlimit(sys::RLIMIT_NOFILE, &mut limit);
        if limit.cur < limit.max {
            limit.cur = limit.max;
            sys::setrlimit(sys::RLIMIT_NOFILE, &limit);
            sys::getrlimit(sys::RLIMIT_NOFILE, &mut limit);
        }
    }
    let max_fds = limit.cur.min(1 << 26) as usize;

    let mut loops = number("FEED_LOOPS", sys::allowed_cpus()).max(1);
    let mut readers = number("FEED_PG_READERS", (48 / loops).max(2)).max(1);
    let mut writers = number("FEED_PG_WRITERS", (32 / loops).max(2)).max(1);
    while loops * (readers + writers) > MAX_PG_CONNECTIONS {
        if readers > 1 {
            readers -= 1;
        } else if writers > 1 {
            writers -= 1;
        } else {
            loops = MAX_PG_CONNECTIONS / 2;
        }
    }

    let cfg: &'static Config = Box::leak(Box::new(Config {
        pg,
        host,
        ports,
        jwt_secret,
        readers,
        writers,
        pass_ns: number("FEED_PASS_US", 2_000u64) * 1_000,
        idle_class: number("FEED_SCHED_IDLE", 1u8) != 0,
        started_ns,
    }));
    eprintln!(
        "feed: {loops} loops on {host}, ports {:?}, {readers} read + {writers} write Postgres connections per loop, up to {max_fds} descriptors",
        cfg.ports
    );

    let table = Table::new(max_fds);
    for i in 1..loops {
        std::thread::Builder::new()
            .name(format!("loop-{i}"))
            .spawn(move || Loop::new(cfg, table).run())
            .unwrap_or_else(|e| die(format!("cannot start a loop thread: {e}")));
    }
    Loop::new(cfg, table).run()
}
