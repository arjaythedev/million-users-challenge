use std::collections::VecDeque;
use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};

pub static WORKER_STOP: AtomicBool = AtomicBool::new(false);

use crate::crypto::HmacSha256Key;
use crate::http::{
    parse_http_request, HttpScratch, ParsedAction, ReplyPayload, RESP_400_BAD_HTTP,
    RESP_500_INTERNAL,
};
use crate::pg::{
    DbConfig, InlineBytes, PgLane, PgPhase, ReadOp, ReadReq, WriteOp, WriteReq,
    RESP_503_DB_UNREACHABLE, RESP_503_DB_UNREACHABLE_CLOSE,
};
use crate::sys::{
    accept4, bind, calloc, close, epoll_create1, epoll_ctl, epoll_pwait2_ns, epoll_wait, errno,
    igzip_ctx_new, listen, monotonic_ns, nanosleep, realtime_sec, recv, send, set_sockopt_int,
    setsockopt, socket, EpollEvent, IgzipCtx, SockAddrIn, Timespec, AF_INET, EAGAIN, ECANCELED,
    ECONNABORTED, EINTR, EMFILE, ENFILE, ENOBUFS, EPOLLERR, EPOLLHUP, EPOLLIN, EPOLLOUT,
    EPOLLRDHUP, EPOLL_CLOEXEC, EPOLL_CTL_ADD, EPOLL_CTL_DEL, EPOLL_CTL_MOD, IPPROTO_TCP,
    MSG_NOSIGNAL, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR, SO_REUSEPORT,
    TCP_CONGESTION, TCP_DEFER_ACCEPT, TCP_NODELAY, TCP_QUICKACK, TCP_THIN_LINEAR_TIMEOUTS,
    TCP_USER_TIMEOUT,
};
use crate::uring::{
    IoUringCqe, Uring, IORING_CQE_BUFFER_SHIFT, IORING_CQE_F_BUFFER, IORING_CQE_F_MORE,
};

const TAG_LISTEN_0: u64 = u64::MAX;
const TAG_LISTEN_1: u64 = u64::MAX - 1;
const TAG_PG_BASE: u64 = 1u64 << 62;

const UTAG_SHIFT: u32 = 60;
const UTAG_MASK: u64 = 0xFu64 << UTAG_SHIFT;
const UTAG_LISTEN_0: u64 = 1u64 << UTAG_SHIFT;
const UTAG_LISTEN_1: u64 = 2u64 << UTAG_SHIFT;
const UTAG_CLIENT_POLL: u64 = 3u64 << UTAG_SHIFT;
const UTAG_CLIENT_RECV: u64 = 4u64 << UTAG_SHIFT;
const UTAG_CLIENT_SEND: u64 = 5u64 << UTAG_SHIFT;
const UTAG_PG_IN: u64 = 6u64 << UTAG_SHIFT;
const UTAG_PG_OUT: u64 = 7u64 << UTAG_SHIFT;
const UTAG_IGNORE: u64 = 8u64 << UTAG_SHIFT;
const UTAG_PG_RECV: u64 = 9u64 << UTAG_SHIFT;
const UTAG_PG_SEND: u64 = 10u64 << UTAG_SHIFT;
const UTAG_TICK: u64 = 11u64 << UTAG_SHIFT;

static TICK_TS_1S: Timespec = Timespec {
    tv_sec: 1,
    tv_nsec: 0,
};

const PG_ARM_POLL_IN: u32 = 1;
const PG_ARM_POLL_OUT: u32 = 2;
const PG_ARM_URING_RX: u32 = 4;
const PG_ARM_URING_TX: u32 = 8;

const STATE_CLOSED: u8 = 0;
const STATE_IDLE: u8 = 1;
const STATE_READING_PARTIAL: u8 = 2;
const STATE_WAITING_PG: u8 = 3;
const STATE_WRITING_PARTIAL: u8 = 4;

const FLAG_CLOSE_AFTER: u8 = 1;
const FLAG_EPOLLOUT: u8 = 2;
const FLAG_RECV_INFLIGHT: u8 = 4;
const FLAG_PENDING_RX: u8 = 8;
const FLAG_POLL_ARMED: u8 = 16;

const NO_AUX: u32 = 0;
const IDLE_TIMEOUT_SEC: u32 = 90;
const PG_WATCHDOG_NS: u64 = 10_000_000_000;

#[repr(C)]
#[derive(Clone, Copy)]
struct ConnSlot {
    state: u8,
    flags: u8,
    gen: u16,
    last_active_sec: u32,
    aux_idx: u32,
    _pad: u32,
}

struct PartialBuf {
    data: Vec<u8>,
    pos: usize,
}

struct PartialPool {
    items: Vec<PartialBuf>,
    free: Vec<u32>,
}

impl PartialPool {
    fn new() -> Self {
        let mut items = Vec::with_capacity(256);
        items.push(PartialBuf {
            data: Vec::new(),
            pos: 0,
        });
        Self {
            items,
            free: Vec::with_capacity(256),
        }
    }

    fn alloc(&mut self, bytes: &[u8]) -> u32 {
        if let Some(idx) = self.free.pop() {
            let b = &mut self.items[idx as usize];
            b.data.clear();
            b.data.extend_from_slice(bytes);
            b.pos = 0;
            idx
        } else {
            let idx = self.items.len() as u32;
            let mut v = Vec::with_capacity(bytes.len().max(2048));
            v.extend_from_slice(bytes);
            self.items.push(PartialBuf { data: v, pos: 0 });
            idx
        }
    }

    fn release(&mut self, idx: u32) {
        if idx == NO_AUX {
            return;
        }
        let b = &mut self.items[idx as usize];
        b.data.clear();
        if b.data.capacity() > 16384 {
            b.data.shrink_to(4096);
        }
        b.pos = 0;
        self.free.push(idx);
    }
}

struct TxItem {
    data: Vec<u8>,
    static_slice: Option<&'static [u8]>,
    pos: usize,
    end: usize,
}

struct TxPool {
    items: Vec<TxItem>,
    free: Vec<u32>,
}

impl TxPool {
    fn new() -> Self {
        let mut items = Vec::with_capacity(512);
        items.push(TxItem {
            data: Vec::new(),
            static_slice: None,
            pos: 0,
            end: 0,
        });
        Self {
            items,
            free: Vec::with_capacity(512),
        }
    }

    #[inline(always)]
    fn lease_static(&mut self, s: &'static [u8]) -> (u32, *const u8, usize) {
        let idx = if let Some(i) = self.free.pop() {
            let it = &mut self.items[i as usize];
            it.static_slice = Some(s);
            it.pos = 0;
            it.end = s.len();
            i
        } else {
            let i = self.items.len() as u32;
            self.items.push(TxItem {
                data: Vec::new(),
                static_slice: Some(s),
                pos: 0,
                end: s.len(),
            });
            i
        };
        (idx, s.as_ptr(), s.len())
    }

    #[inline(always)]
    fn lease_swapped(
        &mut self,
        src: &mut Vec<u8>,
        start: usize,
        end: usize,
    ) -> (u32, *const u8, usize) {
        let len = end - start;
        let idx = if let Some(i) = self.free.pop() {
            let it = &mut self.items[i as usize];
            it.static_slice = None;
            if it.data.capacity() < 4096 {
                it.data.reserve(6144);
            }
            std::mem::swap(&mut it.data, src);
            it.pos = start;
            it.end = end;
            i
        } else {
            let i = self.items.len() as u32;
            let mut replacement = Vec::with_capacity(6144);
            std::mem::swap(&mut replacement, src);
            self.items.push(TxItem {
                data: replacement,
                static_slice: None,
                pos: start,
                end,
            });
            i
        };
        let ptr = unsafe { self.items[idx as usize].data.as_ptr().add(start) };
        (idx, ptr, len)
    }

    #[inline(always)]
    fn advance(&mut self, idx: u32, sent: usize) -> (*const u8, usize) {
        let it = &mut self.items[idx as usize];
        it.pos += sent;
        if it.pos < it.end {
            let rem = it.end - it.pos;
            let ptr = match it.static_slice {
                Some(s) => unsafe { s.as_ptr().add(it.pos) },
                None => unsafe { it.data.as_ptr().add(it.pos) },
            };
            (ptr, rem)
        } else {
            (std::ptr::null(), 0)
        }
    }

    #[inline(always)]
    fn remaining(&self, idx: u32) -> (*const u8, usize) {
        let it = &self.items[idx as usize];
        let rem = it.end.saturating_sub(it.pos);
        let ptr = match it.static_slice {
            Some(s) => unsafe { s.as_ptr().add(it.pos) },
            None => unsafe { it.data.as_ptr().add(it.pos) },
        };
        (ptr, rem)
    }

    #[inline(always)]
    fn release(&mut self, idx: u32) {
        if idx == NO_AUX {
            return;
        }
        let it = &mut self.items[idx as usize];
        it.static_slice = None;
        if it.data.capacity() > 65536 {
            it.data.clear();
            it.data.shrink_to(8192);
        }
        it.pos = 0;
        it.end = 0;
        self.free.push(idx);
    }
}

#[derive(Clone)]
pub struct WorkerConfig {
    pub worker_id: usize,
    pub num_workers: usize,
    pub listen_addr_0: SockAddrIn,
    pub listen_addr_1: Option<SockAddrIn>,
    pub db: DbConfig,
    pub hmac_key: HmacSha256Key,
    pub read_lanes: usize,
    pub write_lanes: usize,
    pub max_read_batch: usize,
    pub max_write_batch: usize,
    pub min_read_batch: usize,
    pub min_write_batch: usize,
    pub horizon_us: u64,
    pub horizon_off: bool,
    pub lifo_age_ms: u32,
    pub gz_level: i32,
    pub gz_train_huff: bool,
    pub gz_min_bytes: usize,
    pub piggyback_ack: bool,
    pub per_send_quickack: bool,
    pub try_bbr: bool,
    pub use_uring: bool,
    pub uring_pbuf_rx: bool,
    pub uring_pg_io: bool,
    pub direct_fd: bool,
    pub batch_min: u32,
    pub batch_wait_us: u32,
    pub pace_max_us: u32,
    pub max_fds: usize,
    pub boot_ns: u64,
}

fn open_listener(addr: &SockAddrIn, piggyback_ack: bool, try_bbr: bool) -> Result<i32, String> {
    let fd = unsafe { socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP) };
    if fd < 0 {
        return Err(format!("socket() failed errno={}", errno()));
    }
    set_sockopt_int(fd, SOL_SOCKET, SO_REUSEADDR, 1);
    set_sockopt_int(fd, SOL_SOCKET, SO_REUSEPORT, 1);
    set_sockopt_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    set_sockopt_int(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, 1);
    set_sockopt_int(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, 1);
    set_sockopt_int(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, 30_000);
    if try_bbr {
        unsafe {
            setsockopt(
                fd,
                IPPROTO_TCP,
                TCP_CONGESTION,
                b"bbr".as_ptr() as *const c_void,
                3,
            );
        }
    }

    let rc = unsafe {
        bind(
            fd,
            addr as *const SockAddrIn,
            std::mem::size_of::<SockAddrIn>() as u32,
        )
    };
    if rc < 0 {
        let e = errno();
        unsafe {
            close(fd);
        }
        return Err(format!("bind() failed errno={}", e));
    }
    if unsafe { listen(fd, 65535) } < 0 {
        let e = errno();
        unsafe {
            close(fd);
        }
        return Err(format!("listen() failed errno={}", e));
    }
    if piggyback_ack {
        set_sockopt_int(fd, IPPROTO_TCP, TCP_QUICKACK, 0);
    }
    Ok(fd)
}

struct WorkerCore {
    cfg: WorkerConfig,
    epfd: i32,
    lfd0: i32,
    lfd1: i32,
    listeners_attached: bool,
    accept_paused_until_ns: u64,
    slots: *mut ConnSlot,
    max_fds: usize,
    active_conns: usize,
    highest_fd: usize,
    sweep_cursor: usize,
    last_sweep_sec: u32,

    partials: PartialPool,
    tx_pool: TxPool,
    uring: Option<Uring>,
    pbuf_multishot_rx: bool,
    uring_pg_enabled: bool,
    enobufs_retry: Vec<(i32, u16)>,
    pg_enobufs_retry: Vec<(usize, u16)>,
    pg_poll_gen: Vec<u16>,
    pg_poll_armed: Vec<u32>,
    http_scratch: HttpScratch,
    rx_tmp: Vec<u8>,
    igzip: *mut IgzipCtx,

    read_queue: VecDeque<ReadReq>,
    write_queue: VecDeque<WriteReq>,
    read_rr: usize,
    write_rr: usize,
}

struct WorkerState {
    core: WorkerCore,
    pg_lanes: Vec<PgLane>,
}

impl WorkerCore {
    #[inline(always)]
    fn enqueue_read(&mut self, req: ReadReq) {
        self.read_queue.push_back(req);
    }

