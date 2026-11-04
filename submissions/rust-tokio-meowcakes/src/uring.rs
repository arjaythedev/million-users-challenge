use std::cell::{Cell, RefCell};
use std::io;
use std::os::fd::{AsRawFd, RawFd};
use std::sync::atomic::{AtomicBool, AtomicU16, Ordering};
use std::time::{Duration, Instant};

use io_uring::{IoUring, cqueue, opcode, squeue, types};

use crate::Worker;
use crate::db::{self, Waiter};
use crate::http::{self, Flags, ReqState};

const BGID: u16 = 0;
const BUF_SIZE: usize = 2048;
const BUF_COUNT: u16 = 8192;
const CQE_BATCH: usize = 4096;
const MAX_CARRY: usize = 64 * 1024;

const OP_ACCEPT: u64 = 1;
const OP_RECV: u64 = 2;
const OP_SEND: u64 = 3;
const OP_PG_POLL: u64 = 4;
const OP_IGNORE: u64 = 5;
const OP_RESERVE: u64 = 6;

const IORING_REGISTER_PBUF_RING: libc::c_uint = 22;
const SOCKET_URING_OP_SETSOCKOPT: u32 = 3;

const LATE_MS: u32 = 25;
static ZERO: libc::c_int = 0;

const IDLE_WAIT: Duration = Duration::from_millis(10);

fn user_data(op: u64, gen_: u32, idx: u32) -> u64 {
    op << 56 | (gen_ as u64 & 0xFF_FFFF) << 32 | idx as u64
}

#[derive(Clone, Copy)]
pub struct Target {
    idx: u32,
    gen_: u32,
}

pub struct Ring {
    ring: RefCell<IoUring>,
    batch_wait: Duration,
    batch_min: usize,
    reserve_mem: libc::c_int,
    reserve_max: usize,
    reserved: Cell<usize>,
    started: Instant,
    now_ms: Cell<u32>,
    conns: RefCell<Conns>,
    bufs: Bufs,
    watched: RefCell<Vec<RawFd>>,
}

#[derive(Default)]
struct Conns {
    slots: Vec<Conn>,
    free: Vec<u32>,
}

#[derive(Default)]
struct Conn {
    file: u32,
    gen_: u32,
    live: bool,
    reserved: bool,
    req_at: u32,
    recv_armed: bool,
    sending: bool,
    waiting: bool,
    close_after: bool,
    eof: bool,
    req: ReqState,
    flags: Flags,
    carry: Vec<u8>,
    out: Vec<u8>,
    inflight: Vec<u8>,
    sent: usize,
}

impl Conns {
    fn alloc(&mut self, file: u32) -> (u32, u32) {
        let idx = self.free.pop().unwrap_or_else(|| {
            self.slots.push(Conn::default());
            (self.slots.len() - 1) as u32
        });
        let c = &mut self.slots[idx as usize];
        c.file = file;
        c.live = true;
        (idx, c.gen_)
    }

    fn get(&mut self, idx: u32, gen_: u32) -> Option<&mut Conn> {
        self.slots
            .get_mut(idx as usize)
            .filter(|c| c.live && c.gen_ == gen_)
    }

    fn free(&mut self, idx: u32) {
        let c = &mut self.slots[idx as usize];
        *c = Conn {
            gen_: (c.gen_ + 1) & 0xFF_FFFF,
            ..Conn::default()
        };
        self.free.push(idx);
    }
}

struct Bufs {
    ring: *mut types::BufRingEntry,
    mem: *mut u8,
    tail: Cell<u16>,
}

impl Bufs {
    fn new() -> Bufs {
        let layout = std::alloc::Layout::from_size_align(BUF_COUNT as usize * 16, 4096).unwrap();
        let ring = unsafe { std::alloc::alloc_zeroed(layout) } as *mut types::BufRingEntry;
        assert!(!ring.is_null());
        let mem =
            Box::leak(vec![0u8; BUF_COUNT as usize * BUF_SIZE].into_boxed_slice()).as_mut_ptr();
        let bufs = Bufs {
            ring,
            mem,
            tail: Cell::new(0),
        };
        for bid in 0..BUF_COUNT {
            bufs.put(bid);
        }
        bufs.publish();
        bufs
    }

