//! A feed API on Postgres for the Million Users Challenge: one loop per CPU on an io_uring of its own (epoll
//! where the kernel has none for us), its own HTTP/1.1 and its own Postgres wire client. See README.md for the
//! design and the reasons behind it.

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
mod uring;

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

/// How many loops for `cpus` CPUs: one per CPU, except on two.
/// - On two CPUs, three loops. Two would be tied to a CPU each, one of them the CPU that takes the card's
///   interrupts; with three the scheduler gives that CPU one loop and the other CPU two (measured on a rented VM
///   with one queue, see README.md).
/// - From three CPUs up, one per CPU. Fewer measured better on that VM, but the entries that ran fewer on the
///   benchmark box itself scored lower there (README.md, "Loops").
fn default_loops(cpus: usize) -> usize {
    match cpus {
        2 => 3,
        n => n,
    }
}

/// Length of a pass in microseconds. Short where there is a loop per CPU, so a pass's batch and the burst of
/// Postgres replies it brings back stay small; longer on one or two CPUs, where fewer and fuller batches cost
/// less CPU and CPU is what runs out.
fn default_pass_us(cpus: usize) -> u64 {
    if cpus > 2 { 2_000 } else { 4_000 }
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

    let cpus = sys::allowed_cpus();
    let mut loops = number("FEED_LOOPS", default_loops(cpus)).max(1);
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

    // io_uring unless it is switched off or the kernel refuses it (containers and some CI runners do).
    let ring = match (var("FEED_IO").as_deref(), uring::probe()) {
        (Some("epoll"), _) => false,
        (Some("uring"), Err(e)) => die(format!("FEED_IO=uring, but io_uring is not available (errno {e})")),
        (None | Some("uring"), Ok(())) => true,
        (None, Err(e)) => {
            eprintln!("feed: io_uring is not available here (errno {e}), the loops run on epoll");
            false
        }
        (Some(other), _) => die(format!("FEED_IO is neither epoll nor uring: {other}")),
    };
    let ring_buffers = number("FEED_RING_BUFFERS", 4096u32);
    if !ring_buffers.is_power_of_two() || ring_buffers > 32_768 {
        die("FEED_RING_BUFFERS must be a power of two, at most 32768");
    }

    let cfg: &'static Config = Box::leak(Box::new(Config {
        pg,
        host,
        ports,
        jwt_secret,
        readers,
        writers,
        pass_ns: number("FEED_PASS_US", default_pass_us(cpus)) * 1_000,
        idle_class: number("FEED_SCHED_IDLE", 1u8) != 0,
        started_ns,
        ring,
        ring_buffers,
        buffer_ring: number("FEED_BUFFER_RING", 1u8) != 0,
    }));
    eprintln!(
        "feed: {loops} {} loops on {host}, ports {:?}, {readers} read + {writers} write Postgres connections per loop, up to {max_fds} descriptors",
        if ring { "io_uring" } else { "epoll" },
        cfg.ports
    );

    let table = Table::new(max_fds);
    for i in 1..loops {
        std::thread::Builder::new()
            .name(format!("loop-{i}"))
            .spawn(move || Loop::new(cfg, table, i).run())
            .unwrap_or_else(|e| die(format!("cannot start a loop thread: {e}")));
    }
    Loop::new(cfg, table, 0).run()
}