    #[inline(always)]
    fn enqueue_write(&mut self, req: WriteReq) {
        self.write_queue.push_back(req);
    }

    fn pause_listeners(&mut self, now_ns: u64) {
        if self.listeners_attached {
            if let Some(ref mut ring) = self.uring {
                if self.lfd0 >= 0 {
                    ring.prep_async_cancel(UTAG_LISTEN_0, UTAG_IGNORE);
                }
                if self.lfd1 >= 0 {
                    ring.prep_async_cancel(UTAG_LISTEN_1, UTAG_IGNORE);
                }
            } else {
                let mut dummy = EpollEvent::default();
                if self.lfd0 >= 0 {
                    unsafe {
                        epoll_ctl(self.epfd, EPOLL_CTL_DEL, self.lfd0, &mut dummy);
                    }
                }
                if self.lfd1 >= 0 {
                    unsafe {
                        epoll_ctl(self.epfd, EPOLL_CTL_DEL, self.lfd1, &mut dummy);
                    }
                }
            }
            self.listeners_attached = false;
        }
        self.accept_paused_until_ns = now_ns + 100_000_000;
    }

    fn maybe_resume_listeners(&mut self, now_ns: u64) {
        if !self.listeners_attached && now_ns >= self.accept_paused_until_ns {
            if let Some(ref mut ring) = self.uring {
                let acc_flags = (SOCK_NONBLOCK | SOCK_CLOEXEC) as u32;
                if self.lfd0 >= 0 {
                    ring.prep_multishot_accept(self.lfd0, acc_flags, UTAG_LISTEN_0);
                }
                if self.lfd1 >= 0 {
                    ring.prep_multishot_accept(self.lfd1, acc_flags, UTAG_LISTEN_1);
                }
            } else {
                if self.lfd0 >= 0 {
                    let mut ev0 = EpollEvent {
                        events: EPOLLIN,
                        data: TAG_LISTEN_0,
                    };
                    unsafe {
                        epoll_ctl(self.epfd, EPOLL_CTL_ADD, self.lfd0, &mut ev0);
                    }
                }
                if self.lfd1 >= 0 {
                    let mut ev1 = EpollEvent {
                        events: EPOLLIN,
                        data: TAG_LISTEN_1,
                    };
                    unsafe {
                        epoll_ctl(self.epfd, EPOLL_CTL_ADD, self.lfd1, &mut ev1);
                    }
                }
            }
            self.listeners_attached = true;
        }
    }

    fn close_client(&mut self, fd: i32) {
        if fd < 0 || (fd as usize) >= self.max_fds {
            return;
        }
        let (aux, old_flags, old_gen) = unsafe {
            let s = &mut *self.slots.add(fd as usize);
            if s.state == STATE_CLOSED {
                if let Some(ref mut ring) = self.uring {
                    ring.prep_client_close(fd, UTAG_IGNORE);
                } else {
                    close(fd);
                }
                return;
            }
            let fl = s.flags;
            let gn = s.gen;
            s.state = STATE_CLOSED;
            s.flags = 0;
            s.gen = gn.wrapping_add(1);
            if s.gen == 0 {
                s.gen = 1;
            }
            let a = s.aux_idx;
            s.aux_idx = NO_AUX;
            (a, fl, gn)
        };
        self.active_conns = self.active_conns.saturating_sub(1);
        self.partials.release(aux);
        if let Some(ref mut ring) = self.uring {
            if ring.use_direct_fds {
                if (old_flags & FLAG_RECV_INFLIGHT) != 0 {
                    let recv_ud = UTAG_CLIENT_RECV | ((old_gen as u64) << 32) | (fd as u32 as u64);
                    ring.prep_direct_cancel_ud_and_close(fd, recv_ud, UTAG_IGNORE);
                } else if (old_flags & FLAG_POLL_ARMED) != 0 {
                    ring.prep_direct_cancel_and_close(fd, UTAG_IGNORE);
                } else {
                    ring.prep_client_close(fd, UTAG_IGNORE);
                }
            } else {
                if (old_flags & FLAG_POLL_ARMED) != 0 {
                    let poll_ud = UTAG_CLIENT_POLL | ((old_gen as u64) << 32) | (fd as u32 as u64);
                    ring.prep_poll_remove(poll_ud, UTAG_IGNORE);
                }
                if (old_flags & FLAG_RECV_INFLIGHT) != 0 {
                    let recv_ud = UTAG_CLIENT_RECV | ((old_gen as u64) << 32) | (fd as u32 as u64);
                    ring.prep_async_cancel(recv_ud, UTAG_IGNORE);
                }
                ring.prep_client_close(fd, UTAG_IGNORE);
            }
        } else {
            unsafe {
                close(fd);
            }
        }
    }

    fn sync_client_epoll(&mut self, fd: i32, want_out: bool) {
        if self.uring.is_some() {
            return;
        }
        let gen = unsafe {
            let s = &mut *self.slots.add(fd as usize);
            let cur = (s.flags & FLAG_EPOLLOUT) != 0;
            if cur == want_out {
                return;
            }
            if want_out {
                s.flags |= FLAG_EPOLLOUT;
            } else {
                s.flags &= !FLAG_EPOLLOUT;
            }
            s.gen
        };
        let mut ev = EpollEvent {
            events: EPOLLIN | EPOLLRDHUP | if want_out { EPOLLOUT } else { 0 },
            data: ((gen as u64) << 32) | (fd as u32 as u64),
        };
        unsafe {
            epoll_ctl(self.epfd, EPOLL_CTL_MOD, fd, &mut ev);
        }
    }

    fn sync_pg_lane(&mut self, idx: usize, lane: &mut PgLane) {
        if let Some(ref mut ring) = self.uring {
            let dead = lane.fd < 0 || lane.phase == PgPhase::Disconnected;
            let gen_changed = self.pg_poll_gen[idx] != lane.conn_gen;
            if dead || gen_changed || (!self.uring_pg_enabled && lane.epoll_events == 0 && self.pg_poll_armed[idx] != 0) {
                let armed = self.pg_poll_armed[idx];
                let old_g = self.pg_poll_gen[idx];
                if (armed & PG_ARM_POLL_IN) != 0 {
                    ring.prep_poll_remove(
                        UTAG_PG_IN | ((old_g as u64) << 16) | (idx as u64),
                        UTAG_IGNORE,
                    );
                }
                if (armed & PG_ARM_POLL_OUT) != 0 {
                    ring.prep_poll_remove(
                        UTAG_PG_OUT | ((old_g as u64) << 16) | (idx as u64),
                        UTAG_IGNORE,
                    );
                }
                if (armed & PG_ARM_URING_RX) != 0 {
                    ring.prep_async_cancel(
                        UTAG_PG_RECV | ((old_g as u64) << 16) | (idx as u64),
                        UTAG_IGNORE,
                    );
                }
                if (armed & PG_ARM_URING_TX) != 0 {
                    ring.prep_async_cancel(
                        UTAG_PG_SEND | ((old_g as u64) << 16) | (idx as u64),
                        UTAG_IGNORE,
                    );
                }
                self.pg_poll_armed[idx] = 0;
                self.pg_poll_gen[idx] = lane.conn_gen;
            }
            if dead {
                return;
            }
            let g = lane.conn_gen;
            if self.uring_pg_enabled {
                if lane.phase == PgPhase::Connecting {
                    if (self.pg_poll_armed[idx] & PG_ARM_POLL_OUT) == 0 {
                        ring.prep_poll_add(
                            lane.fd,
                            EPOLLOUT,
                            false,
                            UTAG_PG_OUT | ((g as u64) << 16) | (idx as u64),
                        );
                        self.pg_poll_armed[idx] |= PG_ARM_POLL_OUT;
                    }
                    return;
                }
                if !lane.uring_rx_armed {
                    ring.prep_pg_multishot_recv(
                        lane.fd,
                        UTAG_PG_RECV | ((g as u64) << 16) | (idx as u64),
                    );
                    lane.uring_rx_armed = true;
                    self.pg_poll_armed[idx] |= PG_ARM_URING_RX;
                }
                if lane.has_pending_tx()
                    && !lane.uring_tx_inflight
                    && (self.pg_poll_armed[idx] & PG_ARM_POLL_OUT) == 0
                {
                    let (ptr, len) = lane.pending_tx_slice();
                    if len > 0 {
                        ring.prep_pg_send(
                            lane.fd,
                            ptr,
                            len,
                            MSG_NOSIGNAL as u32,
                            UTAG_PG_SEND | ((g as u64) << 16) | (idx as u64),
                        );
                        lane.uring_tx_inflight = true;
                        self.pg_poll_armed[idx] |= PG_ARM_URING_TX;
                    }
                }
                return;
            }
            if (lane.epoll_events & EPOLLIN) == 0 {
                ring.prep_poll_add(
                    lane.fd,
                    EPOLLIN | EPOLLRDHUP,
                    true,
                    UTAG_PG_IN | ((g as u64) << 16) | (idx as u64),
                );
                lane.epoll_events |= EPOLLIN;
                self.pg_poll_armed[idx] |= PG_ARM_POLL_IN;
            }
            let want_out = lane.phase == PgPhase::Connecting || lane.has_pending_tx();
            if want_out && (lane.epoll_events & EPOLLOUT) == 0 {
                ring.prep_poll_add(
                    lane.fd,
                    EPOLLOUT,
                    false,
                    UTAG_PG_OUT | ((g as u64) << 16) | (idx as u64),
                );
                lane.epoll_events |= EPOLLOUT;
                self.pg_poll_armed[idx] |= PG_ARM_POLL_OUT;
            }
            return;
        }
        if lane.fd < 0 || lane.phase == PgPhase::Disconnected {
            return;
        }
        let want_out = lane.phase == PgPhase::Connecting || lane.has_pending_tx();
        let want = EPOLLIN | EPOLLRDHUP | if want_out { EPOLLOUT } else { 0 };
        if lane.epoll_events == want {
            return;
        }
        let op = if lane.epoll_events == 0 {
            EPOLL_CTL_ADD
        } else {
            EPOLL_CTL_MOD
        };
        let mut ev = EpollEvent {
            events: want,
            data: TAG_PG_BASE | (idx as u64),
        };
        if unsafe { epoll_ctl(self.epfd, op, lane.fd, &mut ev) } == 0 {
            lane.epoll_events = want;
        }
    }

    fn accept_loop(
        &mut self,
        lfd: i32,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) {
        for _ in 0..512 {
            let cfd = unsafe {
                accept4(
                    lfd,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                    SOCK_NONBLOCK | SOCK_CLOEXEC,
                )
            };
            if cfd < 0 {
                let e = errno();
                if e == EINTR || e == ECONNABORTED {
                    continue;
                }
                if e == EMFILE || e == ENFILE {
                    self.pause_listeners(now_ns);
                }
                break;
            }
            if (cfd as usize) >= self.max_fds {
                unsafe {
                    close(cfd);
                }
                self.pause_listeners(now_ns);
                break;
            }
            if (cfd as usize) > self.highest_fd {
                self.highest_fd = cfd as usize;
            }
            let (gen, was_closed) = unsafe {
                let s = &mut *self.slots.add(cfd as usize);
                let closed_prev = s.state == STATE_CLOSED;
                if s.aux_idx != NO_AUX {
                    let old = s.aux_idx;
                    s.aux_idx = NO_AUX;
                    self.partials.release(old);
                }
                let mut g = s.gen.wrapping_add(1);
                if g == 0 {
                    g = 1;
                }
                s.state = STATE_IDLE;
                s.flags = 0;
                s.gen = g;
                s.last_active_sec = now_sec_mono;
                s.aux_idx = NO_AUX;
                (g, closed_prev)
            };
            if was_closed {
                self.active_conns += 1;
            }

            let mut ev = EpollEvent {
                events: EPOLLIN | EPOLLRDHUP,
                data: ((gen as u64) << 32) | (cfd as u32 as u64),
            };
            if unsafe { epoll_ctl(self.epfd, EPOLL_CTL_ADD, cfd, &mut ev) } < 0 {
                self.close_client(cfd);
                continue;
            }
            self.handle_client_readable(cfd, gen, now_real, now_sec_mono, now_ms, now_ns);
        }
    }