    fn put(&self, bid: u16) {
        let t = self.tail.get();
        let e = unsafe { &mut *self.ring.add((t & (BUF_COUNT - 1)) as usize) };
        e.set_addr(self.mem as u64 + bid as u64 * BUF_SIZE as u64);
        e.set_len(BUF_SIZE as u32);
        e.set_bid(bid);
        self.tail.set(t.wrapping_add(1));
    }

    fn publish(&self) {
        let tail = unsafe { AtomicU16::from_ptr(types::BufRingEntry::tail(self.ring) as *mut u16) };
        tail.store(self.tail.get(), Ordering::Release);
    }

    fn get(&self, bid: u16, len: usize) -> &[u8] {
        unsafe {
            std::slice::from_raw_parts(self.mem.add(bid as usize * BUF_SIZE), len.min(BUF_SIZE))
        }
    }
}

#[repr(C)]
struct BufReg {
    ring_addr: u64,
    ring_entries: u32,
    bgid: u16,
    flags: u16,
    resv: [u64; 3],
}

fn register_bufs(ring_fd: RawFd, ring_addr: u64) -> io::Result<bool> {
    for resv0 in [0, 1] {
        let reg = BufReg {
            ring_addr,
            ring_entries: BUF_COUNT as u32,
            bgid: BGID,
            flags: 0,
            resv: [resv0, 0, 0],
        };
        let r = unsafe {
            libc::syscall(
                libc::SYS_io_uring_register,
                ring_fd,
                IORING_REGISTER_PBUF_RING,
                &reg as *const BufReg,
                1,
            )
        };
        if r >= 0 {
            return Ok(resv0 == 1);
        }
        let e = io::Error::last_os_error();
        if resv0 == 1 || e.raw_os_error() != Some(libc::EINVAL) {
            return Err(e);
        }
    }
    unreachable!()
}

fn file_table_size(threads: usize) -> u32 {
    let mut rl = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl) };
    ((4usize << 20) / threads.max(1))
        .clamp(1 << 16, 1 << 20)
        .min(rl.rlim_cur as usize) as u32
}

fn reserve_max(bytes: libc::c_int, threads: usize) -> usize {
    let pages: usize = std::fs::read_to_string("/proc/sys/net/ipv4/tcp_mem")
        .ok()
        .and_then(|s| s.split_whitespace().next()?.parse().ok())
        .unwrap_or(0);
    if bytes <= 0 {
        0
    } else {
        pages * 4096 / 2 / bytes as usize / threads.max(1)
    }
}

fn setsockopt(
    target: types::Fixed,
    level: libc::c_int,
    name: libc::c_int,
    val: *const libc::c_int,
) -> squeue::Entry {
    const { assert!(std::mem::size_of::<squeue::Entry>() == 64) };
    let e = opcode::UringCmd16::new(target, SOCKET_URING_OP_SETSOCKOPT).build();
    let mut raw: [u8; 64] = unsafe { std::mem::transmute(e) };
    raw[16..20].copy_from_slice(&(level as u32).to_ne_bytes());
    raw[20..24].copy_from_slice(&(name as u32).to_ne_bytes());
    raw[44..48].copy_from_slice(&4u32.to_ne_bytes());
    raw[48..56].copy_from_slice(&(val as u64).to_ne_bytes());
    unsafe { std::mem::transmute::<[u8; 64], squeue::Entry>(raw) }
}

impl Ring {
    pub fn new(cfg: &crate::Config) -> io::Result<Ring> {
        let ring = IoUring::builder()
            .setup_cqsize(65536)
            .setup_defer_taskrun()
            .setup_single_issuer()
            .setup_taskrun_flag()
            .build(4096)?;
        let bufs = Bufs::new();
        if register_bufs(ring.as_raw_fd(), bufs.ring as u64)? {
            static LOGGED: AtomicBool = AtomicBool::new(false);
            if !LOGGED.swap(true, Ordering::Relaxed) {
                eprintln!(
                    "io_uring: buffer ring registered with resv[0] = 1 (Ubuntu 6.8.0-1xx kernel)"
                );
            }
        }
        ring.submitter()
            .register_files_sparse(file_table_size(cfg.threads))?;
        Ok(Ring {
            ring: RefCell::new(ring),
            batch_wait: cfg.batch_wait,
            batch_min: cfg.batch_min,
            reserve_mem: cfg.reserve_mem,
            reserve_max: reserve_max(cfg.reserve_mem, cfg.threads),
            reserved: Cell::new(0),
            started: Instant::now(),
            now_ms: Cell::new(0),
            conns: RefCell::new(Conns::default()),
            bufs,
            watched: RefCell::new(Vec::new()),
        })
    }

