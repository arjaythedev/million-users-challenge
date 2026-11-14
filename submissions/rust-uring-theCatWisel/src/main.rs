mod crypto;
mod http;
mod json;
mod pg;
mod sys;
mod uring;
mod worker;

use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};

use crypto::HmacSha256Key;
use pg::{parse_database_url, resolve_ipv4};
use sys::{
    available_cpus, close, dl_iterate_phdr, fork, getrlimit, kill, madvise, mallopt, mmap,
    monotonic_ns, mprotect, mremap, munmap, nanosleep, prctl, setrlimit, signal, syscall, unshare,
    waitpid, DlPhdrInfo, RLimit, Timespec, CLONE_FILES, MADV_COLLAPSE, MADV_HUGEPAGE,
    MAP_ANONYMOUS, MAP_FIXED_NOREPLACE, MAP_PRIVATE, MREMAP_FIXED, MREMAP_MAYMOVE, M_ARENA_MAX,
    PF_W, PF_X, PROT_EXEC, PROT_NONE, PROT_READ, PROT_WRITE, PR_SET_PDEATHSIG, PR_SET_THP_DISABLE,
    PR_THP_DISABLE_EXCEPT_ADVISED, PT_LOAD, RLIMIT_MEMLOCK, RLIMIT_NOFILE, SIGINT, SIGPIPE,
    SIGTERM, SIG_IGN, SYS_MSEAL,
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

unsafe extern "C" fn hugify_phdr_cb(
    info: *mut DlPhdrInfo,
    _size: usize,
    _data: *mut c_void,
) -> i32 {
    const TWO_MIB: usize = 2 * 1024 * 1024;
    const PAGE_SZ: usize = 4096;
    const MAP_FAILED: *mut c_void = !0usize as *mut c_void;

    let info_ref = &*info;
    if !info_ref.dlpi_name.is_null() && *info_ref.dlpi_name != 0 {
        return 0;
    }
    if info_ref.dlpi_phdr.is_null() || info_ref.dlpi_phnum == 0 {
        return 0;
    }
    let phdrs = std::slice::from_raw_parts(info_ref.dlpi_phdr, info_ref.dlpi_phnum as usize);
    let base = info_ref.dlpi_addr;

    for (idx, ph) in phdrs.iter().enumerate() {
        if ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0 || (ph.p_flags & PF_W) != 0 {
            continue;
        }
        let seg_start = base.wrapping_add(ph.p_vaddr as usize);
        let seg_end_raw = seg_start.wrapping_add(ph.p_memsz as usize);
        if (seg_start & (TWO_MIB - 1)) != 0 || seg_end_raw <= seg_start {
            continue;
        }
        let page_end = (seg_end_raw + (PAGE_SZ - 1)) & !(PAGE_SZ - 1);
        let slot_end = (page_end + (TWO_MIB - 1)) & !(TWO_MIB - 1);
        let slot_len = slot_end - seg_start;
        let copy_len = page_end - seg_start;

        let mut overlaps_other = false;
        for (j, other) in phdrs.iter().enumerate() {
            if j == idx || other.p_type != PT_LOAD || other.p_memsz == 0 {
                continue;
            }
            let o_start = base.wrapping_add(other.p_vaddr as usize) & !(PAGE_SZ - 1);
            let o_end = (base
                .wrapping_add(other.p_vaddr as usize)
                .wrapping_add(other.p_memsz as usize)
                + (PAGE_SZ - 1))
                & !(PAGE_SZ - 1);
            if o_start < slot_end && o_end > seg_start {
                overlaps_other = true;
                break;
            }
        }
        if overlaps_other {
            continue;
        }

        if copy_len == slot_len
            && madvise(seg_start as *mut c_void, slot_len, MADV_COLLAPSE) == 0
        {
            #[cfg(target_arch = "x86_64")]
            {
                let _ = syscall(SYS_MSEAL, seg_start, slot_len, 0usize);
            }
            continue;
        }

        let gap_len = slot_end - page_end;
        if gap_len > 0 {
            let gap_map = mmap(
                page_end as *mut c_void,
                gap_len,
                PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                -1,
                0,
            );
            if gap_map == MAP_FAILED || (gap_map as usize) != page_end {
                if gap_map != MAP_FAILED {
                    let _ = munmap(gap_map, gap_len);
                }
                continue;
            }
        }

        let raw_len = slot_len + TWO_MIB;
        let raw = mmap(
            std::ptr::null_mut(),
            raw_len,
            PROT_NONE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0,
        );
        if raw == MAP_FAILED {
            if gap_len > 0 {
                let _ = munmap(page_end as *mut c_void, gap_len);
            }
            continue;
        }
        let raw_addr = raw as usize;
        let cand_addr = (raw_addr + (TWO_MIB - 1)) & !(TWO_MIB - 1);
        let head_trim = cand_addr - raw_addr;
        let tail_trim = (raw_addr + raw_len) - (cand_addr + slot_len);
        if head_trim > 0 {
            let _ = munmap(raw, head_trim);
        }
        if tail_trim > 0 {
            let _ = munmap((cand_addr + slot_len) as *mut c_void, tail_trim);
        }
        let cand = cand_addr as *mut c_void;

        let mut collapsed = false;
        if mprotect(cand, slot_len, PROT_READ | PROT_WRITE) == 0 {
            let _ = madvise(cand, slot_len, MADV_HUGEPAGE);
            std::ptr::copy_nonoverlapping(seg_start as *const u8, cand as *mut u8, copy_len);
            if gap_len > 0 {
                std::ptr::write_bytes((cand as *mut u8).add(copy_len), 0xCC, gap_len);
            }
            if madvise(cand, slot_len, MADV_COLLAPSE) == 0
                && mprotect(cand, slot_len, PROT_READ | PROT_EXEC) == 0
            {
                let moved = mremap(
                    cand,
                    slot_len,
                    slot_len,
                    MREMAP_MAYMOVE | MREMAP_FIXED,
                    seg_start as *mut c_void,
                );
                if moved != MAP_FAILED && (moved as usize) == seg_start {
                    collapsed = true;
                    #[cfg(target_arch = "x86_64")]
                    {
                        let _ = syscall(SYS_MSEAL, seg_start, slot_len, 0usize);
                    }
                } else if moved != MAP_FAILED {
                    let _ = munmap(moved, slot_len);
                    collapsed = true;
                }
            }
        }
        if !collapsed {
            let _ = munmap(cand, slot_len);
            if gap_len > 0 {
                let _ = munmap(page_end as *mut c_void, gap_len);
            }
        }
    }
    1
}

fn hugify_and_fence_thp() {
    unsafe {
        let _ = dl_iterate_phdr(hugify_phdr_cb, std::ptr::null_mut());
        if prctl(
            PR_SET_THP_DISABLE,
            1,
            PR_THP_DISABLE_EXCEPT_ADVISED,
            0,
            0,
        ) != 0
        {
            let _ = prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
        }
    }
}

fn main() {
    unsafe {
        for fd in 3..1024 {
            close(fd);
        }
        signal(SIGPIPE, SIG_IGN);
    }
    hugify_and_fence_thp();

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
