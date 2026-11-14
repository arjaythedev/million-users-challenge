//! The libc calls and Linux constants the server uses, declared by hand so the crate has no dependencies.
#![allow(dead_code)]

use core::ffi::{c_int, c_long, c_void};

pub const AF_INET: c_int = 2;
pub const AF_INET6: c_int = 10;
pub const SOCK_STREAM: c_int = 1;
pub const SOCK_NONBLOCK: c_int = 0o4000;
pub const SOCK_CLOEXEC: c_int = 0o2000000;
pub const SOL_SOCKET: c_int = 1;
pub const SO_REUSEADDR: c_int = 2;
pub const SO_REUSEPORT: c_int = 15;
pub const IPPROTO_TCP: c_int = 6;
pub const TCP_NODELAY: c_int = 1;
pub const TCP_DEFER_ACCEPT: c_int = 9;
pub const TCP_QUICKACK: c_int = 12;
pub const TCP_THIN_LINEAR_TIMEOUTS: c_int = 16;
pub const MSG_DONTWAIT: c_int = 0x40;
pub const MSG_WAITALL: u32 = 0x100;
pub const MSG_NOSIGNAL: u32 = 0x4000;
pub const SHUT_RDWR: c_int = 2;

pub const EPOLLIN: u32 = 0x001;
pub const EPOLLOUT: u32 = 0x004;
pub const EPOLLERR: u32 = 0x008;
pub const EPOLLHUP: u32 = 0x010;
pub const EPOLLRDHUP: u32 = 0x2000;
pub const EPOLL_CTL_ADD: c_int = 1;
pub const EPOLL_CTL_MOD: c_int = 3;
pub const EPOLL_CLOEXEC: c_int = 0o2000000;

pub const CLOCK_REALTIME: c_int = 0;
pub const CLOCK_MONOTONIC: c_int = 1;
pub const RLIMIT_NOFILE: c_int = 7;
pub const SCHED_IDLE: c_int = 5;
pub const EINTR: c_int = 4;
pub const EAGAIN: c_int = 11;
pub const EINVAL: c_int = 22;
pub const EOPNOTSUPP: c_int = 95;
pub const ENOBUFS: c_int = 105;
pub const F_GETFL: c_int = 3;
pub const F_SETFL: c_int = 4;
pub const O_NONBLOCK: c_int = 0o4000;
pub const PROT_READ: c_int = 1;
pub const PROT_WRITE: c_int = 2;
pub const MAP_SHARED: c_int = 0x01;
pub const MAP_PRIVATE: c_int = 0x02;
pub const MAP_ANONYMOUS: c_int = 0x20;
pub const MAP_NORESERVE: c_int = 0x4000;
pub const MAP_POPULATE: c_int = 0x8000;

