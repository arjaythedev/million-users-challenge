#![allow(dead_code)]

use std::ffi::c_void;

pub const AF_UNIX: i32 = 1;
pub const AF_INET: i32 = 2;
pub const SOCK_STREAM: i32 = 1;
pub const SOCK_NONBLOCK: i32 = 2048;
pub const SOCK_CLOEXEC: i32 = 524288;

pub const SOL_SOCKET: i32 = 1;
pub const SO_REUSEADDR: i32 = 2;
pub const SO_ERROR: i32 = 4;
pub const SO_SNDBUF: i32 = 7;
pub const SO_RCVBUF: i32 = 8;
pub const SO_KEEPALIVE: i32 = 9;
pub const SO_REUSEPORT: i32 = 15;

pub const IPPROTO_TCP: i32 = 6;
pub const TCP_NODELAY: i32 = 1;
pub const TCP_KEEPIDLE: i32 = 4;
pub const TCP_KEEPINTVL: i32 = 5;
pub const TCP_KEEPCNT: i32 = 6;
pub const TCP_DEFER_ACCEPT: i32 = 9;
pub const TCP_QUICKACK: i32 = 12;
pub const TCP_CONGESTION: i32 = 13;
pub const TCP_THIN_LINEAR_TIMEOUTS: i32 = 16;
pub const TCP_USER_TIMEOUT: i32 = 18;

pub const MSG_NOSIGNAL: i32 = 0x4000;

pub const EPOLL_CLOEXEC: i32 = 524288;
pub const EPOLL_CTL_ADD: i32 = 1;
pub const EPOLL_CTL_DEL: i32 = 2;
pub const EPOLL_CTL_MOD: i32 = 3;

pub const EPOLLIN: u32 = 0x001;
pub const EPOLLOUT: u32 = 0x004;
pub const EPOLLERR: u32 = 0x008;
pub const EPOLLHUP: u32 = 0x010;
pub const EPOLLRDHUP: u32 = 0x2000;

pub const POLLIN: i16 = 0x0001;
pub const POLLOUT: i16 = 0x0004;

pub const EINTR: i32 = 4;
pub const EAGAIN: i32 = 11;
pub const EMFILE: i32 = 24;
pub const ENFILE: i32 = 23;
pub const ENOSYS: i32 = 38;
pub const EPIPE: i32 = 32;
pub const ECONNABORTED: i32 = 103;
pub const ENOBUFS: i32 = 105;
pub const EINPROGRESS: i32 = 115;
pub const ECANCELED: i32 = 125;

#[cfg(target_arch = "x86_64")]
pub const SYS_EPOLL_PWAIT2: i64 = 441;

pub const CLOCK_REALTIME: i32 = 0;
pub const CLOCK_MONOTONIC: i32 = 1;
pub const CLOCK_REALTIME_COARSE: i32 = 5;
pub const CLOCK_MONOTONIC_COARSE: i32 = 6;

pub const RLIMIT_NOFILE: i32 = 7;
pub const RLIMIT_MEMLOCK: i32 = 8;

pub const SIGINT: i32 = 2;
pub const SIGTERM: i32 = 15;
pub const SIGPIPE: i32 = 13;
pub const SIG_DFL: usize = 0;
pub const SIG_IGN: usize = 1;

