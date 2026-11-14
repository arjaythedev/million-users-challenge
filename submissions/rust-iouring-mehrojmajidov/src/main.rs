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

/// How many loops for `cpus` CPUs. Not one per CPU. The box's card has a single queue: one CPU takes all of its
/// interrupts, and frees every transmitted packet under the lock that every sending CPU needs. That CPU fills up
/// first, while the rest of the box is half idle, and the more CPUs send at once, the longer it waits for the
/// lock. So the server sends from few loops (measured on a rented VM with one queue, see README.md):
/// - From three CPUs up, half as many loops as CPUs, at least two and at most four. At a million and a half
///   users a loop then uses about half a CPU, and the loops rarely sit on the card's CPU.
/// - On two CPUs, three loops. Two would be tied to a CPU each; with three the scheduler gives the card's CPU
///   one and the other CPU two.
fn default_loops(cpus: usize) -> usize {
    match cpus {
        1 => 1,
        2 => 3,
        n => (n / 2).clamp(2, 4),
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

    let mut loops = number("FEED_LOOPS", default_loops(sys::allowed_cpus())).max(1);
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
        pass_ns: number("FEED_PASS_US", 4_000u64) * 1_000,
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
