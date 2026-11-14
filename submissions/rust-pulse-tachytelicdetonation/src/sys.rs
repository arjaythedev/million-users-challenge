use std::io;

pub fn mono_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

pub fn unix_s() -> i64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_REALTIME, &mut ts) };
    ts.tv_sec as i64
}

pub fn thread_cpu_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

pub fn affinity_count() -> usize {
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        if libc::sched_getaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &mut set) != 0 {
            return 1;
        }
        (libc::CPU_COUNT(&set) as usize).max(1)
    }
}

pub fn current_cpu() -> i32 {
    unsafe { libc::sched_getcpu() }
}

pub fn setopt(fd: i32, level: i32, opt: i32, val: i32) -> bool {
    unsafe {
        libc::setsockopt(fd, level, opt, &val as *const i32 as *const libc::c_void, 4) == 0
    }
}

pub fn set_nonblock(fd: i32) {
    unsafe {
        let fl = libc::fcntl(fd, libc::F_GETFL);
        libc::fcntl(fd, libc::F_SETFL, fl | libc::O_NONBLOCK);
    }
}

pub fn listener(ip: [u8; 4], port: u16, defer: i32, cpu: i32) -> io::Result<i32> {
    unsafe {
        let fd = libc::socket(libc::AF_INET, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0);
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        setopt(fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, 1);
        if !setopt(fd, libc::SOL_SOCKET, libc::SO_REUSEPORT, 1) {
            return Err(io::Error::last_os_error());
        }
        setopt(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, 1);
        if defer > 0 {
            setopt(fd, libc::IPPROTO_TCP, libc::TCP_DEFER_ACCEPT, defer);
        }
        setopt(fd, libc::IPPROTO_TCP, libc::TCP_THIN_LINEAR_TIMEOUTS, 1);
        setopt(fd, libc::IPPROTO_TCP, libc::TCP_QUICKACK, 0);
        if cpu >= 0 {
            setopt(fd, libc::SOL_SOCKET, libc::SO_INCOMING_CPU, cpu);
        }
        let mut sa: libc::sockaddr_in = std::mem::zeroed();
        sa.sin_family = libc::AF_INET as u16;
        sa.sin_port = port.to_be();
        sa.sin_addr.s_addr = u32::from_ne_bytes(ip);
        if libc::bind(fd, &sa as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_in>() as u32) != 0 {
            let e = io::Error::last_os_error();
            libc::close(fd);
            return Err(e);
        }
        if libc::listen(fd, 65535) != 0 {
            let e = io::Error::last_os_error();
            libc::close(fd);
            return Err(e);
        }
        setopt(fd, libc::IPPROTO_TCP, libc::TCP_QUICKACK, 0);
        Ok(fd)
    }
}

pub fn set_sched_idle() {
    unsafe {
        let p = libc::sched_param { sched_priority: 0 };
        libc::sched_setscheduler(0, libc::SCHED_IDLE, &p);
    }
}

pub fn nofile_limit() -> u64 {
    unsafe {
        let mut r: libc::rlimit = std::mem::zeroed();
        if libc::getrlimit(libc::RLIMIT_NOFILE, &mut r) != 0 {
            return 1024;
        }
        if r.rlim_cur < r.rlim_max {
            let want = libc::rlimit { rlim_cur: r.rlim_max, rlim_max: r.rlim_max };
            if libc::setrlimit(libc::RLIMIT_NOFILE, &want) == 0 {
                return r.rlim_max as u64;
            }
        }
        r.rlim_cur as u64
    }
}
