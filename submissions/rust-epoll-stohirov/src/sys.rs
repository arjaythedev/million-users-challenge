use std::net::IpAddr;

pub const EPOLLIN: u32 = libc::EPOLLIN as u32;
pub const EPOLLOUT: u32 = libc::EPOLLOUT as u32;
pub const EPOLLERR: u32 = libc::EPOLLERR as u32;
pub const EPOLLHUP: u32 = libc::EPOLLHUP as u32;
pub const EPOLLRDHUP: u32 = libc::EPOLLRDHUP as u32;
pub const EPOLLET: u32 = libc::EPOLLET as u32;

pub const EAGAIN: i32 = libc::EAGAIN;
pub const EINTR: i32 = libc::EINTR;

#[inline]
pub fn errno() -> i32 {
    unsafe { *libc::__errno_location() }
}

#[inline]
pub fn now_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

#[inline]
pub fn unix_secs() -> i64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_REALTIME_COARSE, &mut ts) };
    ts.tv_sec as i64
}

pub fn set_int(fd: i32, level: i32, name: i32, value: i32) -> i32 {
    unsafe {
        libc::setsockopt(fd, level, name, &value as *const i32 as *const libc::c_void, 4)
    }
}

pub fn get_int(fd: i32, level: i32, name: i32) -> i32 {
    let mut v: i32 = 0;
    let mut len: libc::socklen_t = 4;
    let r = unsafe { libc::getsockopt(fd, level, name, &mut v as *mut i32 as *mut libc::c_void, &mut len) };
    if r == 0 { v } else { -1 }
}

pub fn set_nodelay(fd: i32) {
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, 1);
}

#[inline]
pub fn clear_quickack(fd: i32) {
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_QUICKACK, 0);
}

pub fn listen(host: IpAddr, port: u16, backlog: i32, defer_accept: i32) -> Result<i32, String> {
    let family = if host.is_ipv6() { libc::AF_INET6 } else { libc::AF_INET };
    let fd = unsafe { libc::socket(family, libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC, 0) };
    if fd < 0 {
        return Err(format!("socket: errno {}", errno()));
    }
    set_int(fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, 1);
    set_int(fd, libc::SOL_SOCKET, libc::SO_REUSEPORT, 1);
    set_int(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, 1);
    if defer_accept > 0 {
        set_int(fd, libc::IPPROTO_TCP, libc::TCP_DEFER_ACCEPT, defer_accept);
    }
    let r = unsafe {
        match host {
            IpAddr::V4(a) => {
                let mut sa: libc::sockaddr_in = std::mem::zeroed();
                sa.sin_family = libc::AF_INET as libc::sa_family_t;
                sa.sin_port = port.to_be();
                sa.sin_addr = libc::in_addr { s_addr: u32::from_ne_bytes(a.octets()) };
                libc::bind(fd, &sa as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_in>() as u32)
            }
            IpAddr::V6(a) => {
                let mut sa: libc::sockaddr_in6 = std::mem::zeroed();
                sa.sin6_family = libc::AF_INET6 as libc::sa_family_t;
                sa.sin6_port = port.to_be();
                sa.sin6_addr = libc::in6_addr { s6_addr: a.octets() };
                libc::bind(fd, &sa as *const _ as *const libc::sockaddr, std::mem::size_of::<libc::sockaddr_in6>() as u32)
            }
        }
    };
    if r != 0 {
        let e = errno();
        close(fd);
        return Err(format!("bind {host}:{port}: errno {e}"));
    }
    if unsafe { libc::listen(fd, backlog) } != 0 {
        let e = errno();
        close(fd);
        return Err(format!("listen {host}:{port}: errno {e}"));
    }
    clear_quickack(fd);
    Ok(fd)
}

#[inline]
pub fn accept(fd: i32) -> i32 {
    unsafe { libc::accept4(fd, std::ptr::null_mut(), std::ptr::null_mut(), libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC) }
}

#[inline]
pub fn read(fd: i32, buf: &mut [u8]) -> isize {
    unsafe { libc::read(fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) }
}

#[inline]
pub fn send(fd: i32, buf: &[u8]) -> isize {
    unsafe { libc::send(fd, buf.as_ptr() as *const libc::c_void, buf.len(), libc::MSG_NOSIGNAL | libc::MSG_DONTWAIT) }
}

#[inline]
pub fn close(fd: i32) {
    unsafe { libc::close(fd) };
}

pub fn epoll_create() -> i32 {
    let ep = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
    assert!(ep >= 0, "epoll_create1: errno {}", errno());
    ep
}

#[inline]
pub fn epoll_ctl(ep: i32, op: i32, fd: i32, events: u32, data: u64) -> i32 {
    let mut ev = libc::epoll_event { events, u64: data };
    unsafe { libc::epoll_ctl(ep, op, fd, &mut ev) }
}

pub const CTL_ADD: i32 = libc::EPOLL_CTL_ADD;
pub const CTL_MOD: i32 = libc::EPOLL_CTL_MOD;
pub const CTL_DEL: i32 = libc::EPOLL_CTL_DEL;