    fn send_reply(
        &mut self,
        fd: i32,
        expected_gen: u16,
        close_after: bool,
        enqueued_ms: u32,
        reply: ReplyPayload<'_>,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) {
        if fd < 0 || (fd as usize) >= self.max_fds {
            return;
        }
        let valid = unsafe {
            let s = &*self.slots.add(fd as usize);
            s.state != STATE_CLOSED && s.gen == expected_gen
        };
        if !valid {
            return;
        }

        let slow_ack = self.cfg.per_send_quickack
            && self.cfg.piggyback_ack
            && !close_after
            && (self.active_conns >= 256
                || (enqueued_ms != 0 && now_ms.wrapping_sub(enqueued_ms) >= 25));

        if self.uring.is_some() {
            let (tx_idx, ptr, len) = match reply {
                ReplyPayload::Static(s) => self.tx_pool.lease_static(s),
                ReplyPayload::OwnedSlice { buf, start, end } => {
                    self.tx_pool.lease_swapped(buf, start, end)
                }
            };
            unsafe {
                let s = &mut *self.slots.add(fd as usize);
                s.state = STATE_WRITING_PARTIAL;
                if close_after {
                    s.flags |= FLAG_CLOSE_AFTER;
                }
                s.last_active_sec = now_sec_mono;
            }
            let ud = UTAG_CLIENT_SEND
                | (((tx_idx as u64) & 0xFFFFF) << 40)
                | ((expected_gen as u64) << 24)
                | ((fd as u32 as u64) & 0xFF_FFFF);
            if let Some(ref mut ring) = self.uring {
                if slow_ack {
                    ring.ensure_sqe_space(2);
                    ring.prep_linked_no_quickack(fd, UTAG_IGNORE);
                }
                ring.prep_client_send(fd, ptr, len, MSG_NOSIGNAL as u32, ud);
            }
            return;
        }

        if slow_ack {
            set_sockopt_int(fd, IPPROTO_TCP, TCP_QUICKACK, 0);
        }

        let payload: &[u8] = match &reply {
            ReplyPayload::Static(s) => s,
            ReplyPayload::OwnedSlice { buf, start, end } => &buf[*start..*end],
        };

        let mut sent = 0usize;
        while sent < payload.len() {
            let n = unsafe {
                send(
                    fd,
                    payload.as_ptr().add(sent) as *const c_void,
                    payload.len() - sent,
                    MSG_NOSIGNAL,
                )
            };
            if n > 0 {
                sent += n as usize;
            } else if n < 0 {
                let e = errno();
                if e == EINTR {
                    continue;
                }
                if e == EAGAIN {
                    break;
                }
                self.close_client(fd);
                return;
            } else {
                self.close_client(fd);
                return;
            }
        }

        if sent == payload.len() {
            let should_close = close_after || unsafe {
                ((*self.slots.add(fd as usize)).flags & FLAG_CLOSE_AFTER) != 0
            };
            if should_close {
                self.close_client(fd);
                return;
            }
            let leftover_aux = unsafe {
                let s = &mut *self.slots.add(fd as usize);
                s.last_active_sec = now_sec_mono;
                s.flags &= !FLAG_CLOSE_AFTER;
                let a = s.aux_idx;
                if a == NO_AUX {
                    s.state = STATE_IDLE;
                } else {
                    s.state = STATE_READING_PARTIAL;
                }
                a
            };
            self.sync_client_epoll(fd, false);
            if leftover_aux != NO_AUX {
                let now_real = realtime_sec();
                self.process_buffered_client(fd, now_real, now_sec_mono, now_ms, now_ns);
            }
        } else {
            let rem = &payload[sent..];
            let old_aux = unsafe { (*self.slots.add(fd as usize)).aux_idx };
            self.partials.release(old_aux);
            let new_aux = self.partials.alloc(rem);
            unsafe {
                let s = &mut *self.slots.add(fd as usize);
                s.state = STATE_WRITING_PARTIAL;
                if close_after {
                    s.flags |= FLAG_CLOSE_AFTER;
                }
                s.last_active_sec = now_sec_mono;
                s.aux_idx = new_aux;
            }
            self.sync_client_epoll(fd, true);
        }
    }

    fn flush_partial_client_write(&mut self, fd: i32, now_sec_mono: u32) {
        let (aux, close_after) = unsafe {
            let s = &*self.slots.add(fd as usize);
            if s.state != STATE_WRITING_PARTIAL || s.aux_idx == NO_AUX {
                return;
            }
            (s.aux_idx, (s.flags & FLAG_CLOSE_AFTER) != 0)
        };

        let done = {
            let pb = &mut self.partials.items[aux as usize];
            loop {
                if pb.pos >= pb.data.len() {
                    break true;
                }
                let rem = &pb.data[pb.pos..];
                let n = unsafe {
                    send(
                        fd,
                        rem.as_ptr() as *const c_void,
                        rem.len(),
                        MSG_NOSIGNAL,
                    )
                };
                if n > 0 {
                    pb.pos += n as usize;
                } else if n < 0 {
                    let e = errno();
                    if e == EINTR {
                        continue;
                    }
                    if e == EAGAIN {
                        break false;
                    }
                    self.close_client(fd);
                    return;
                } else {
                    self.close_client(fd);
                    return;
                }
            }
        };

        if done {
            if close_after {
                self.close_client(fd);
                return;
            }
            self.partials.release(aux);
            unsafe {
                let s = &mut *self.slots.add(fd as usize);
                s.state = STATE_IDLE;
                s.aux_idx = NO_AUX;
                s.last_active_sec = now_sec_mono;
            }
            self.sync_client_epoll(fd, false);
        } else {
            unsafe {
                (*self.slots.add(fd as usize)).last_active_sec = now_sec_mono;
            }
        }
    }

    fn handle_client_readable(
        &mut self,
        fd: i32,
        expected_gen: u16,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) {
        let (state, aux) = unsafe {
            let s = &*self.slots.add(fd as usize);
            if s.state == STATE_CLOSED || s.gen != expected_gen {
                return;
            }
            (s.state, s.aux_idx)
        };

        if state == STATE_WAITING_PG || state == STATE_WRITING_PARTIAL {
            return;
        }

        let is_uring = self.uring.is_some();

        if state == STATE_READING_PARTIAL && aux != NO_AUX {
            let read_ok = {
                let pb = &mut self.partials.items[aux as usize];
                if pb.pos > 0 {
                    let unread = pb.data.len() - pb.pos;
                    pb.data.copy_within(pb.pos.., 0);
                    pb.data.truncate(unread);
                    pb.pos = 0;
                }
                loop {
                    if pb.data.len() >= 65536 {
                        break true;
                    }
                    let n = unsafe {
                        recv(
                            fd,
                            self.rx_tmp.as_mut_ptr() as *mut c_void,
                            self.rx_tmp.len().min(65536 - pb.data.len()),
                            0,
                        )
                    };
                    if n > 0 {
                        pb.data.extend_from_slice(&self.rx_tmp[..n as usize]);
                        if !is_uring && n < 4096 {
                            break true;
                        }
                    } else if n == 0 {
                        break false;
                    } else {
                        let e = errno();
                        if e == EINTR {
                            continue;
                        }
                        if e == EAGAIN {
                            break true;
                        }
                        break false;
                    }
                }
            };
            if !read_ok {
                self.close_client(fd);
                return;
            }
            self.process_buffered_client(fd, now_real, now_sec_mono, now_ms, now_ns);
            return;
        }

        let mut got = 0usize;
        loop {
            let n = unsafe {
                recv(
                    fd,
                    self.rx_tmp.as_mut_ptr().add(got) as *mut c_void,
                    self.rx_tmp.len() - got,
                    0,
                )
            };
            if n > 0 {
                got += n as usize;
                if got >= self.rx_tmp.len() || (n as usize) < 4096 {
                    break;
                }
            } else if n == 0 {
                if got == 0 {
                    self.close_client(fd);
                    return;
                }
                break;
            } else {
                let e = errno();
                if e == EINTR {
                    continue;
                }
                if e == EAGAIN {
                    break;
                }
                self.close_client(fd);
                return;
            }
        }

        if got == 0 {
            return;
        }

        unsafe {
            (*self.slots.add(fd as usize)).last_active_sec = now_sec_mono;
        }

        let rx_ptr = self.rx_tmp.as_ptr();
        let pkt = unsafe { std::slice::from_raw_parts(rx_ptr, got) };
        let need_retry =
            self.dispatch_fresh_packet(fd, pkt, false, now_real, now_sec_mono, now_ms, now_ns);
        if need_retry && is_uring {
            let gen = unsafe { (*self.slots.add(fd as usize)).gen };
            self.handle_client_readable(fd, gen, now_real, now_sec_mono, now_ms, now_ns);
        }
    }

    #[inline(always)]
    fn dispatch_fresh_packet(
        &mut self,
        fd: i32,
        pkt: &[u8],
        sock_more: bool,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) -> bool {
        let got = pkt.len();
        let mut offset = 0usize;
        while offset < got {
            let slice = &pkt[offset..got];
            let gen = unsafe { (*self.slots.add(fd as usize)).gen };
            match parse_http_request(slice, &self.cfg.hmac_key, now_real, &mut self.http_scratch) {
                ParsedAction::NeedMoreData => {
                    let a = self.partials.alloc(slice);
                    unsafe {
                        let s = &mut *self.slots.add(fd as usize);
                        s.state = STATE_READING_PARTIAL;
                        s.aux_idx = a;
                    }
                    return true;
                }
                ParsedAction::BadHttp => {
                    self.send_reply(
                        fd,
                        gen,
                        true,
                        0,
                        ReplyPayload::Static(RESP_400_BAD_HTTP),
                        now_sec_mono,
                        now_ms,
                        now_ns,
                    );
                    return false;
                }
                ParsedAction::Immediate {
                    consumed,
                    close_after,
                    resp,
                } => {
                    offset += consumed;
                    if offset < got && !close_after {
                        let rem_idx = self.partials.alloc(&pkt[offset..got]);
                        unsafe {
                            let s = &mut *self.slots.add(fd as usize);
                            s.aux_idx = rem_idx;
                            if sock_more {
                                s.flags |= FLAG_PENDING_RX;
                            }
                        }
                        self.send_reply(
                            fd,
                            gen,
                            false,
                            0,
                            ReplyPayload::Static(resp),
                            now_sec_mono,
                            now_ms,
                            now_ns,
                        );
                        return false;
                    }
                    if sock_more && !close_after {
                        unsafe {
                            (*self.slots.add(fd as usize)).flags |= FLAG_PENDING_RX;
                        }
                    }
                    self.send_reply(
                        fd,
                        gen,
                        close_after,
                        0,
                        ReplyPayload::Static(resp),
                        now_sec_mono,
                        now_ms,
                        now_ns,
                    );
                    return false;
                }
                ParsedAction::Feed {
                    consumed,
                    close_after,
                    accept_gzip,
                } => {
                    offset += consumed;
                    self.stash_slice_and_wait_pg(fd, &pkt[offset..got], sock_more);
                    self.enqueue_read(ReadReq {
                        client_fd: fd,
                        client_gen: gen,
                        close_after,
                        accept_gzip,
                        retries: 0,
                        enqueued_ms: now_ms,
                        enqueued_ns: now_ns,
                        op: ReadOp::Feed,
                    });
                    return false;
                }
                ParsedAction::GetPost {
                    consumed,
                    close_after,
                    accept_gzip,
                    post_id,
                } => {
                    offset += consumed;
                    self.stash_slice_and_wait_pg(fd, &pkt[offset..got], sock_more);
                    self.enqueue_read(ReadReq {
                        client_fd: fd,
                        client_gen: gen,
                        close_after,
                        accept_gzip,
                        retries: 0,
                        enqueued_ms: now_ms,
                        enqueued_ns: now_ns,
                        op: ReadOp::GetPost { post_id },
                    });
                    return false;
                }
                ParsedAction::Health {
                    consumed,
                    close_after,
                    accept_gzip,
                } => {
                    offset += consumed;
                    self.stash_slice_and_wait_pg(fd, &pkt[offset..got], sock_more);
                    self.enqueue_read(ReadReq {
                        client_fd: fd,
                        client_gen: gen,
                        close_after,
                        accept_gzip,
                        retries: 0,
                        enqueued_ms: now_ms,
                        enqueued_ns: now_ns,
                        op: ReadOp::Health,
                    });
                    return false;
                }
                ParsedAction::CreatePost {
                    consumed,
                    close_after,
                    accept_gzip,
                    user_id,
                    username,
                    body,
                } => {
                    let uname_inline = InlineBytes::from_slice(username);
                    let body_inline = InlineBytes::from_slice(body);
                    offset += consumed;
                    self.stash_slice_and_wait_pg(fd, &pkt[offset..got], sock_more);
                    self.enqueue_write(WriteReq {
                        client_fd: fd,
                        client_gen: gen,
                        close_after,
                        accept_gzip,
                        single_step: false,
                        enqueued_ms: now_ms,
                        enqueued_ns: now_ns,
                        op: WriteOp::CreatePost {
                            user_id,
                            username: uname_inline,
                            body: body_inline,
                        },
                    });
                    return false;
                }
                ParsedAction::LikePost {
                    consumed,
                    close_after,
                    accept_gzip,
                    user_id,
                    post_id,
                } => {
                    offset += consumed;
                    self.stash_slice_and_wait_pg(fd, &pkt[offset..got], sock_more);
                    self.enqueue_write(WriteReq {
                        client_fd: fd,
                        client_gen: gen,
                        close_after,
                        accept_gzip,
                        single_step: false,
                        enqueued_ms: now_ms,
                        enqueued_ns: now_ns,
                        op: WriteOp::LikePost { user_id, post_id },
                    });
                    return false;
                }
            }
        }
        false
    }