    pub fn watch(&self, fd: RawFd) {
        self.watched.borrow_mut().push(fd);
        let e = opcode::PollAdd::new(types::Fd(fd), libc::POLLIN as u32)
            .multi(true)
            .build();
        self.push(e.user_data(user_data(OP_PG_POLL, 0, fd as u32)));
    }

    pub fn unwatch(&self, fd: RawFd) {
        self.watched.borrow_mut().retain(|&f| f != fd);
        let e = opcode::AsyncCancel::new(user_data(OP_PG_POLL, 0, fd as u32)).build();
        self.push(e.user_data(user_data(OP_IGNORE, 0, 0)));
    }

    fn push(&self, e: squeue::Entry) {
        let mut ring = self.ring.borrow_mut();
        while unsafe { ring.submission().push(&e) }.is_err() {
            let _ = ring.submit();
        }
    }

    fn pass(
        &'static self,
        w: &'static Worker,
        listeners: &[std::net::TcpListener],
        wait: Option<(Duration, usize)>,
        cqes: &mut Vec<(u64, i32, u32)>,
    ) -> usize {
        self.now_ms.set(self.started.elapsed().as_millis() as u32);
        {
            let mut ring = self.ring.borrow_mut();
            let sq = ring.submission();
            let (queued, pending) = (sq.len(), sq.taskrun() || sq.cq_overflow());
            drop(sq);
            let res = if let Some((t, min)) = wait {
                let ts = types::Timespec::new()
                    .sec(t.as_secs())
                    .nsec(t.subsec_nanos());
                ring.submitter()
                    .submit_with_args(min + queued, &types::SubmitArgs::new().timespec(&ts))
            } else if queued > 0 || pending {
                ring.submit()
            } else {
                Ok(0)
            };
            if let Err(e) = res
                && !matches!(
                    e.raw_os_error(),
                    Some(libc::ETIME | libc::EINTR | libc::EAGAIN | libc::EBUSY)
                )
            {
                eprintln!("io_uring_enter: {e}");
            }
        }
        let mut handled = 0;
        loop {
            cqes.clear();
            cqes.extend(
                self.ring
                    .borrow_mut()
                    .completion()
                    .take(CQE_BATCH)
                    .map(|e| (e.user_data(), e.result(), e.flags())),
            );
            if cqes.is_empty() {
                break;
            }
            handled += cqes.len();
            for &(ud, res, flags) in cqes.iter() {
                let (idx, gen_) = (ud as u32, (ud >> 32) as u32 & 0xFF_FFFF);
                match ud >> 56 {
                    OP_ACCEPT => self.on_accept(
                        idx,
                        listeners[idx as usize].as_raw_fd(),
                        res,
                        cqueue::more(flags),
                    ),
                    OP_RECV => self.on_recv(w, idx, gen_, res, flags),
                    OP_SEND => self.on_send(w, idx, res),
                    OP_RESERVE => self.on_reserve_failed(idx, gen_, res),
                    OP_PG_POLL
                        if !cqueue::more(flags)
                            && self.watched.borrow().contains(&(idx as RawFd)) =>
                    {
                        let e = opcode::PollAdd::new(types::Fd(idx as RawFd), libc::POLLIN as u32)
                            .multi(true)
                            .build();
                        self.push(e.user_data(ud));
                    }
                    _ => {}
                }
            }
            self.bufs.publish();
        }
        handled
    }

    fn accept(&self, listener: RawFd, li: u32) {
        let e = opcode::AcceptMulti::new(types::Fd(listener))
            .allocate_file_index(true)
            .build();
        self.push(e.user_data(user_data(OP_ACCEPT, 0, li)));
    }

    fn recv(&self, file: u32, gen_: u32, idx: u32) {
        let e = opcode::RecvMulti::new(types::Fixed(file), BGID).build();
        self.push(e.user_data(user_data(OP_RECV, gen_, idx)));
    }

    fn send(&self, c: &Conn, idx: u32, from: usize) {
        let data = &c.inflight[from..];
        let e = opcode::Send::new(types::Fixed(c.file), data.as_ptr(), data.len() as u32)
            .flags(libc::MSG_NOSIGNAL)
            .build();
        self.push(e.user_data(user_data(OP_SEND, c.gen_, idx)));
    }