pub const PR_SET_PDEATHSIG: i32 = 1;
pub const PR_SET_THP_DISABLE: i32 = 41;
pub const CLONE_FILES: i32 = 0x0000_0400;
pub const M_ARENA_MAX: i32 = -8;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SockAddrIn {
    pub sin_family: u16,
    pub sin_port: u16,
    pub sin_addr: u32,
    pub sin_zero: [u8; 8],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AddrInfo {
    pub ai_flags: i32,
    pub ai_family: i32,
    pub ai_socktype: i32,
    pub ai_protocol: i32,
    pub ai_addrlen: u32,
    pub ai_addr: *mut SockAddrIn,
    pub ai_canonname: *mut u8,
    pub ai_next: *mut AddrInfo,
}

#[repr(C, packed)]
#[derive(Clone, Copy, Default)]
pub struct EpollEvent {
    pub events: u32,
    pub data: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PollFd {
    pub fd: i32,
    pub events: i16,
    pub revents: i16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Timespec {
    pub tv_sec: i64,
    pub tv_nsec: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct RLimit {
    pub rlim_cur: u64,
    pub rlim_max: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct CpuSet {
    pub bits: [u64; 16],
}

#[repr(C)]
pub struct IgzipCtx {
    _priv: [u8; 0],
}

extern "C" {
    pub fn __errno_location() -> *mut i32;
    pub fn socket(domain: i32, ty: i32, protocol: i32) -> i32;
    pub fn socketpair(domain: i32, ty: i32, protocol: i32, sv: *mut i32) -> i32;
    pub fn bind(fd: i32, addr: *const SockAddrIn, len: u32) -> i32;
    pub fn listen(fd: i32, backlog: i32) -> i32;
    pub fn accept4(fd: i32, addr: *mut SockAddrIn, len: *mut u32, flags: i32) -> i32;
    pub fn connect(fd: i32, addr: *const SockAddrIn, len: u32) -> i32;
    pub fn setsockopt(fd: i32, level: i32, optname: i32, optval: *const c_void, optlen: u32) -> i32;
    pub fn getsockopt(fd: i32, level: i32, optname: i32, optval: *mut c_void, optlen: *mut u32) -> i32;
    pub fn fcntl(fd: i32, cmd: i32, ...) -> i32;
    pub fn read(fd: i32, buf: *mut c_void, count: usize) -> isize;
    pub fn write(fd: i32, buf: *const c_void, count: usize) -> isize;
    pub fn send(fd: i32, buf: *const c_void, len: usize, flags: i32) -> isize;
    pub fn recv(fd: i32, buf: *mut c_void, len: usize, flags: i32) -> isize;
    pub fn close(fd: i32) -> i32;
    pub fn poll(fds: *mut PollFd, nfds: u64, timeout: i32) -> i32;

    pub fn epoll_create1(flags: i32) -> i32;
    pub fn epoll_ctl(epfd: i32, op: i32, fd: i32, event: *mut EpollEvent) -> i32;
    pub fn epoll_wait(epfd: i32, events: *mut EpollEvent, maxevents: i32, timeout: i32) -> i32;
    pub fn syscall(num: i64, ...) -> i64;

    pub fn getaddrinfo(node: *const u8, service: *const u8, hints: *const AddrInfo, res: *mut *mut AddrInfo) -> i32;
    pub fn freeaddrinfo(res: *mut AddrInfo);

    pub fn clock_gettime(clk_id: i32, tp: *mut Timespec) -> i32;
    pub fn nanosleep(req: *const Timespec, rem: *mut Timespec) -> i32;

    pub fn getrlimit(resource: i32, rlim: *mut RLimit) -> i32;
    pub fn setrlimit(resource: i32, rlim: *const RLimit) -> i32;

    pub fn sched_getaffinity(pid: i32, cpusetsize: usize, mask: *mut CpuSet) -> i32;

    pub fn fork() -> i32;
    pub fn waitpid(pid: i32, status: *mut i32, options: i32) -> i32;
    pub fn kill(pid: i32, sig: i32) -> i32;
    pub fn signal(signum: i32, handler: usize) -> usize;
    pub fn _exit(status: i32) -> !;
    pub fn prctl(option: i32, arg2: u64, arg3: u64, arg4: u64, arg5: u64) -> i32;
    pub fn unshare(flags: i32) -> i32;
    pub fn getrandom(buf: *mut c_void, buflen: usize, flags: u32) -> isize;

    pub fn calloc(nmemb: usize, size: usize) -> *mut c_void;
    pub fn free(ptr: *mut c_void);
    pub fn mallopt(param: i32, value: i32) -> i32;

    pub fn igzip_ctx_new(level: i32, train_huff: i32) -> *mut IgzipCtx;
    pub fn igzip_ctx_free(ctx: *mut IgzipCtx);
    pub fn igzip_compress_gzip(
        ctx: *mut IgzipCtx,
        src: *const u8,
        src_len: usize,
        dst: *mut u8,
        dst_cap: usize,
        now_ms: u64,
    ) -> usize;
}

#[inline(always)]
pub fn errno() -> i32 {
    unsafe { *__errno_location() }
}

#[inline(always)]
pub fn monotonic_ns() -> u64 {
    let mut ts = Timespec::default();
    unsafe {
        clock_gettime(CLOCK_MONOTONIC, &mut ts);
    }
    (ts.tv_sec as u64) * 1_000_000_000 + (ts.tv_nsec as u64)
}

#[inline(always)]
pub fn realtime_sec() -> i64 {
    let mut ts = Timespec::default();
    unsafe {
        clock_gettime(CLOCK_REALTIME_COARSE, &mut ts);
    }
    ts.tv_sec
}

pub fn available_cpus() -> usize {
    let mut set = CpuSet { bits: [0; 16] };
    let rc = unsafe { sched_getaffinity(0, std::mem::size_of::<CpuSet>(), &mut set) };
    if rc == 0 {
        let count: u32 = set.bits.iter().map(|w| w.count_ones()).sum();
        if count > 0 {
            return count as usize;
        }
    }
    std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1)
}

pub fn set_sockopt_int(fd: i32, level: i32, opt: i32, val: i32) {
    unsafe {
        setsockopt(
            fd,
            level,
            opt,
            &val as *const i32 as *const c_void,
            std::mem::size_of::<i32>() as u32,
        );
    }
}

#[inline(always)]
pub fn epoll_pwait2_ns(
    epfd: i32,
    events: *mut EpollEvent,
    maxevents: i32,
    timeout_ns: u64,
) -> i32 {
    #[cfg(target_arch = "x86_64")]
    {
        let ts = Timespec {
            tv_sec: (timeout_ns / 1_000_000_000) as i64,
            tv_nsec: (timeout_ns % 1_000_000_000) as i64,
        };
        let rc = unsafe {
            syscall(
                SYS_EPOLL_PWAIT2,
                epfd,
                events,
                maxevents,
                &ts as *const Timespec,
                std::ptr::null::<c_void>(),
                8usize,
            )
        };
        if rc >= 0 || errno() != ENOSYS {
            return rc as i32;
        }
    }
    let ms = ((timeout_ns + 999_999) / 1_000_000) as i32;
    unsafe { epoll_wait(epfd, events, maxevents, ms) }
}