// epoll_event is packed on x86-64 only.
#[cfg_attr(target_arch = "x86_64", repr(C, packed))]
#[cfg_attr(not(target_arch = "x86_64"), repr(C))]
#[derive(Clone, Copy)]
pub struct EpollEvent {
    pub events: u32,
    pub data: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Timespec {
    pub sec: i64,
    pub nsec: i64,
}

#[repr(C)]
pub struct Rlimit {
    pub cur: u64,
    pub max: u64,
}

#[repr(C)]
pub struct IoVec {
    pub base: *const c_void,
    pub len: usize,
}

#[repr(C)]
pub struct SockAddrIn {
    pub family: u16,
    pub port: u16,
    pub addr: [u8; 4],
    pub zero: [u8; 8],
}

#[repr(C)]
pub struct SockAddrIn6 {
    pub family: u16,
    pub port: u16,
    pub flowinfo: u32,
    pub addr: [u8; 16],
    pub scope_id: u32,
}

extern "C" {
    pub fn socket(domain: c_int, ty: c_int, protocol: c_int) -> c_int;
    pub fn setsockopt(fd: c_int, level: c_int, name: c_int, val: *const c_void, len: u32) -> c_int;
    pub fn bind(fd: c_int, addr: *const c_void, len: u32) -> c_int;
    pub fn listen(fd: c_int, backlog: c_int) -> c_int;
    pub fn accept4(fd: c_int, addr: *mut c_void, len: *mut u32, flags: c_int) -> c_int;
    pub fn recv(fd: c_int, buf: *mut c_void, len: usize, flags: c_int) -> isize;
    pub fn write(fd: c_int, buf: *const c_void, len: usize) -> isize;
    pub fn writev(fd: c_int, iov: *const IoVec, cnt: c_int) -> isize;
    pub fn close(fd: c_int) -> c_int;
    pub fn shutdown(fd: c_int, how: c_int) -> c_int;
    pub fn syscall(number: c_long, ...) -> c_long;
    pub fn fcntl(fd: c_int, cmd: c_int, ...) -> c_int;
    pub fn epoll_create1(flags: c_int) -> c_int;
    pub fn epoll_ctl(epfd: c_int, op: c_int, fd: c_int, ev: *mut EpollEvent) -> c_int;
    pub fn epoll_wait(epfd: c_int, evs: *mut EpollEvent, max: c_int, timeout_ms: c_int) -> c_int;
    pub fn clock_gettime(clk: c_int, ts: *mut Timespec) -> c_int;
    pub fn nanosleep(req: *const Timespec, rem: *mut Timespec) -> c_int;
    pub fn getrlimit(res: c_int, rl: *mut Rlimit) -> c_int;
    pub fn setrlimit(res: c_int, rl: *const Rlimit) -> c_int;
    pub fn sched_getaffinity(pid: c_int, size: usize, mask: *mut u8) -> c_int;
    pub fn sched_setscheduler(pid: c_int, policy: c_int, param: *const c_int) -> c_int;
    pub fn mmap(addr: *mut c_void, len: usize, prot: c_int, flags: c_int, fd: c_int, off: i64) -> *mut c_void;
    pub fn getrandom(buf: *mut c_void, len: usize, flags: u32) -> isize;
    fn __errno_location() -> *mut c_int;
}

pub fn errno() -> c_int {
    unsafe { *__errno_location() }
}

pub fn set_opt(fd: c_int, level: c_int, name: c_int, val: c_int) -> bool {
    unsafe { setsockopt(fd, level, name, &val as *const c_int as *const c_void, 4) == 0 }
}

pub fn mono_ns() -> u64 {
    let mut ts = Timespec::default();
    unsafe { clock_gettime(CLOCK_MONOTONIC, &mut ts) };
    ts.sec as u64 * 1_000_000_000 + ts.nsec as u64
}

pub fn wall_secs() -> i64 {
    let mut ts = Timespec::default();
    unsafe { clock_gettime(CLOCK_REALTIME, &mut ts) };
    ts.sec
}

pub fn sleep_ns(ns: u64) {
    let ts = Timespec { sec: (ns / 1_000_000_000) as i64, nsec: (ns % 1_000_000_000) as i64 };
    unsafe { nanosleep(&ts, core::ptr::null_mut()) };
}

pub fn set_nonblocking(fd: c_int) {
    unsafe {
        let fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

/// CPUs this process may run on. The 8- and 4-core runs shrink this set, so it decides the loop count.
pub fn allowed_cpus() -> usize {
    let mut mask = [0u8; 1024];
    let n = unsafe { sched_getaffinity(0, mask.len(), mask.as_mut_ptr()) };
    if n != 0 {
        return 1;
    }
    (mask.iter().map(|b| b.count_ones() as usize).sum::<usize>()).max(1)
}

/// Asks for the cache line at `p` ahead of its use. Nothing happens if the address is not mapped.
#[inline(always)]
pub fn prefetch<T>(p: *const T) {
    #[cfg(target_arch = "x86_64")]
    unsafe {
        core::arch::x86_64::_mm_prefetch::<{ core::arch::x86_64::_MM_HINT_T0 }>(p.cast())
    };
    #[cfg(not(target_arch = "x86_64"))]
    let _ = p;
}
