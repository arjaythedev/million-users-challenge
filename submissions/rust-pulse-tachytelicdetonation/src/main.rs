mod crc32;
mod fmt;
mod gzip;
mod http;
mod json;
mod jwt;
mod pg;
mod sha256;
mod sys;
mod uring;
mod worker;

use std::sync::atomic::AtomicUsize;
use worker::{Config, Shared};

fn env_or<T: std::str::FromStr>(k: &str, d: T) -> T {
    std::env::var(k).ok().and_then(|v| v.trim().parse().ok()).unwrap_or(d)
}

fn parse_ip(h: &str) -> [u8; 4] {
    if h == "localhost" {
        return [127, 0, 0, 1];
    }
    let parts: Vec<u8> = h.split('.').filter_map(|p| p.parse().ok()).collect();
    if parts.len() == 4 {
        [parts[0], parts[1], parts[2], parts[3]]
    } else {
        [0, 0, 0, 0]
    }
}

fn main() {
    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };
    let url = std::env::var("DATABASE_URL").unwrap_or_else(|_| "postgres://postgres@localhost:5432/feed".into());
    let pgcfg = match pg::PgCfg::from_url(&url) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("bad DATABASE_URL: {}", e);
            std::process::exit(1);
        }
    };
    let secret = std::env::var("JWT_SECRET").unwrap_or_else(|_| "million-users-challenge".into());
    let ip = parse_ip(&std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into()));
    let mut ports = vec![env_or("PORT", 3000u16)];
    if let Some(p) = std::env::var("EXTRA_PORT").ok().and_then(|v| v.parse::<u16>().ok()) {
        if p != ports[0] {
            ports.push(p);
        }
    }
    let n = env_or("PULSE_THREADS", sys::affinity_count()).max(1);
    let total_r: usize = env_or("PULSE_PG_READ", 32usize);
    let total_w: usize = env_or("PULSE_PG_WRITE", 16usize);
    let mut r = ((total_r + n / 2) / n).max(1);
    let mut w = ((total_w + n / 2) / n).max(1);
    while (r + w) * n > 500 && (r > 1 || w > 1) {
        if r > w {
            r -= 1;
        } else {
            w -= 1;
        }
    }
    let cfg = Config {
        pg: pgcfg,
        jwt: jwt::Jwt::new(secret.as_bytes()),
        ip,
        ports,
        reads: r,
        writes: w,
        batch_ns: env_or("PULSE_BATCH_US", 3000u64) * 1000,
        min_complete: env_or("PULSE_MIN_COMPLETE", 512u32),
        epoll: env_or("PULSE_EPOLL", 0u32) == 1,
        epoll_batch_ns: env_or("PULSE_EPOLL_BATCH_US", 0u64) * 1000,
        stats: env_or("PULSE_STATS", 0u32) == 1,
        defer: env_or("PULSE_DEFER_ACCEPT", 2i32),
        sched_idle: env_or("PULSE_SCHED_IDLE", 1u32) == 1,
        read_batch: env_or("PULSE_READ_BATCH", 128usize).max(1),
        write_batch: env_or("PULSE_WRITE_BATCH", 512usize).max(1),
        gz_fixed: env_or("PULSE_GZ_FIXED", 0u32) == 1,
        bufs: env_or("PULSE_BUFS", 4096u32).next_power_of_two().min(32768),
        buf_size: env_or("PULSE_BUF_SIZE", 2048usize),
        max_fixed: env_or("PULSE_FIXED_FILES", 1u32 << 20),
    };
    eprintln!(
        "pulse: {} workers, {} read + {} write postgres connections each, ports {:?}",
        n, r, w, cfg.ports
    );
    let sh: &'static Shared = Box::leak(Box::new(Shared { cfg, n, ready: AtomicUsize::new(0), start_s: sys::unix_s() }));
    let mut hs = Vec::new();
    for id in 0..n {
        hs.push(
            std::thread::Builder::new()
                .name(format!("pulse-{}", id))
                .stack_size(4 << 20)
                .spawn(move || worker::run(id, sh))
                .unwrap(),
        );
    }
    for h in hs {
        let _ = h.join();
    }
}