const SYS_EPOLL_PWAIT2: libc::c_long = 441;

pub struct Waiter {
    pwait2: bool,
}

impl Waiter {
    pub fn new() -> Self {
        Waiter { pwait2: true }
    }

    #[inline]
    pub fn wait(&mut self, ep: i32, events: &mut [libc::epoll_event], nanos: i64) -> i32 {
        if self.pwait2 {
            let ts = libc::timespec { tv_sec: (nanos.max(0) / 1_000_000_000) as _, tv_nsec: (nanos.max(0) % 1_000_000_000) as _ };
            let tp: *const libc::timespec = if nanos < 0 { std::ptr::null() } else { &ts };
            let n = unsafe {
                libc::syscall(SYS_EPOLL_PWAIT2, ep, events.as_mut_ptr(), events.len() as i32, tp, std::ptr::null::<u8>(), 0usize)
            } as i32;
            if n >= 0 {
                return n;
            }
            let e = errno();
            if e != libc::ENOSYS && e != libc::EPERM {
                return n;
            }
            self.pwait2 = false;
            eprintln!("epoll_pwait2: errno {e}; waiting in whole milliseconds instead");
        }
        let ms = if nanos < 0 { -1 } else { ((nanos + 999_999) / 1_000_000).min(i32::MAX as i64) as i32 };
        unsafe { libc::epoll_wait(ep, events.as_mut_ptr(), events.len() as i32, ms) }
    }
}

pub fn raise_nofile() -> usize {
    unsafe {
        let mut rl: libc::rlimit = std::mem::zeroed();
        if libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl) == 0 {
            if rl.rlim_cur < rl.rlim_max {
                rl.rlim_cur = rl.rlim_max;
                libc::setrlimit(libc::RLIMIT_NOFILE, &rl);
                libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl);
            }
            return rl.rlim_cur as usize;
        }
    }
    1 << 20
}

pub fn map_zeroed(bytes: usize) -> *mut u8 {
    const HUGE: usize = 2 << 20;
    let len = bytes.div_ceil(HUGE) * HUGE + HUGE;
    let p = unsafe {
        libc::mmap(
            std::ptr::null_mut(),
            len,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_NORESERVE,
            -1,
            0,
        )
    };
    assert!(p != libc::MAP_FAILED, "mmap {bytes} bytes: errno {}", errno());
    let aligned = (p as usize).div_ceil(HUGE) * HUGE;
    unsafe { libc::madvise(aligned as *mut libc::c_void, len - (aligned - p as usize), libc::MADV_HUGEPAGE) };
    aligned as *mut u8
}

pub fn allowed_cpus() -> Vec<usize> {
    unsafe {
        let mut set: libc::cpu_set_t = std::mem::zeroed();
        if libc::sched_getaffinity(0, std::mem::size_of::<libc::cpu_set_t>(), &mut set) != 0 {
            return Vec::new();
        }
        (0..libc::CPU_SETSIZE as usize).filter(|&c| libc::CPU_ISSET(c, &set)).collect()
    }
}

pub const TCP_THIN_LINEAR_TIMEOUTS: i32 = 16;
const TCP_RTO_MIN_US: i32 = 45;

pub fn set_thin(fd: i32) {
    set_int(fd, libc::IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, 1);
}

pub fn tune_listener(fd: i32, thin: bool, rto_min_us: i32, cpu: i32, log: bool) {
    if thin {
        set_thin(fd);
    }
    if rto_min_us > 0 {
        let r = set_int(fd, libc::IPPROTO_TCP, TCP_RTO_MIN_US, rto_min_us);
        if log {
            eprintln!("TCP_RTO_MIN_US {}", if r == 0 { format!("{rto_min_us} us") } else { format!("not available (errno {})", errno()) });
        }
    }
    if cpu >= 0 {
        set_int(fd, libc::SOL_SOCKET, libc::SO_INCOMING_CPU, cpu);
    }
}

pub fn tune_thread(sched_idle: bool, timer_slack_ns: u64) {
    unsafe {
        if sched_idle {
            let p: libc::sched_param = std::mem::zeroed();
            if libc::sched_setscheduler(0, libc::SCHED_IDLE, &p) != 0 {
                eprintln!("SCHED_IDLE: errno {}", errno());
            }
        }
        if timer_slack_ns > 0 {
            libc::prctl(libc::PR_SET_TIMERSLACK, timer_slack_ns as libc::c_ulong, 0, 0, 0);
        }
    }
}

pub fn random_bytes(buf: &mut [u8]) {
    let mut got = 0;
    while got < buf.len() {
        let r = unsafe { libc::getrandom(buf[got..].as_mut_ptr() as *mut libc::c_void, buf.len() - got, 0) };
        if r > 0 {
            got += r as usize;
        } else if errno() != EINTR {
            panic!("getrandom: errno {}", errno());
        }
    }
}