    #[inline(always)]
    fn stash_slice_and_wait_pg(&mut self, fd: i32, rem: &[u8], sock_more: bool) {
        let aux = if rem.is_empty() {
            NO_AUX
        } else {
            self.partials.alloc(rem)
        };
        unsafe {
            let s = &mut *self.slots.add(fd as usize);
            s.state = STATE_WAITING_PG;
            s.aux_idx = aux;
            if sock_more {
                s.flags |= FLAG_PENDING_RX;
            }
        }
    }

    fn process_buffered_client(
        &mut self,
        fd: i32,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) {
        let (gen, aux) = unsafe {
            let s = &*self.slots.add(fd as usize);
            if s.state != STATE_READING_PARTIAL || s.aux_idx == NO_AUX {
                return;
            }
            (s.gen, s.aux_idx)
        };

        let (rem_ptr, rem_len) = {
            let pb = &self.partials.items[aux as usize];
            let rem = &pb.data[pb.pos..];
            (rem.as_ptr(), rem.len())
        };
        let rem_slice = unsafe { std::slice::from_raw_parts(rem_ptr, rem_len) };

        match parse_http_request(
            rem_slice,
            &self.cfg.hmac_key,
            now_real,
            &mut self.http_scratch,
        ) {
            ParsedAction::NeedMoreData => {}
            ParsedAction::BadHttp => {
                self.send_reply(
                    fd,
                    gen,
                    true,
                    0,
                    ReplyPayload::Static(RESP_400_BAD_HTTP),
                    now_sec_mono,
                    now_ms,
                    now_ns,
                );
            }
            ParsedAction::Immediate {
                consumed,
                close_after,
                resp,
            } => {
                self.consume_partial_after_parse(fd, aux, consumed, false);
                self.send_reply(
                    fd,
                    gen,
                    close_after,
                    0,
                    ReplyPayload::Static(resp),
                    now_sec_mono,
                    now_ms,
                    now_ns,
                );
            }
            ParsedAction::Feed {
                consumed,
                close_after,
                accept_gzip,
            } => {
                self.consume_partial_after_parse(fd, aux, consumed, true);
                self.enqueue_read(ReadReq {
                    client_fd: fd,
                    client_gen: gen,
                    close_after,
                    accept_gzip,
                    retries: 0,
                    enqueued_ms: now_ms,
                    enqueued_ns: now_ns,
                    op: ReadOp::Feed,
                });
            }
            ParsedAction::GetPost {
                consumed,
                close_after,
                accept_gzip,
                post_id,
            } => {
                self.consume_partial_after_parse(fd, aux, consumed, true);
                self.enqueue_read(ReadReq {
                    client_fd: fd,
                    client_gen: gen,
                    close_after,
                    accept_gzip,
                    retries: 0,
                    enqueued_ms: now_ms,
                    enqueued_ns: now_ns,
                    op: ReadOp::GetPost { post_id },
                });
            }
            ParsedAction::Health {
                consumed,
                close_after,
                accept_gzip,
            } => {
                self.consume_partial_after_parse(fd, aux, consumed, true);
                self.enqueue_read(ReadReq {
                    client_fd: fd,
                    client_gen: gen,
                    close_after,
                    accept_gzip,
                    retries: 0,
                    enqueued_ms: now_ms,
                    enqueued_ns: now_ns,
                    op: ReadOp::Health,
                });
            }
            ParsedAction::CreatePost {
                consumed,
                close_after,
                accept_gzip,
                user_id,
                username,
                body,
            } => {
                let uname_inline = InlineBytes::from_slice(username);
                let body_inline = InlineBytes::from_slice(body);
                self.consume_partial_after_parse(fd, aux, consumed, true);
                self.enqueue_write(WriteReq {
                    client_fd: fd,
                    client_gen: gen,
                    close_after,
                    accept_gzip,
                    single_step: false,
                    enqueued_ms: now_ms,
                    enqueued_ns: now_ns,
                    op: WriteOp::CreatePost {
                        user_id,
                        username: uname_inline,
                        body: body_inline,
                    },
                });
            }
            ParsedAction::LikePost {
                consumed,
                close_after,
                accept_gzip,
                user_id,
                post_id,
            } => {
                self.consume_partial_after_parse(fd, aux, consumed, true);
                self.enqueue_write(WriteReq {
                    client_fd: fd,
                    client_gen: gen,
                    close_after,
                    accept_gzip,
                    single_step: false,
                    enqueued_ms: now_ms,
                    enqueued_ns: now_ns,
                    op: WriteOp::LikePost { user_id, post_id },
                });
            }
        }
    }

    fn consume_partial_after_parse(&mut self, fd: i32, aux: u32, consumed: usize, waiting_pg: bool) {
        let empty = {
            let pb = &mut self.partials.items[aux as usize];
            pb.pos += consumed;
            pb.pos >= pb.data.len()
        };
        if empty {
            self.partials.release(aux);
            unsafe {
                let s = &mut *self.slots.add(fd as usize);
                s.aux_idx = NO_AUX;
                s.state = if waiting_pg { STATE_WAITING_PG } else { STATE_IDLE };
            }
        } else if waiting_pg {
            unsafe {
                (*self.slots.add(fd as usize)).state = STATE_WAITING_PG;
            }
        }
    }

    fn sweep_idle_connections(&mut self, now_sec_mono: u32) {
        if now_sec_mono == self.last_sweep_sec || (self.highest_fd == 0 && self.active_conns == 0) {
            return;
        }
        self.last_sweep_sec = now_sec_mono;
        let limit = self.highest_fd + 1;
        let batch = limit.min(8192);
        for _ in 0..batch {
            if self.sweep_cursor >= limit {
                self.sweep_cursor = 0;
            }
            let fd = self.sweep_cursor;
            self.sweep_cursor += 1;
            let should_close = unsafe {
                let s = &*self.slots.add(fd);
                (s.state == STATE_IDLE || s.state == STATE_READING_PARTIAL)
                    && now_sec_mono.wrapping_sub(s.last_active_sec) > IDLE_TIMEOUT_SEC
            };
            if should_close {
                self.close_client(fd as i32);
            }
        }
    }

    #[inline(always)]
    fn arm_uring_multishot_recv(&mut self, fd: i32, gen: u16) {
        let should_arm = unsafe {
            let s = &mut *self.slots.add(fd as usize);
            if s.state == STATE_CLOSED
                || s.gen != gen
                || (s.flags & (FLAG_RECV_INFLIGHT | FLAG_CLOSE_AFTER)) != 0
            {
                false
            } else {
                s.flags |= FLAG_RECV_INFLIGHT;
                true
            }
        };
        if should_arm {
            if let Some(ref mut ring) = self.uring {
                let ud = UTAG_CLIENT_RECV | ((gen as u64) << 32) | (fd as u32 as u64);
                ring.prep_multishot_recv_pbuf(fd, ud);
            }
        }
    }

    fn uring_accept_one(
        &mut self,
        cfd: i32,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) {
        if (cfd as usize) >= self.max_fds {
            if let Some(ref mut ring) = self.uring {
                ring.prep_client_close(cfd, UTAG_IGNORE);
            } else {
                unsafe {
                    close(cfd);
                }
            }
            self.pause_listeners(now_ns);
            return;
        }
        if (cfd as usize) > self.highest_fd {
            self.highest_fd = cfd as usize;
        }
        let pbuf_ms = self.pbuf_multishot_rx;
        let (gen, was_closed) = unsafe {
            let s = &mut *self.slots.add(cfd as usize);
            let closed_prev = s.state == STATE_CLOSED;
            if s.aux_idx != NO_AUX {
                let old = s.aux_idx;
                s.aux_idx = NO_AUX;
                self.partials.release(old);
            }
            let mut g = s.gen.wrapping_add(1);
            if g == 0 {
                g = 1;
            }
            s.state = STATE_IDLE;
            s.flags = if pbuf_ms { FLAG_RECV_INFLIGHT } else { 0 };
            s.gen = g;
            s.last_active_sec = now_sec_mono;
            s.aux_idx = NO_AUX;
            (g, closed_prev)
        };
        if was_closed {
            self.active_conns += 1;
        }
        if pbuf_ms {
            if let Some(ref mut ring) = self.uring {
                let ud = UTAG_CLIENT_RECV | ((gen as u64) << 32) | (cfd as u32 as u64);
                ring.prep_multishot_recv_pbuf(cfd, ud);
            }
        } else {
            self.handle_client_readable(cfd, gen, now_real, now_sec_mono, now_ms, now_ns);
            let still_open = unsafe {
                let s = &*self.slots.add(cfd as usize);
                s.state != STATE_CLOSED && s.gen == gen
            };
            if still_open {
                if let Some(ref mut ring) = self.uring {
                    let poll_ud = UTAG_CLIENT_POLL | ((gen as u64) << 32) | (cfd as u32 as u64);
                    ring.prep_client_poll_add(cfd, EPOLLIN | EPOLLRDHUP, true, poll_ud);
                    unsafe {
                        (*self.slots.add(cfd as usize)).flags |= FLAG_POLL_ARMED;
                    }
                }
            }
        }
    }

    fn handle_uring_client_poll(
        &mut self,
        fd: i32,
        expected_gen: u16,
        revents: u32,
        more: bool,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) {
        if fd < 0 || (fd as usize) >= self.max_fds {
            return;
        }
        let valid_state = unsafe {
            let s = &mut *self.slots.add(fd as usize);
            if s.state == STATE_CLOSED || s.gen != expected_gen {
                return;
            }
            if !more {
                s.flags &= !FLAG_POLL_ARMED;
            }
            s.state
        };
        if (revents & (EPOLLERR | EPOLLHUP)) != 0 {
            self.close_client(fd);
            return;
        }
        if !more {
            if let Some(ref mut ring) = self.uring {
                let poll_ud =
                    UTAG_CLIENT_POLL | ((expected_gen as u64) << 32) | (fd as u32 as u64);
                ring.prep_client_poll_add(fd, EPOLLIN | EPOLLRDHUP, true, poll_ud);
                unsafe {
                    (*self.slots.add(fd as usize)).flags |= FLAG_POLL_ARMED;
                }
            }
        }
        if valid_state == STATE_WAITING_PG || valid_state == STATE_WRITING_PARTIAL {
            unsafe {
                (*self.slots.add(fd as usize)).flags |= FLAG_PENDING_RX;
            }
            return;
        }
        self.handle_client_readable(fd, expected_gen, now_real, now_sec_mono, now_ms, now_ns);
    }

    fn handle_uring_client_recv(
        &mut self,
        fd: i32,
        expected_gen: u16,
        res: i32,
        cqe_flags: u32,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) -> bool {
        let has_buf = (cqe_flags & IORING_CQE_F_BUFFER) != 0;
        let more = (cqe_flags & IORING_CQE_F_MORE) != 0;
        let bid = (cqe_flags >> IORING_CQE_BUFFER_SHIFT) as u16;

        if fd < 0 || (fd as usize) >= self.max_fds {
            if has_buf {
                if let Some(ref mut ring) = self.uring {
                    ring.recycle_pbuf(bid);
                    return true;
                }
            }
            return false;
        }

        let (state, aux) = unsafe {
            let s = &mut *self.slots.add(fd as usize);
            if s.state == STATE_CLOSED || s.gen != expected_gen {
                if has_buf {
                    if let Some(ref mut ring) = self.uring {
                        ring.recycle_pbuf(bid);
                        return true;
                    }
                }
                return false;
            }
            if !more {
                s.flags &= !FLAG_RECV_INFLIGHT;
            }
            (s.state, s.aux_idx)
        };

        if res <= 0 {
            let mut recycled = false;
            if has_buf {
                if let Some(ref mut ring) = self.uring {
                    ring.recycle_pbuf(bid);
                    recycled = true;
                }
            }
            if res == 0 {
                if state == STATE_WAITING_PG || state == STATE_WRITING_PARTIAL {
                    unsafe {
                        (*self.slots.add(fd as usize)).flags |= FLAG_CLOSE_AFTER;
                    }
                } else {
                    self.close_client(fd);
                }
                return recycled;
            }
            let err = -res;
            if err == ECANCELED {
                return recycled;
            }
            if err == ENOBUFS || err == EAGAIN || err == EINTR {
                self.enobufs_retry.push((fd, expected_gen));
                return recycled;
            }
            self.close_client(fd);
            return recycled;
        }

        let got = res as usize;
        let (pkt_ptr, pkt_len) = if has_buf {
            match self.uring {
                Some(ref ring) => ring.pbuf_raw(bid, got),
                None => return false,
            }
        } else {
            return false;
        };

        unsafe {
            (*self.slots.add(fd as usize)).last_active_sec = now_sec_mono;
        }

        let pkt = unsafe { std::slice::from_raw_parts(pkt_ptr, pkt_len) };

        if state == STATE_WAITING_PG || state == STATE_WRITING_PARTIAL {
            if aux != NO_AUX {
                let pb = &mut self.partials.items[aux as usize];
                if pb.data.len() - pb.pos + pkt.len() > 65536 {
                    if let Some(ref mut ring) = self.uring {
                        ring.recycle_pbuf(bid);
                    }
                    self.close_client(fd);
                    return true;
                }
                pb.data.extend_from_slice(pkt);
            } else {
                let a = self.partials.alloc(pkt);
                unsafe {
                    (*self.slots.add(fd as usize)).aux_idx = a;
                }
            }
        } else if state == STATE_READING_PARTIAL && aux != NO_AUX {
            let too_big = {
                let pb = &mut self.partials.items[aux as usize];
                if pb.pos > 0 {
                    let unread = pb.data.len() - pb.pos;
                    pb.data.copy_within(pb.pos.., 0);
                    pb.data.truncate(unread);
                    pb.pos = 0;
                }
                if pb.data.len() + pkt.len() > 65536 {
                    true
                } else {
                    pb.data.extend_from_slice(pkt);
                    false
                }
            };
            if too_big {
                if let Some(ref mut ring) = self.uring {
                    ring.recycle_pbuf(bid);
                }
                self.close_client(fd);
                return true;
            }
            self.process_buffered_client(fd, now_real, now_sec_mono, now_ms, now_ns);
        } else {
            let _ =
                self.dispatch_fresh_packet(fd, pkt, false, now_real, now_sec_mono, now_ms, now_ns);
        }

        if let Some(ref mut ring) = self.uring {
            ring.recycle_pbuf(bid);
        }

        if !more {
            self.arm_uring_multishot_recv(fd, expected_gen);
        }
        true
    }