    fn on_reserve_failed(&self, idx: u32, gen_: u32, res: i32) {
        if let Some(c) = self
            .conns
            .borrow_mut()
            .get(idx, gen_)
            .filter(|c| c.reserved)
        {
            c.reserved = false;
            self.reserved.set(self.reserved.get() - 1);
        }
        static LOGGED: AtomicBool = AtomicBool::new(false);
        if !LOGGED.swap(true, Ordering::Relaxed) {
            eprintln!("SO_RESERVE_MEM: {}", io::Error::from_raw_os_error(-res));
        }
    }

    fn settle(&self, w: &Worker, conns: &mut Conns, idx: u32) {
        let c = &mut conns.slots[idx as usize];
        if !c.live {
            return;
        }
        if !c.sending && !c.out.is_empty() {
            c.inflight = std::mem::take(&mut c.out);
            c.sent = 0;
            c.sending = true;
            self.send(c, idx, 0);
        } else if c.out.capacity() > 0 && c.out.is_empty() {
            w.give_buf(std::mem::take(&mut c.out));
        }
        if (c.close_after || c.eof) && !c.sending && !c.waiting {
            self.close(w, conns, idx);
        }
    }

    fn close(&self, w: &Worker, conns: &mut Conns, idx: u32) {
        let c = &mut conns.slots[idx as usize];
        if !c.live {
            return;
        }
        c.live = false;
        let skip = squeue::Flags::SKIP_SUCCESS;
        if c.recv_armed {
            let e = opcode::AsyncCancel::new(user_data(OP_RECV, c.gen_, idx)).build();
            self.push(
                e.flags(skip | squeue::Flags::IO_HARDLINK)
                    .user_data(user_data(OP_IGNORE, 0, 0)),
            );
        }
        self.push(
            opcode::Close::new(types::Fixed(c.file))
                .build()
                .flags(skip)
                .user_data(user_data(OP_IGNORE, 0, 0)),
        );
        if c.reserved {
            c.reserved = false;
            self.reserved.set(self.reserved.get() - 1);
        }
        w.give_buf(std::mem::take(&mut c.out));
        c.carry = Vec::new();
        if !c.sending {
            conns.free(idx);
        }
    }

    fn on_accept(&'static self, li: u32, listener: RawFd, res: i32, more: bool) {
        if res >= 0 {
            let file = res as u32;
            let mut conns = self.conns.borrow_mut();
            let (idx, gen_) = conns.alloc(file);
            let c = &mut conns.slots[idx as usize];
            if self.reserved.get() < self.reserve_max {
                c.reserved = true;
                self.reserved.set(self.reserved.get() + 1);
                let e = setsockopt(
                    types::Fixed(file),
                    libc::SOL_SOCKET,
                    libc::SO_RESERVE_MEM,
                    &self.reserve_mem,
                );
                self.push(
                    e.flags(squeue::Flags::SKIP_SUCCESS)
                        .user_data(user_data(OP_RESERVE, gen_, idx)),
                );
            }
            c.recv_armed = true;
            self.recv(file, gen_, idx);
        } else if matches!(
            -res,
            libc::EMFILE | libc::ENFILE | libc::ENOMEM | libc::ENOBUFS
        ) {
            eprintln!("accept: {}", io::Error::from_raw_os_error(-res));
            if !more {
                tokio::task::spawn_local(async move {
                    tokio::time::sleep(Duration::from_millis(50)).await;
                    self.accept(listener, li);
                });
            }
            return;
        }
        if !more {
            self.accept(listener, li);
        }
    }

    fn on_recv(&self, w: &'static Worker, idx: u32, gen_: u32, res: i32, flags: u32) {
        let bid = cqueue::buffer_select(flags);
        let mut conns = self.conns.borrow_mut();
        let Some(c) = conns.get(idx, gen_) else {
            if let Some(b) = bid {
                self.bufs.put(b)
            }
            return;
        };
        if !cqueue::more(flags) {
            c.recv_armed = false;
        }
        if res > 0 {
            let bid = bid.expect("receive completion without a buffer");
            self.input(w, c, Target { idx, gen_ }, self.bufs.get(bid, res as usize));
            self.bufs.put(bid);
        } else if res == 0 {
            c.eof = true;
        } else if res != -libc::ENOBUFS {
            if let Some(b) = bid {
                self.bufs.put(b)
            }
            self.close(w, &mut conns, idx);
            return;
        }
        let c = &mut conns.slots[idx as usize];
        if c.live && !c.recv_armed && !c.close_after && !c.eof {
            c.recv_armed = true;
            self.recv(c.file, gen_, idx);
        }
        self.settle(w, &mut conns, idx);
    }

