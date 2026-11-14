mod crypto;
mod http;
mod json;
mod pg;
mod sys;
mod uring;
mod worker;

use std::sync::atomic::{AtomicBool, Ordering};

use crypto::HmacSha256Key;
use pg::{parse_database_url, resolve_ipv4};
use sys::{
    available_cpus, close, fork, getrlimit, kill, mallopt, monotonic_ns, nanosleep, prctl,
    setrlimit, signal, unshare, waitpid, RLimit, Timespec, CLONE_FILES, M_ARENA_MAX,
    PR_SET_PDEATHSIG, PR_SET_THP_DISABLE, RLIMIT_MEMLOCK, RLIMIT_NOFILE, SIGINT, SIGPIPE,
    SIGTERM, SIG_IGN,
};
use worker::{run_worker, WorkerConfig, WORKER_STOP};

static SHUTDOWN: AtomicBool = AtomicBool::new(false);

extern "C" fn on_term_signal(_sig: i32) {
    SHUTDOWN.store(true, Ordering::Relaxed);
}

extern "C" fn on_worker_term(_sig: i32) {
    WORKER_STOP.store(true, Ordering::Relaxed);
}

fn env_usize(name: &str, default: usize) -> usize {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .unwrap_or(default)
}

fn raise_nofile_limit() -> usize {
    let mut rl = RLimit::default();
    if unsafe { getrlimit(RLIMIT_NOFILE, &mut rl) } == 0 {
        if rl.rlim_cur < rl.rlim_max {
            let raised = RLimit {
                rlim_cur: rl.rlim_max,
                rlim_max: rl.rlim_max,
            };
            if unsafe { setrlimit(RLIMIT_NOFILE, &raised) } == 0 {
                rl.rlim_cur = rl.rlim_max;
            }
        }
        return (rl.rlim_cur as usize).clamp(4096, 8_388_608);
    }
    1_048_576
}

fn raise_memlock_limit() {
    let mut rl = RLimit::default();
    if unsafe { getrlimit(RLIMIT_MEMLOCK, &mut rl) } == 0 && rl.rlim_cur < rl.rlim_max {
        let raised = RLimit {
            rlim_cur: rl.rlim_max,
            rlim_max: rl.rlim_max,
        };
        unsafe {
            setrlimit(RLIMIT_MEMLOCK, &raised);
        }
    }
}