    fn handle_uring_client_send(
        &mut self,
        ud: u64,
        res: i32,
        now_real: i64,
        now_sec_mono: u32,
        now_ms: u32,
        now_ns: u64,
    ) {
        let fd = (ud & 0xFF_FFFF) as i32;
        let expected_gen = ((ud >> 24) & 0xFFFF) as u16;
        let tx_idx = ((ud >> 40) & 0xFFFFF) as u32;

        if fd < 0 || (fd as usize) >= self.max_fds {
            self.tx_pool.release(tx_idx);
            return;
        }
        let valid = unsafe {
            let s = &*self.slots.add(fd as usize);
            s.state != STATE_CLOSED && s.gen == expected_gen
        };
        if !valid {
            self.tx_pool.release(tx_idx);
            return;
        }

        if res > 0 {
            let (rem_ptr, rem_len) = self.tx_pool.advance(tx_idx, res as usize);
            if rem_len > 0 {
                if let Some(ref mut ring) = self.uring {
                    ring.prep_client_send(fd, rem_ptr, rem_len, MSG_NOSIGNAL as u32, ud);
                }
                return;
            }
            self.tx_pool.release(tx_idx);
            let (close_after, leftover_aux, had_pending_rx, need_rearm) = unsafe {
                let s = &mut *self.slots.add(fd as usize);
                let ca = (s.flags & FLAG_CLOSE_AFTER) != 0;
                let prx = (s.flags & FLAG_PENDING_RX) != 0;
                s.flags &= !(FLAG_CLOSE_AFTER | FLAG_PENDING_RX);
                s.last_active_sec = now_sec_mono;
                let a = s.aux_idx;
                if !ca {
                    s.state = if a == NO_AUX {
                        STATE_IDLE
                    } else {
                        STATE_READING_PARTIAL
                    };
                }
                let rearm = !ca && (s.flags & FLAG_RECV_INFLIGHT) == 0;
                (ca, a, prx, rearm)
            };
            if close_after {
                self.close_client(fd);
                return;
            }
            if self.pbuf_multishot_rx && need_rearm {
                self.arm_uring_multishot_recv(fd, expected_gen);
            }
            if leftover_aux != NO_AUX {
                self.process_buffered_client(fd, now_real, now_sec_mono, now_ms, now_ns);
            } else if had_pending_rx && !self.pbuf_multishot_rx {
                self.handle_client_readable(
                    fd,
                    expected_gen,
                    now_real,
                    now_sec_mono,
                    now_ms,
                    now_ns,
                );
            }
        } else {
            let err = -res;
            if res < 0 && (err == EINTR || err == EAGAIN) {
                let (rem_ptr, rem_len) = self.tx_pool.remaining(tx_idx);
                if rem_len > 0 {
                    if let Some(ref mut ring) = self.uring {
                        ring.prep_client_send(fd, rem_ptr, rem_len, MSG_NOSIGNAL as u32, ud);
                    }
                    return;
                }
            }
            self.tx_pool.release(tx_idx);
            self.close_client(fd);
        }
    }
}

impl WorkerState {
    #[inline(always)]
    fn read_horizon_ns(&self) -> u64 {
        if self.core.cfg.horizon_off || self.core.active_conns < 256 {
            return 0;
        }
        if self.core.cfg.horizon_us > 0 {
            return self.core.cfg.horizon_us * 1_000;
        }
        let total_est = self.core.active_conns * self.core.cfg.num_workers;
        let mut sum = 0u64;
        let mut cnt = 0u64;
        for idx in 0..self.core.cfg.read_lanes {
            let rtt = self.pg_lanes[idx].ewma_rtt_ns;
            if rtt > 0 {
                sum += rtt;
                cnt += 1;
            }
        }
        if total_est < 250_000 {
            if cnt == 0 {
                500_000
            } else {
                (sum / (cnt * 2)).clamp(400_000, 800_000)
            }
        } else if cnt == 0 {
            1_500_000
        } else {
            (sum / cnt).clamp(1_000_000, 2_000_000)
        }
    }

    #[inline(always)]
    fn write_horizon_ns(&self) -> u64 {
        if self.core.cfg.horizon_off || self.core.active_conns < 256 {
            return 0;
        }
        if self.core.cfg.horizon_us > 0 {
            return self.core.cfg.horizon_us * 1_000;
        }
        let total_est = self.core.active_conns * self.core.cfg.num_workers;
        let mut sum = 0u64;
        let mut cnt = 0u64;
        for idx in self.core.cfg.read_lanes..self.pg_lanes.len() {
            let rtt = self.pg_lanes[idx].ewma_rtt_ns;
            if rtt > 0 {
                sum += rtt;
                cnt += 1;
            }
        }
        if total_est < 250_000 {
            if cnt == 0 {
                600_000
            } else {
                (sum / (cnt * 2)).clamp(500_000, 1_000_000)
            }
        } else if cnt == 0 {
            2_000_000
        } else {
            (sum / cnt).clamp(1_500_000, 2_500_000)
        }
    }

    fn dispatch_pg_queues(&mut self, now_ns: u64, now_sec_mono: u32, now_ms: u32) -> (bool, u64) {
        let r_end = self.core.cfg.read_lanes;
        let total = self.pg_lanes.len();
        let defer_pg_tx = self.core.uring_pg_enabled;
        let mut next_wait_ns: u64 = 250_000_000;
        let mut need_short_timer = false;

        for idx in 0..total {
            if self.pg_lanes[idx].phase == PgPhase::Disconnected {
                if now_ns >= self.pg_lanes[idx].next_reconnect_ns {
                    let db_cfg = self.core.cfg.db.clone();
                    if self.pg_lanes[idx].start_connect(&db_cfg, now_ns, defer_pg_tx) {
                        self.core.sync_pg_lane(idx, &mut self.pg_lanes[idx]);
                    }
                }
                if self.pg_lanes[idx].phase == PgPhase::Disconnected {
                    need_short_timer = true;
                    let rem = self.pg_lanes[idx].next_reconnect_ns.saturating_sub(now_ns);
                    next_wait_ns = next_wait_ns.min(rem.clamp(5_000_000, 50_000_000));
                }
            } else if self.pg_lanes[idx].is_busy()
                && now_ns.saturating_sub(self.pg_lanes[idx].last_activity_ns) > PG_WATCHDOG_NS
            {
                let mut retry_reads = VecDeque::new();
                {
                    let core = &mut self.core;
                    let lane = &mut self.pg_lanes[idx];
                    lane.fail_and_reset(
                        now_ns,
                        &mut retry_reads,
                        &mut |cfd, cgen, close_after, enq_ms, rep| {
                            core.send_reply(
                                cfd,
                                cgen,
                                close_after,
                                enq_ms,
                                rep,
                                now_sec_mono,
                                now_ms,
                                now_ns,
                            );
                        },
                    );
                    let db_cfg = core.cfg.db.clone();
                    let _ = lane.start_connect(&db_cfg, now_ns, defer_pg_tx);
                    core.sync_pg_lane(idx, lane);
                }
                while let Some(r) = retry_reads.pop_back() {
                    self.core.read_queue.push_front(r);
                }
            }
        }

        if !self.core.read_queue.is_empty() {
            let mut any_alive = false;
            let mut idle_r = 0usize;
            for idx in 0..r_end {
                if self.pg_lanes[idx].phase != PgPhase::Disconnected {
                    any_alive = true;
                }
                if self.pg_lanes[idx].is_idle_ready() {
                    idle_r += 1;
                }
            }
            if !any_alive {
                while let Some(req) = self.core.read_queue.pop_front() {
                    let s = match req.op {
                        ReadOp::Health => {
                            if req.close_after {
                                RESP_503_DB_UNREACHABLE_CLOSE
                            } else {
                                RESP_503_DB_UNREACHABLE
                            }
                        }
                        _ => RESP_500_INTERNAL,
                    };
                    self.core.send_reply(
                        req.client_fd,
                        req.client_gen,
                        req.close_after,
                        req.enqueued_ms,
                        ReplyPayload::Static(s),
                        now_sec_mono,
                        now_ms,
                        now_ns,
                    );
                }
            } else {
                let r_hor = self.read_horizon_ns();
                if idle_r > 0 {
                    let per_lane_cap = self.core.cfg.max_read_batch;
                    for step in 0..r_end {
                        let Some(front) = self.core.read_queue.front() else {
                            break;
                        };
                        let front_expired =
                            r_hor == 0 || now_ns.saturating_sub(front.enqueued_ns) >= r_hor;
                        if !front_expired
                            && self.core.read_queue.len() < self.core.cfg.min_read_batch
                        {
                            break;
                        }
                        let idx = (self.core.read_rr + step) % r_end;
                        if self.pg_lanes[idx].is_idle_ready() {
                            let res = self.pg_lanes[idx].send_read_batch(
                                &mut self.core.read_queue,
                                per_lane_cap,
                                now_ns,
                                now_ms,
                                self.core.cfg.lifo_age_ms,
                                defer_pg_tx,
                            );
                            if res.is_err() {
                                let mut retry_reads = VecDeque::new();
                                {
                                    let core = &mut self.core;
                                    let lane = &mut self.pg_lanes[idx];
                                    lane.fail_and_reset(
                                        now_ns,
                                        &mut retry_reads,
                                        &mut |cfd, cgen, close_after, enq_ms, rep| {
                                            core.send_reply(
                                                cfd,
                                                cgen,
                                                close_after,
                                                enq_ms,
                                                rep,
                                                now_sec_mono,
                                                now_ms,
                                                now_ns,
                                            );
                                        },
                                    );
                                    let db_cfg = core.cfg.db.clone();
                                    let _ = lane.start_connect(&db_cfg, now_ns, defer_pg_tx);
                                }
                                while let Some(r) = retry_reads.pop_back() {
                                    self.core.read_queue.push_front(r);
                                }
                            } else {
                                self.core.read_rr = (idx + 1) % r_end;
                            }
                            self.core.sync_pg_lane(idx, &mut self.pg_lanes[idx]);
                        }
                    }
                }

                if let Some(front) = self.core.read_queue.front() {
                    need_short_timer = true;
                    let any_idle = (0..r_end).any(|i| self.pg_lanes[i].is_idle_ready());
                    if any_idle && r_hor > 0 {
                        let rem = (front.enqueued_ns + r_hor).saturating_sub(now_ns);
                        next_wait_ns = next_wait_ns.min(rem.max(50_000));
                    } else {
                        next_wait_ns = next_wait_ns.min(5_000_000);
                    }
                }
            }
        }

        if !self.core.write_queue.is_empty() {
            let w_count = total - r_end;
            let mut any_alive = false;
            let mut idle_w = 0usize;
            for idx in r_end..total {
                if self.pg_lanes[idx].phase != PgPhase::Disconnected {
                    any_alive = true;
                }
                if self.pg_lanes[idx].is_idle_ready() {
                    idle_w += 1;
                }
            }
            if !any_alive {
                while let Some(req) = self.core.write_queue.pop_front() {
                    self.core.send_reply(
                        req.client_fd,
                        req.client_gen,
                        req.close_after,
                        req.enqueued_ms,
                        ReplyPayload::Static(RESP_500_INTERNAL),
                        now_sec_mono,
                        now_ms,
                        now_ns,
                    );
                }
            } else {
                let w_hor = self.write_horizon_ns();
                if idle_w > 0 {
                    let per_w_cap = self.core.cfg.max_write_batch;
                    for step in 0..w_count {
                        let Some(front) = self.core.write_queue.front() else {
                            break;
                        };
                        let front_expired = front.single_step
                            || w_hor == 0
                            || now_ns.saturating_sub(front.enqueued_ns) >= w_hor;
                        if !front_expired
                            && self.core.write_queue.len() < self.core.cfg.min_write_batch
                        {
                            break;
                        }
                        let idx = r_end + ((self.core.write_rr + step) % w_count);
                        if self.pg_lanes[idx].is_idle_ready() {
                            let res = self.pg_lanes[idx].send_write_batch(
                                &mut self.core.write_queue,
                                per_w_cap,
                                now_ns,
                                defer_pg_tx,
                            );
                            if res.is_err() {
                                let mut retry_reads = VecDeque::new();
                                let core = &mut self.core;
                                let lane = &mut self.pg_lanes[idx];
                                lane.fail_and_reset(
                                    now_ns,
                                    &mut retry_reads,
                                    &mut |cfd, cgen, close_after, enq_ms, rep| {
                                        core.send_reply(
                                            cfd,
                                            cgen,
                                            close_after,
                                            enq_ms,
                                            rep,
                                            now_sec_mono,
                                            now_ms,
                                            now_ns,
                                        );
                                    },
                                );
                                let db_cfg = core.cfg.db.clone();
                                let _ = lane.start_connect(&db_cfg, now_ns, defer_pg_tx);
                            } else {
                                self.core.write_rr = (idx - r_end + 1) % w_count;
                            }
                            self.core.sync_pg_lane(idx, &mut self.pg_lanes[idx]);
                        }
                    }
                }

                if let Some(front) = self.core.write_queue.front() {
                    need_short_timer = true;
                    let any_idle = (r_end..total).any(|i| self.pg_lanes[i].is_idle_ready());
                    if any_idle && w_hor > 0 {
                        let rem = (front.enqueued_ns + w_hor).saturating_sub(now_ns);
                        next_wait_ns = next_wait_ns.min(rem.max(50_000));
                    } else {
                        next_wait_ns = next_wait_ns.min(5_000_000);
                    }
                }
            }
        }

        if !self.core.listeners_attached {
            need_short_timer = true;
            let rem = self.core.accept_paused_until_ns.saturating_sub(now_ns);
            next_wait_ns = next_wait_ns.min(rem.max(1_000_000));
        }

        (need_short_timer, next_wait_ns)
    }