    fn on_send(&self, w: &Worker, idx: u32, res: i32) {
        let mut conns = self.conns.borrow_mut();
        let c = &mut conns.slots[idx as usize];
        c.sending = false;
        if !c.live {
            w.give_buf(std::mem::take(&mut c.inflight));
            conns.free(idx);
            return;
        }
        if res < 0 {
            w.give_buf(std::mem::take(&mut c.inflight));
            self.close(w, &mut conns, idx);
            return;
        }
        c.sent += res as usize;
        if c.sent < c.inflight.len() {
            c.sending = true;
            self.send(c, idx, c.sent);
            return;
        }
        w.give_buf(std::mem::take(&mut c.inflight));
        self.settle(w, &mut conns, idx);
    }

    fn input(&self, w: &'static Worker, c: &mut Conn, t: Target, data: &[u8]) {
        if c.close_after {
            return;
        }
        if c.waiting || !c.carry.is_empty() {
            if c.carry.len() + data.len() > MAX_CARRY {
                c.carry = Vec::new();
                c.close_after = true;
                return;
            }
            c.carry.extend_from_slice(data);
            if !c.waiting {
                self.pump(w, c, t);
            }
            return;
        }
        let used = self.handle(w, c, t, data);
        if used < data.len() && !c.close_after {
            c.carry.extend_from_slice(&data[used..]);
        }
    }

    fn pump(&self, w: &'static Worker, c: &mut Conn, t: Target) {
        let mut carry = std::mem::take(&mut c.carry);
        let used = self.handle(w, c, t, &carry);
        if used < carry.len() && !c.close_after {
            carry.drain(..used);
            c.carry = carry;
        }
    }

    fn handle(&self, w: &'static Worker, c: &mut Conn, t: Target, data: &[u8]) -> usize {
        if c.out.capacity() == 0 {
            c.out = w.take_buf();
        }
        let (used, db_request) = http::handle(w, &mut c.req, data, &mut c.out);
        c.close_after |= c.req.closing;
        if let Some((flags, q)) = db_request {
            c.waiting = true;
            c.flags = flags;
            c.req_at = self.now_ms.get();
            db::enqueue(w, q, Waiter::Conn(t));
        }
        used
    }
}

pub fn start(w: &'static Worker, r: &'static Ring, listeners: Vec<std::net::TcpListener>) {
    let listeners: &'static [std::net::TcpListener] = listeners.leak();
    for (li, l) in listeners.iter().enumerate() {
        r.accept(l.as_raw_fd(), li as u32);
    }
    tokio::task::spawn_local(driver(w, r, listeners));
}

async fn driver(w: &'static Worker, r: &'static Ring, listeners: &'static [std::net::TcpListener]) {
    let mut cqes: Vec<(u64, i32, u32)> = Vec::with_capacity(CQE_BATCH);
    let mut busy = false;
    loop {
        let wait = match busy {
            true => (!r.batch_wait.is_zero()).then_some((r.batch_wait, r.batch_min)),
            false => Some((IDLE_WAIT, 1)),
        };
        busy = r.pass(w, listeners, wait, &mut cqes) > 0;
        tokio::task::yield_now().await;
    }
}

pub fn db_reply(w: &'static Worker, t: Target, status: u16, body: Vec<u8>) {
    let r = w.ring.expect("ring connection without a ring");
    let mut conns = r.conns.borrow_mut();
    let Some(c) = conns.get(t.idx, t.gen_) else {
        w.give_buf(body);
        return;
    };
    c.waiting = false;
    if r.now_ms.get().wrapping_sub(c.req_at) >= LATE_MS {
        let e = setsockopt(
            types::Fixed(c.file),
            libc::IPPROTO_TCP,
            libc::TCP_QUICKACK,
            &ZERO,
        );
        r.push(
            e.flags(squeue::Flags::SKIP_SUCCESS)
                .user_data(user_data(OP_IGNORE, 0, 0)),
        );
    }
    if c.out.capacity() == 0 {
        c.out = w.take_buf();
    }
    http::respond(w, &mut c.out, status, &body, c.flags);
    w.give_buf(body);
    c.close_after |= c.flags.close;
    if !c.close_after && !c.carry.is_empty() {
        r.pump(w, c, t);
    }
    r.settle(w, &mut conns, t.idx);
}