fn main() {
    unsafe {
        for fd in 3..1024 {
            close(fd);
        }
        signal(SIGPIPE, SIG_IGN);
        prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
    }

    let rlim_nofile = raise_nofile_limit();
    raise_memlock_limit();
    let boot_ns = monotonic_ns();

    let host = std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".to_string());
    let port = std::env::var("PORT").unwrap_or_else(|_| "3000".to_string());
    let extra_port = std::env::var("EXTRA_PORT")
        .ok()
        .filter(|s| !s.is_empty() && s != &port);
    let db_url = std::env::var("DATABASE_URL")
        .unwrap_or_else(|_| "postgres://feed:feed@127.0.0.1:5432/feed".to_string());
    let jwt_secret =
        std::env::var("JWT_SECRET").unwrap_or_else(|_| "million-users-challenge".to_string());

    let listen_addr_0 = match resolve_ipv4(&host, &port) {
        Ok(a) => a,
        Err(e) => {
            eprintln!("invalid HOST/PORT: {}", e);
            std::process::exit(1);
        }
    };
    let listen_addr_1 = match extra_port {
        Some(ref ep) => match resolve_ipv4(&host, ep) {
            Ok(a) => Some(a),
            Err(e) => {
                eprintln!("invalid EXTRA_PORT: {}", e);
                std::process::exit(1);
            }
        },
        None => None,
    };

    let db = match parse_database_url(&db_url) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("invalid DATABASE_URL: {}", e);
            std::process::exit(1);
        }
    };

    let hmac_key = HmacSha256Key::new(jwt_secret.as_bytes());
    let detected_cpus = available_cpus().max(1);
    let workers = env_usize("FEED_WORKERS", detected_cpus).clamp(1, 64);

    let default_read_lanes = if workers == 1 {
        36
    } else if workers == 2 {
        28
    } else if workers <= 4 {
        16
    } else if workers <= 8 {
        14
    } else {
        (384 / workers).max(4)
    };
    let default_write_lanes = if workers == 1 {
        8
    } else if workers == 2 {
        6
    } else if workers <= 8 {
        4
    } else {
        3
    };
    let mut read_lanes = env_usize("FEED_READ_LANES", default_read_lanes).max(1);
    let mut write_lanes = env_usize("FEED_WRITE_LANES", default_write_lanes).max(1);

    while workers * (read_lanes + write_lanes) > 480 {
        if read_lanes > 2 {
            read_lanes -= 1;
        } else if write_lanes > 1 {
            write_lanes -= 1;
        } else if read_lanes > 1 {
            read_lanes -= 1;
        } else {
            break;
        }
    }

    let max_read_batch = env_usize("FEED_MAX_READ_BATCH", 48).clamp(1, 256);
    let max_write_batch = env_usize("FEED_MAX_WRITE_BATCH", 64).clamp(1, 256);
    let min_read_batch = env_usize("FEED_MIN_READ_BATCH", 32).clamp(1, max_read_batch);
    let min_write_batch = env_usize("FEED_MIN_WRITE_BATCH", 16).clamp(1, max_write_batch);
    let horizon_us = env_usize("FEED_HORIZON_US", 0) as u64;
    let horizon_off = env_usize("FEED_HORIZON_OFF", 0) != 0;
    let lifo_age_ms = env_usize("FEED_LIFO_MS", 15) as u32;
    let gz_level = env_usize("FEED_GZ_LEVEL", 0).min(3) as i32;
    let gz_train_huff = env_usize("FEED_GZ_TRAIN_HUFF", 1) != 0;
    let gz_min_bytes = env_usize("FEED_GZ_MIN_BYTES", 1024);
    let piggyback_ack = env_usize("FEED_PIGGYBACK_ACK", 1) != 0;
    let per_send_quickack = env_usize("FEED_PER_SEND_QUICKACK", 0) != 0;
    let try_bbr = env_usize("FEED_TCP_BBR", 1) != 0;
    let use_uring = std::env::var("FEED_REACTOR")
        .map(|v| v != "epoll")
        .unwrap_or_else(|_| env_usize("FEED_URING", 1) != 0);
    let uring_pbuf_rx = env_usize("FEED_URING_PBUF_RX", 1) != 0;
    let uring_pg_io = env_usize("FEED_URING_PG", 1) != 0;
    let direct_fd = env_usize("FEED_DIRECT_FD", 1) != 0;
    let batch_min = env_usize("FEED_BATCH_MIN", 24).clamp(1, 256) as u32;
    let batch_wait_us = env_usize("FEED_BATCH_WAIT_US", 0) as u32;
    let pace_max_us = env_usize("FEED_PACE_MAX_US", 0) as u32;

    let per_worker_fd_target = (3_400_000 / workers) + 8192;
    let max_fds = per_worker_fd_target
        .min(rlim_nofile)
        .min(1_048_576)
        .max(4096);

    let base_cfg = WorkerConfig {
        worker_id: 0,
        num_workers: workers,
        listen_addr_0,
        listen_addr_1,
        db,
        hmac_key,
        read_lanes,
        write_lanes,
        max_read_batch,
        max_write_batch,
        min_read_batch,
        min_write_batch,
        horizon_us,
        horizon_off,
        lifo_age_ms,
        gz_level,
        gz_train_huff,
        gz_min_bytes,
        piggyback_ack,
        per_send_quickack,
        try_bbr,
        use_uring,
        uring_pbuf_rx,
        uring_pg_io,
        direct_fd,
        batch_min,
        batch_wait_us,
        pace_max_us,
        max_fds,
        boot_ns,
    };

    if workers == 1 {
        unsafe {
            unshare(CLONE_FILES);
            mallopt(M_ARENA_MAX, 1);
            signal(SIGTERM, on_worker_term as *const () as usize);
            signal(SIGINT, on_worker_term as *const () as usize);
        }
        run_worker(base_cfg);
        return;
    }

    unsafe {
        signal(SIGTERM, on_term_signal as *const () as usize);
        signal(SIGINT, on_term_signal as *const () as usize);
    }

    let mut children = Vec::with_capacity(workers);
    for wid in 0..workers {
        let pid = unsafe { fork() };
        if pid < 0 {
            eprintln!("fork() failed for worker {}", wid);
            for &c in &children {
                unsafe {
                    kill(c, SIGTERM);
                }
            }
            std::process::exit(1);
        }
        if pid == 0 {
            unsafe {
                unshare(CLONE_FILES);
                mallopt(M_ARENA_MAX, 1);
                signal(SIGTERM, on_worker_term as *const () as usize);
                signal(SIGINT, on_worker_term as *const () as usize);
                prctl(PR_SET_PDEATHSIG, SIGTERM as u64, 0, 0, 0);
            }
            let mut wcfg = base_cfg.clone();
            wcfg.worker_id = wid;
            run_worker(wcfg);
            std::process::exit(0);
        }
        children.push(pid);
    }

    let ts = Timespec {
        tv_sec: 0,
        tv_nsec: 50_000_000,
    };
    let mut exit_code = 0;
    while !SHUTDOWN.load(Ordering::Relaxed) {
        let mut status: i32 = 0;
        let dead = unsafe { waitpid(-1, &mut status, 1) };
        if dead > 0 && !SHUTDOWN.load(Ordering::Relaxed) {
            eprintln!("worker pid {} exited unexpectedly (status={})", dead, status);
            exit_code = 1;
            break;
        }
        unsafe {
            nanosleep(&ts, std::ptr::null_mut());
        }
    }

    for &c in &children {
        unsafe {
            kill(c, SIGTERM);
        }
    }
    for &c in &children {
        let mut status: i32 = 0;
        unsafe {
            waitpid(c, &mut status, 0);
        }
    }
    if exit_code != 0 {
        std::process::exit(exit_code);
    }
}