    fn process_pg_uring_cqe(
        &mut self,
        cqe: IoUringCqe,
        startup_db_cfg: &DbConfig,
        now_ns: u64,
        now_sec_mono: u32,
        now_ms: u32,
        uptime_s: u64,
        retry_reads: &mut VecDeque<ReadReq>,
        retry_writes: &mut VecDeque<WriteReq>,
    ) -> bool {
        let total_pg = self.pg_lanes.len();
        let ud = cqe.user_data;
        let tag = ud & UTAG_MASK;
        let idx = (ud & 0xFFFF) as usize;
        let g = ((ud >> 16) & 0xFFFF) as u16;

        if tag == UTAG_PG_RECV {
            let has_buf = (cqe.flags & IORING_CQE_F_BUFFER) != 0;
            let more = (cqe.flags & IORING_CQE_F_MORE) != 0;
            let bid = (cqe.flags >> IORING_CQE_BUFFER_SHIFT) as u16;

            if idx >= total_pg
                || g != self.pg_lanes[idx].conn_gen
                || self.pg_lanes[idx].phase == PgPhase::Disconnected
            {
                if has_buf {
                    if let Some(ref mut ring) = self.core.uring {
                        ring.recycle_pg_pbuf_span(bid, cqe.res);
                        return true;
                    }
                }
                return false;
            }

            if !more {
                self.pg_lanes[idx].uring_rx_armed = false;
                self.core.pg_poll_armed[idx] &= !PG_ARM_URING_RX;
            }

            if cqe.res <= 0 {
                let mut recycled = false;
                if has_buf {
                    if let Some(ref mut ring) = self.core.uring {
                        ring.recycle_pg_pbuf_span(bid, cqe.res);
                        recycled = true;
                    }
                }
                if cqe.res < 0 {
                    let err = -cqe.res;
                    if err == ECANCELED {
                        return recycled;
                    }
                    if err == ENOBUFS || err == EAGAIN || err == EINTR {
                        self.core.pg_enobufs_retry.push((idx, g));
                        return recycled;
                    }
                }
                retry_reads.clear();
                {
                    let core = &mut self.core;
                    let lane = &mut self.pg_lanes[idx];
                    lane.fail_and_reset(
                        now_ns,
                        retry_reads,
                        &mut |cfd, cgen, close_after, enq_ms, rep| {
                            core.send_reply(
                                cfd,
                                cgen,
                                close_after,
                                enq_ms,
                                rep,
                                now_sec_mono,
                                now_ms,
                                now_ns,
                            );
                        },
                    );
                    if now_ns >= lane.next_reconnect_ns {
                        let _ = lane.start_connect(startup_db_cfg, now_ns, core.uring_pg_enabled);
                    }
                    core.sync_pg_lane(idx, lane);
                }
                while let Some(r) = retry_reads.pop_back() {
                    self.core.read_queue.push_front(r);
                }
                return recycled;
            }

            if !has_buf {
                return false;
            }

            let ((p0, len0), (p1, len1)) = match self.core.uring {
                Some(ref ring) => ring.pg_pbuf_spans(bid, cqe.res as usize),
                None => return false,
            };
            let pkt0 = unsafe { std::slice::from_raw_parts(p0, len0) };
            let pkt1 = unsafe { std::slice::from_raw_parts(p1, len1) };

            retry_reads.clear();
            retry_writes.clear();
            let gz_min = self.core.cfg.gz_min_bytes;
            let igz = self.core.igzip;
            {
                let core = &mut self.core;
                let lane = &mut self.pg_lanes[idx];
                lane.ingest_uring_rx_slice(
                    pkt0,
                    startup_db_cfg,
                    now_ns,
                    uptime_s,
                    gz_min,
                    igz,
                    retry_reads,
                    retry_writes,
                    &mut |cfd, cgen, close_after, enq_ms, rep| {
                        core.send_reply(
                            cfd,
                            cgen,
                            close_after,
                            enq_ms,
                            rep,
                            now_sec_mono,
                            now_ms,
                            now_ns,
                        );
                    },
                );
                if len1 > 0 && lane.phase != PgPhase::Disconnected {
                    lane.ingest_uring_rx_slice(
                        pkt1,
                        startup_db_cfg,
                        now_ns,
                        uptime_s,
                        gz_min,
                        igz,
                        retry_reads,
                        retry_writes,
                        &mut |cfd, cgen, close_after, enq_ms, rep| {
                            core.send_reply(
                                cfd,
                                cgen,
                                close_after,
                                enq_ms,
                                rep,
                                now_sec_mono,
                                now_ms,
                                now_ns,
                            );
                        },
                    );
                }
                if let Some(ref mut ring) = core.uring {
                    ring.recycle_pg_pbuf_span(bid, cqe.res);
                }
                if lane.phase == PgPhase::Disconnected && now_ns >= lane.next_reconnect_ns {
                    let _ = lane.start_connect(startup_db_cfg, now_ns, core.uring_pg_enabled);
                }
                core.sync_pg_lane(idx, lane);
            }
            while let Some(w) = retry_writes.pop_back() {
                self.core.write_queue.push_front(w);
            }
            while let Some(r) = retry_reads.pop_back() {
                self.core.read_queue.push_front(r);
            }
            return true;
        }

        if tag == UTAG_PG_SEND {
            if idx >= total_pg
                || g != self.pg_lanes[idx].conn_gen
                || self.pg_lanes[idx].phase == PgPhase::Disconnected
            {
                return false;
            }
            if cqe.res > 0 {
                let (rem_ptr, rem_len) = self.pg_lanes[idx].advance_uring_tx(cqe.res as usize, now_ns);
                if rem_len > 0 {
                    let fd = self.pg_lanes[idx].fd;
                    if let Some(ref mut ring) = self.core.uring {
                        ring.prep_pg_send(fd, rem_ptr, rem_len, MSG_NOSIGNAL as u32, ud);
                    }
                } else {
                    self.core.pg_poll_armed[idx] &= !PG_ARM_URING_TX;
                    self.core.sync_pg_lane(idx, &mut self.pg_lanes[idx]);
                }
            } else {
                let err = -cqe.res;
                if cqe.res < 0 && err == ECANCELED {
                    return false;
                }
                if cqe.res < 0 && (err == EINTR || err == EAGAIN) {
                    self.pg_lanes[idx].uring_tx_inflight = false;
                    self.core.pg_poll_armed[idx] &= !PG_ARM_URING_TX;
                    if (self.core.pg_poll_armed[idx] & PG_ARM_POLL_OUT) == 0 {
                        let fd = self.pg_lanes[idx].fd;
                        if let Some(ref mut ring) = self.core.uring {
                            ring.prep_poll_add(
                                fd,
                                EPOLLOUT,
                                false,
                                UTAG_PG_OUT | ((g as u64) << 16) | (idx as u64),
                            );
                        }
                        self.core.pg_poll_armed[idx] |= PG_ARM_POLL_OUT;
                    }
                    return false;
                }
                retry_reads.clear();
                {
                    let core = &mut self.core;
                    let lane = &mut self.pg_lanes[idx];
                    lane.fail_and_reset(
                        now_ns,
                        retry_reads,
                        &mut |cfd, cgen, close_after, enq_ms, rep| {
                            core.send_reply(
                                cfd,
                                cgen,
                                close_after,
                                enq_ms,
                                rep,
                                now_sec_mono,
                                now_ms,
                                now_ns,
                            );
                        },
                    );
                    if now_ns >= lane.next_reconnect_ns {
                        let _ = lane.start_connect(startup_db_cfg, now_ns, core.uring_pg_enabled);
                    }
                    core.sync_pg_lane(idx, lane);
                }
                while let Some(r) = retry_reads.pop_back() {
                    self.core.read_queue.push_front(r);
                }
            }
            return false;
        }

        if tag == UTAG_PG_IN || tag == UTAG_PG_OUT {
            if idx >= total_pg
                || g != self.pg_lanes[idx].conn_gen
                || self.pg_lanes[idx].phase == PgPhase::Disconnected
            {
                return false;
            }
            if tag == UTAG_PG_OUT {
                self.pg_lanes[idx].epoll_events &= !EPOLLOUT;
                self.core.pg_poll_armed[idx] &= !PG_ARM_POLL_OUT;
                if self.core.uring_pg_enabled {
                    retry_reads.clear();
                    {
                        let core = &mut self.core;
                        let lane = &mut self.pg_lanes[idx];
                        if lane.phase == PgPhase::Connecting {
                            lane.complete_connect(
                                startup_db_cfg,
                                now_ns,
                                true,
                                retry_reads,
                                &mut |cfd, cgen, close_after, enq_ms, rep| {
                                    core.send_reply(
                                        cfd,
                                        cgen,
                                        close_after,
                                        enq_ms,
                                        rep,
                                        now_sec_mono,
                                        now_ms,
                                        now_ns,
                                    );
                                },
                            );
                        }
                        if lane.phase == PgPhase::Disconnected && now_ns >= lane.next_reconnect_ns {
                            let _ = lane.start_connect(startup_db_cfg, now_ns, true);
                        }
                        core.sync_pg_lane(idx, lane);
                    }
                    while let Some(r) = retry_reads.pop_back() {
                        self.core.read_queue.push_front(r);
                    }
                    return false;
                }
            } else if (cqe.flags & IORING_CQE_F_MORE) == 0 {
                self.pg_lanes[idx].epoll_events &= !EPOLLIN;
                self.core.pg_poll_armed[idx] &= !PG_ARM_POLL_IN;
            }
            let rev = if cqe.res > 0 { cqe.res as u32 } else { EPOLLERR };
            let can_r =
                tag == UTAG_PG_IN || (rev & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
            let can_w = tag == UTAG_PG_OUT || (rev & EPOLLOUT) != 0;
            retry_reads.clear();
            retry_writes.clear();
            let gz_min = self.core.cfg.gz_min_bytes;
            let igz = self.core.igzip;
            {
                let core = &mut self.core;
                let lane = &mut self.pg_lanes[idx];
                lane.handle_io(
                    can_r,
                    can_w,
                    startup_db_cfg,
                    now_ns,
                    uptime_s,
                    gz_min,
                    igz,
                    retry_reads,
                    retry_writes,
                    &mut |cfd, cgen, close_after, enq_ms, rep| {
                        core.send_reply(
                            cfd,
                            cgen,
                            close_after,
                            enq_ms,
                            rep,
                            now_sec_mono,
                            now_ms,
                            now_ns,
                        );
                    },
                );
                if lane.phase == PgPhase::Disconnected && now_ns >= lane.next_reconnect_ns {
                    let _ = lane.start_connect(startup_db_cfg, now_ns, false);
                }
                core.sync_pg_lane(idx, lane);
            }
            while let Some(w) = retry_writes.pop_back() {
                self.core.write_queue.push_front(w);
            }
            while let Some(r) = retry_reads.pop_back() {
                self.core.read_queue.push_front(r);
            }
        }
        false
    }
}

pub fn run_worker(cfg: WorkerConfig) {
    let want_direct = cfg.direct_fd && cfg.uring_pbuf_rx;
    let (sq_want, cq_want, pbuf_want) = if cfg.num_workers <= 4 {
        (8192u32, 32768u32, 8192usize)
    } else if cfg.num_workers <= 8 {
        (4096u32, 16384u32, 4096usize)
    } else if cfg.num_workers <= 16 {
        (4096u32, 8192u32, 2048usize)
    } else {
        (2048u32, 4096u32, 1024usize)
    };
    let uring_opt = if cfg.use_uring {
        match Uring::new(sq_want, cq_want, pbuf_want, cfg.max_fds, want_direct) {
            Ok(r) => Some(r),
            Err(e) => {
                eprintln!(
                    "worker {}: Uring::new fallback to epoll ({})",
                    cfg.worker_id, e
                );
                None
            }
        }
    } else {
        None
    };

    let mut max_fds = cfg.max_fds;
    let mut pbuf_multishot_rx = false;
    let mut uring_pg_enabled = false;
    if let Some(ref ring) = uring_opt {
        if ring.use_direct_fds && ring.direct_fds_cap > 0 {
            max_fds = max_fds.min(ring.direct_fds_cap);
        }
        pbuf_multishot_rx = cfg.uring_pbuf_rx && ring.has_pbuf;
        uring_pg_enabled = cfg.uring_pg_io && ring.has_pg_pbuf;
    }

    let epfd = if uring_opt.is_none() {
        let fd = unsafe { epoll_create1(EPOLL_CLOEXEC) };
        if fd < 0 {
            eprintln!("worker {}: epoll_create1 failed", cfg.worker_id);
            std::process::exit(1);
        }
        fd
    } else {
        -1
    };

    let slots = unsafe { calloc(max_fds, std::mem::size_of::<ConnSlot>()) as *mut ConnSlot };
    if slots.is_null() {
        eprintln!("worker {}: calloc({}) failed", cfg.worker_id, max_fds);
        std::process::exit(1);
    }

    let igzip = unsafe { igzip_ctx_new(cfg.gz_level, if cfg.gz_train_huff { 1 } else { 0 }) };

    let total_pg = cfg.read_lanes + cfg.write_lanes;
    let mut pg_lanes = Vec::with_capacity(total_pg);
    for i in 0..total_pg {
        pg_lanes.push(PgLane::new(i >= cfg.read_lanes));
    }

    let mut state = WorkerState {
        core: WorkerCore {
            lfd0: -1,
            lfd1: -1,
            listeners_attached: false,
            accept_paused_until_ns: 0,
            epfd,
            slots,
            max_fds,
            active_conns: 0,
            highest_fd: 0,
            sweep_cursor: 0,
            last_sweep_sec: 0,
            partials: PartialPool::new(),
            tx_pool: TxPool::new(),
            uring: uring_opt,
            pbuf_multishot_rx,
            uring_pg_enabled,
            enobufs_retry: Vec::with_capacity(64),
            pg_enobufs_retry: Vec::with_capacity(32),
            pg_poll_gen: vec![0u16; total_pg],
            pg_poll_armed: vec![0u32; total_pg],
            http_scratch: HttpScratch::new(),
            rx_tmp: vec![0u8; 65536],
            igzip,
            read_queue: VecDeque::with_capacity(1024),
            write_queue: VecDeque::with_capacity(256),
            read_rr: 0,
            write_rr: 0,
            cfg,
        },
        pg_lanes,
    };

    if state.core.uring.is_some() {
        run_worker_uring_loop(&mut state);
        return;
    }

    let start_ns = monotonic_ns();
    let startup_db_cfg = state.core.cfg.db.clone();
    for idx in 0..total_pg {
        if state.pg_lanes[idx].start_connect(&startup_db_cfg, start_ns, false) {
            state.core.sync_pg_lane(idx, &mut state.pg_lanes[idx]);
        }
    }

    let mut events = vec![EpollEvent::default(); 2048];
    let deadline_ns = start_ns + 400_000_000;
    let mut retry_reads = VecDeque::with_capacity(64);
    let mut retry_writes = VecDeque::with_capacity(64);
    loop {
        let all_ready = state.pg_lanes.iter().all(|l| l.phase == PgPhase::Ready);
        let now_ns = monotonic_ns();
        if all_ready || now_ns >= deadline_ns {
            break;
        }
        let n = unsafe { epoll_wait(state.core.epfd, events.as_mut_ptr(), 256, 10) };
        if n > 0 {
            let now_ns = monotonic_ns();
            let uptime_s = now_ns.saturating_sub(state.core.cfg.boot_ns) / 1_000_000_000;
            for ev in &events[..n as usize] {
                let d = ev.data;
                if (d & TAG_PG_BASE) != 0 {
                    let idx = (d & !TAG_PG_BASE) as usize;
                    let can_r = (ev.events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
                    let can_w = (ev.events & EPOLLOUT) != 0;
                    retry_reads.clear();
                    retry_writes.clear();
                    let gz_min = state.core.cfg.gz_min_bytes;
                    let igz = state.core.igzip;
                    state.pg_lanes[idx].handle_io(
                        can_r,
                        can_w,
                        &startup_db_cfg,
                        now_ns,
                        uptime_s,
                        gz_min,
                        igz,
                        &mut retry_reads,
                        &mut retry_writes,
                        &mut |_, _, _, _, _| {},
                    );
                    state.core.sync_pg_lane(idx, &mut state.pg_lanes[idx]);
                }
            }
        }
    }

    let lfd0 = match open_listener(
        &state.core.cfg.listen_addr_0,
        state.core.cfg.piggyback_ack,
        state.core.cfg.try_bbr,
    ) {
        Ok(fd) => fd,
        Err(e) => {
            eprintln!("worker {}: {}", state.core.cfg.worker_id, e);
            std::process::exit(1);
        }
    };
    state.core.lfd0 = lfd0;
    let mut ev0 = EpollEvent {
        events: EPOLLIN,
        data: TAG_LISTEN_0,
    };
    unsafe {
        epoll_ctl(state.core.epfd, EPOLL_CTL_ADD, lfd0, &mut ev0);
    }

    if let Some(addr1) = state.core.cfg.listen_addr_1 {
        let lfd1 = match open_listener(
            &addr1,
            state.core.cfg.piggyback_ack,
            state.core.cfg.try_bbr,
        ) {
            Ok(fd) => fd,
            Err(e) => {
                eprintln!("worker {}: {}", state.core.cfg.worker_id, e);
                std::process::exit(1);
            }
        };
        state.core.lfd1 = lfd1;
        let mut ev1 = EpollEvent {
            events: EPOLLIN,
            data: TAG_LISTEN_1,
        };
        unsafe {
            epoll_ctl(state.core.epfd, EPOLL_CTL_ADD, lfd1, &mut ev1);
        }
    }
    state.core.listeners_attached = true;

    let mut wait_ns: u64 = 250_000_000;
    let mut cached_mono_sec: u32 = u32::MAX;
    let mut cached_real_sec: i64 = realtime_sec();
    while !WORKER_STOP.load(Ordering::Relaxed) {
        let n = epoll_pwait2_ns(
            state.core.epfd,
            events.as_mut_ptr(),
            events.len() as i32,
            wait_ns,
        );

        let now_ns = monotonic_ns();
        let now_sec_mono = (now_ns / 1_000_000_000) as u32;
        let now_ms = (now_ns / 1_000_000) as u32;
        if now_sec_mono != cached_mono_sec {
            cached_mono_sec = now_sec_mono;
            cached_real_sec = realtime_sec();
        }
        let now_real = cached_real_sec;
        let uptime_s = now_ns.saturating_sub(state.core.cfg.boot_ns) / 1_000_000_000;

        if n > 0 {
            for i in 0..n as usize {
                let ev = events[i];
                let d = ev.data;
                if (d & TAG_PG_BASE) != 0 && d != TAG_LISTEN_0 && d != TAG_LISTEN_1 {
                    let idx = (d & !TAG_PG_BASE) as usize;
                    let can_r = (ev.events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
                    let can_w = (ev.events & EPOLLOUT) != 0;
                    retry_reads.clear();
                    retry_writes.clear();
                    let gz_min = state.core.cfg.gz_min_bytes;
                    let igz = state.core.igzip;

                    {
                        let core = &mut state.core;
                        let lane = &mut state.pg_lanes[idx];
                        lane.handle_io(
                            can_r,
                            can_w,
                            &startup_db_cfg,
                            now_ns,
                            uptime_s,
                            gz_min,
                            igz,
                            &mut retry_reads,
                            &mut retry_writes,
                            &mut |cfd, cgen, close_after, enq_ms, rep| {
                                core.send_reply(
                                    cfd,
                                    cgen,
                                    close_after,
                                    enq_ms,
                                    rep,
                                    now_sec_mono,
                                    now_ms,
                                    now_ns,
                                );
                            },
                        );
                        if lane.phase == PgPhase::Disconnected && now_ns >= lane.next_reconnect_ns {
                            let _ = lane.start_connect(&startup_db_cfg, now_ns, false);
                        }
                        core.sync_pg_lane(idx, lane);
                    }

                    while let Some(w) = retry_writes.pop_back() {
                        state.core.write_queue.push_front(w);
                    }
                    while let Some(r) = retry_reads.pop_back() {
                        state.core.read_queue.push_front(r);
                    }
                }
            }

            for i in 0..n as usize {
                let ev = events[i];
                let d = ev.data;
                if d == TAG_LISTEN_0 {
                    state
                        .core
                        .accept_loop(state.core.lfd0, now_real, now_sec_mono, now_ms, now_ns);
                } else if d == TAG_LISTEN_1 {
                    state
                        .core
                        .accept_loop(state.core.lfd1, now_real, now_sec_mono, now_ms, now_ns);
                } else if (d & TAG_PG_BASE) == 0 {
                    let cfd = (d & 0xFFFF_FFFF) as i32;
                    let cgen = (d >> 32) as u16;
                    if (ev.events & (EPOLLERR | EPOLLHUP)) != 0 {
                        state.core.close_client(cfd);
                        continue;
                    }
                    if (ev.events & EPOLLOUT) != 0 {
                        state.core.flush_partial_client_write(cfd, now_sec_mono);
                    }
                    if (ev.events & (EPOLLIN | EPOLLRDHUP)) != 0 {
                        state.core.handle_client_readable(
                            cfd,
                            cgen,
                            now_real,
                            now_sec_mono,
                            now_ms,
                            now_ns,
                        );
                    }
                }
            }
        }

        state.core.maybe_resume_listeners(now_ns);
        let (_, next_w) = state.dispatch_pg_queues(now_ns, now_sec_mono, now_ms);
        wait_ns = next_w;
        state.core.sweep_idle_connections(now_sec_mono);
    }
}

fn run_worker_uring_loop(state: &mut WorkerState) {
    let total_pg = state.pg_lanes.len();
    let startup_db_cfg = state.core.cfg.db.clone();
    let defer_pg_tx = state.core.uring_pg_enabled;
    let start_ns = monotonic_ns();
    for idx in 0..total_pg {
        if state.pg_lanes[idx].start_connect(&startup_db_cfg, start_ns, defer_pg_tx) {
            state.core.sync_pg_lane(idx, &mut state.pg_lanes[idx]);
        }
    }

    let mut cqe_batch = Vec::with_capacity(4096);
    let mut retry_reads = VecDeque::with_capacity(64);
    let mut retry_writes = VecDeque::with_capacity(64);
    let deadline_ns = start_ns + 400_000_000;

    loop {
        let all_ready = state.pg_lanes.iter().all(|l| l.phase == PgPhase::Ready);
        let now_ns = monotonic_ns();
        if all_ready || now_ns >= deadline_ns {
            break;
        }
        if let Some(ref mut ring) = state.core.uring {
            let _ = ring.submit_and_wait_ns(10_000_000);
            let (mut h, t) = ring.cq_snapshot();
            cqe_batch.clear();
            while h != t {
                cqe_batch.push(ring.cqe_at(h));
                h = h.wrapping_add(1);
            }
            if !cqe_batch.is_empty() {
                ring.advance_cq(h);
            }
        }
        let now_ns = monotonic_ns();
        let now_sec_mono = (now_ns / 1_000_000_000) as u32;
        let now_ms = (now_ns / 1_000_000) as u32;
        let uptime_s = now_ns.saturating_sub(state.core.cfg.boot_ns) / 1_000_000_000;
        let mut recycled_pg_pbuf = false;
        for &cqe in &cqe_batch {
            let tag = cqe.user_data & UTAG_MASK;
            if tag == UTAG_PG_IN
                || tag == UTAG_PG_OUT
                || tag == UTAG_PG_RECV
                || tag == UTAG_PG_SEND
            {
                if state.process_pg_uring_cqe(
                    cqe,
                    &startup_db_cfg,
                    now_ns,
                    now_sec_mono,
                    now_ms,
                    uptime_s,
                    &mut retry_reads,
                    &mut retry_writes,
                ) {
                    recycled_pg_pbuf = true;
                }
            }
        }
        if recycled_pg_pbuf {
            if let Some(ref mut ring) = state.core.uring {
                ring.commit_pg_pbufs();
            }
        }
        if !state.core.pg_enobufs_retry.is_empty() {
            let retries: Vec<(usize, u16)> = state.core.pg_enobufs_retry.drain(..).collect();
            for (idx, rgen) in retries {
                if idx < total_pg
                    && state.pg_lanes[idx].conn_gen == rgen
                    && state.pg_lanes[idx].phase != PgPhase::Disconnected
                {
                    state.core.sync_pg_lane(idx, &mut state.pg_lanes[idx]);
                }
            }
        }
    }

    let lfd0 = match open_listener(
        &state.core.cfg.listen_addr_0,
        state.core.cfg.piggyback_ack,
        state.core.cfg.try_bbr,
    ) {
        Ok(fd) => fd,
        Err(e) => {
            eprintln!("worker {}: {}", state.core.cfg.worker_id, e);
            std::process::exit(1);
        }
    };
    state.core.lfd0 = lfd0;

    if let Some(addr1) = state.core.cfg.listen_addr_1 {
        let lfd1 = match open_listener(
            &addr1,
            state.core.cfg.piggyback_ack,
            state.core.cfg.try_bbr,
        ) {
            Ok(fd) => fd,
            Err(e) => {
                eprintln!("worker {}: {}", state.core.cfg.worker_id, e);
                std::process::exit(1);
            }
        };
        state.core.lfd1 = lfd1;
    }

    let acc_flags = (SOCK_NONBLOCK | SOCK_CLOEXEC) as u32;
    let mut multishot_tick = true;
    if let Some(ref mut ring) = state.core.uring {
        ring.prep_multishot_accept(state.core.lfd0, acc_flags, UTAG_LISTEN_0);
        if state.core.lfd1 >= 0 {
            ring.prep_multishot_accept(state.core.lfd1, acc_flags, UTAG_LISTEN_1);
        }
        ring.prep_timeout(&TICK_TS_1S, multishot_tick, UTAG_TICK);
    }
    state.core.listeners_attached = true;

    let batch_min = state.core.cfg.batch_min;
    let batch_wait_us = state.core.cfg.batch_wait_us;
    let pace_max_us = state.core.cfg.pace_max_us;
    let mut wait_ns: u64 = 250_000_000;
    let mut need_short_timer = false;
    let mut prev_cqes: usize = 0;
    let mut cached_mono_sec: u32 = u32::MAX;
    let mut cached_real_sec: i64 = realtime_sec();

    while !WORKER_STOP.load(Ordering::Relaxed) {
        let total_est = state.core.active_conns * state.core.cfg.num_workers;
        let any_pg_busy = state.pg_lanes.iter().any(|l| l.is_busy());
        let in_burst = state.core.active_conns >= 256
            && (prev_cqes >= 2
                || any_pg_busy
                || !state.core.read_queue.is_empty()
                || !state.core.write_queue.is_empty());

        let eff_batch_wait_us = if batch_wait_us > 0 {
            batch_wait_us
        } else if total_est >= 850_000 {
            150
        } else if total_est >= 250_000 {
            120
        } else {
            0
        };

        let eff_pace_us = if pace_max_us > 0 {
            pace_max_us
        } else if total_est >= 850_000 {
            200
        } else if total_est >= 250_000 {
            100
        } else {
            0
        };

        let (want_min, min_wait_u) =
            if state.core.active_conns >= 256 && batch_min > 1 && eff_batch_wait_us > 0 {
                (batch_min, eff_batch_wait_us)
            } else {
                (1, 0)
            };

        if let Some(ref mut ring) = state.core.uring {
            cqe_batch.clear();
            if !need_short_timer && want_min <= 1 && eff_pace_us == 0 && wait_ns > 0 {
                let _ = ring.submit_and_wait_one_notimer();
                let (mut h, t) = ring.cq_snapshot();
                while h != t && cqe_batch.len() < 8192 {
                    cqe_batch.push(ring.cqe_at(h));
                    h = h.wrapping_add(1);
                }
                if !cqe_batch.is_empty() {
                    ring.advance_cq(h);
                }
            } else {
                if eff_pace_us > 0 && in_burst && wait_ns > 0 && !ring.has_min_timeout {
                    let _ = ring.submit_only();
                    let pace_ns = wait_ns.min((eff_pace_us as u64) * 1_000);
                    if pace_ns >= 15_000 {
                        let ts = Timespec {
                            tv_sec: 0,
                            tv_nsec: pace_ns as i64,
                        };
                        unsafe {
                            nanosleep(&ts, std::ptr::null_mut());
                        }
                    }
                    let _ = ring.submit_and_getevents();
                    let (mut h, t) = ring.cq_snapshot();
                    while h != t && cqe_batch.len() < 8192 {
                        cqe_batch.push(ring.cqe_at(h));
                        h = h.wrapping_add(1);
                    }
                    if !cqe_batch.is_empty() {
                        ring.advance_cq(h);
                    }
                }

                if cqe_batch.is_empty() {
                    let rem_wait = if eff_pace_us > 0 && in_burst && !ring.has_min_timeout {
                        wait_ns.saturating_sub((eff_pace_us as u64) * 1_000)
                    } else {
                        wait_ns
                    };
                    let _ =
                        ring.submit_and_wait_coalesced(rem_wait, want_min, min_wait_u, in_burst);
                    let (mut h, t) = ring.cq_snapshot();
                    while h != t && cqe_batch.len() < 8192 {
                        cqe_batch.push(ring.cqe_at(h));
                        h = h.wrapping_add(1);
                    }
                    if !cqe_batch.is_empty() {
                        ring.advance_cq(h);
                    }
                }
            }
        }

        prev_cqes = cqe_batch.len();
        let now_ns = monotonic_ns();
        let now_sec_mono = (now_ns / 1_000_000_000) as u32;
        let now_ms = (now_ns / 1_000_000) as u32;
        if now_sec_mono != cached_mono_sec {
            cached_mono_sec = now_sec_mono;
            cached_real_sec = realtime_sec();
        }
        let now_real = cached_real_sec;
        let uptime_s = now_ns.saturating_sub(state.core.cfg.boot_ns) / 1_000_000_000;

        if !cqe_batch.is_empty() {
            let mut recycled_pbuf = false;
            let mut recycled_pg_pbuf = false;

            for &cqe in &cqe_batch {
                let ud = cqe.user_data;
                let tag = ud & UTAG_MASK;
                match tag {
                    UTAG_PG_RECV | UTAG_PG_SEND | UTAG_PG_IN | UTAG_PG_OUT => {
                        if state.process_pg_uring_cqe(
                            cqe,
                            &startup_db_cfg,
                            now_ns,
                            now_sec_mono,
                            now_ms,
                            uptime_s,
                            &mut retry_reads,
                            &mut retry_writes,
                        ) {
                            recycled_pg_pbuf = true;
                        }
                    }
                    UTAG_LISTEN_0 => {
                        if cqe.res >= 0 {
                            state.core.uring_accept_one(
                                cqe.res,
                                now_real,
                                now_sec_mono,
                                now_ms,
                                now_ns,
                            );
                        } else {
                            let err = -cqe.res;
                            if err == EMFILE || err == ENFILE || err == ENOBUFS {
                                state.core.pause_listeners(now_ns);
                            }
                        }
                        if (cqe.flags & IORING_CQE_F_MORE) == 0 && state.core.listeners_attached {
                            if let Some(ref mut ring) = state.core.uring {
                                ring.prep_multishot_accept(
                                    state.core.lfd0,
                                    acc_flags,
                                    UTAG_LISTEN_0,
                                );
                            }
                        }
                    }
                    UTAG_LISTEN_1 => {
                        if cqe.res >= 0 {
                            state.core.uring_accept_one(
                                cqe.res,
                                now_real,
                                now_sec_mono,
                                now_ms,
                                now_ns,
                            );
                        } else {
                            let err = -cqe.res;
                            if err == EMFILE || err == ENFILE || err == ENOBUFS {
                                state.core.pause_listeners(now_ns);
                            }
                        }
                        if (cqe.flags & IORING_CQE_F_MORE) == 0
                            && state.core.listeners_attached
                            && state.core.lfd1 >= 0
                        {
                            if let Some(ref mut ring) = state.core.uring {
                                ring.prep_multishot_accept(
                                    state.core.lfd1,
                                    acc_flags,
                                    UTAG_LISTEN_1,
                                );
                            }
                        }
                    }
                    UTAG_CLIENT_POLL => {
                        let cfd = (ud & 0xFFFF_FFFF) as i32;
                        let cgen = ((ud >> 32) & 0xFFFF) as u16;
                        let rev = if cqe.res > 0 { cqe.res as u32 } else { EPOLLERR };
                        let more = (cqe.flags & IORING_CQE_F_MORE) != 0;
                        state.core.handle_uring_client_poll(
                            cfd,
                            cgen,
                            rev,
                            more,
                            now_real,
                            now_sec_mono,
                            now_ms,
                            now_ns,
                        );
                    }
                    UTAG_CLIENT_RECV => {
                        let cfd = (ud & 0xFFFF_FFFF) as i32;
                        let cgen = ((ud >> 32) & 0xFFFF) as u16;
                        if state.core.handle_uring_client_recv(
                            cfd,
                            cgen,
                            cqe.res,
                            cqe.flags,
                            now_real,
                            now_sec_mono,
                            now_ms,
                            now_ns,
                        ) {
                            recycled_pbuf = true;
                        }
                    }
                    UTAG_CLIENT_SEND => {
                        state.core.handle_uring_client_send(
                            ud,
                            cqe.res,
                            now_real,
                            now_sec_mono,
                            now_ms,
                            now_ns,
                        );
                    }
                    UTAG_TICK => {
                        if cqe.res == -22 {
                            multishot_tick = false;
                        }
                        if (cqe.flags & IORING_CQE_F_MORE) == 0 {
                            if let Some(ref mut ring) = state.core.uring {
                                ring.prep_timeout(&TICK_TS_1S, multishot_tick, UTAG_TICK);
                            }
                        }
                    }
                    _ => {}
                }
            }

            if recycled_pbuf {
                if let Some(ref mut ring) = state.core.uring {
                    ring.commit_pbufs();
                }
            }
            if recycled_pg_pbuf {
                if let Some(ref mut ring) = state.core.uring {
                    ring.commit_pg_pbufs();
                }
            }
            if !state.core.enobufs_retry.is_empty() {
                let retries: Vec<(i32, u16)> = state.core.enobufs_retry.drain(..).collect();
                for (rfd, rgen) in retries {
                    state.core.arm_uring_multishot_recv(rfd, rgen);
                }
            }
            if !state.core.pg_enobufs_retry.is_empty() {
                let retries: Vec<(usize, u16)> = state.core.pg_enobufs_retry.drain(..).collect();
                for (idx, rgen) in retries {
                    if idx < total_pg
                        && state.pg_lanes[idx].conn_gen == rgen
                        && state.pg_lanes[idx].phase != PgPhase::Disconnected
                    {
                        state.core.sync_pg_lane(idx, &mut state.pg_lanes[idx]);
                    }
                }
            }
        }

        state.core.maybe_resume_listeners(now_ns);
        let (short_t, mut next_wait) = state.dispatch_pg_queues(now_ns, now_sec_mono, now_ms);
        if cqe_batch.len() == 8192 {
            next_wait = 0;
        }
        need_short_timer = short_t;
        wait_ns = next_wait;
        state.core.sweep_idle_connections(now_sec_mono);
    }

    unsafe {
        if state.core.lfd0 >= 0 {
            close(state.core.lfd0);
        }
        if state.core.lfd1 >= 0 {
            close(state.core.lfd1);
        }
        if let Some(ref ring) = state.core.uring {
            close(ring.ring_fd);
        }
    }
}
