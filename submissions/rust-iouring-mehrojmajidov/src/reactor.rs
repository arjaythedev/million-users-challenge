//! One event loop per CPU. A loop owns its listeners on both ports, its clients and its Postgres connections,
//! so loops share nothing and take no locks. This file is the client side; `db.rs` is the Postgres side.
//!
//! A loop does its I/O through an io_uring of its own (`uring.rs`): the kernel accepts, receives and sends for
//! it, and the loop enters the kernel once per pass. Where the kernel has no io_uring for us (containers, old
//! kernels) the same loop runs on epoll with one system call per receive and per send. The two differ only in
//! the functions marked "ring" and "epoll" below; requests are parsed, routed and answered by the same code.

use crate::db::PgConn;
use crate::gzip::Gzip;
use crate::http::{self, Parsed, Request, Route, Status};
use crate::json::{self, Doc, Value};
use crate::jwt::{AuthError, Verifier};
use crate::pg;
use crate::sys::{self, EpollEvent};
use crate::uring::{Ring, CQE_BUFFER, CQE_MORE};
use core::ffi::c_void;
use std::collections::VecDeque;
use std::mem;
use std::net::IpAddr;

pub struct Config {
    pub pg: pg::PgConfig,
    pub host: IpAddr,
    pub ports: Vec<u16>,
    pub jwt_secret: Vec<u8>,
    /// Postgres connections per loop.
    pub readers: usize,
    pub writers: usize,
    /// A pass of the loop lasts at least this long; see `Loop::run`.
    pub pass_ns: u64,
    pub idle_class: bool,
    pub started_ns: u64,
    /// io_uring for the loops; false runs them on epoll.
    pub ring: bool,
    /// Receive buffers per loop's ring, and whether they go back to the kernel through a shared ring.
    pub ring_buffers: u32,
    pub buffer_ring: bool,
}

const EVENTS: usize = 512;
const READ_BUF: usize = 64 * 1024;
/// Request bytes a connection may have waiting behind the request being served.
const MAX_STASH: usize = http::MAX_HEAD + http::MAX_BODY;
/// Smaller bodies (everything but the feed) would not shrink enough to matter.
const GZIP_MIN: usize = 1024;
/// The kernel leaves delayed-ACK mode when its 40 ms timer fires before the response; see `finish`.
const QUICKACK_REARM_NS: u64 = 30_000_000;

const TAG_LISTENER: u64 = 1 << 32;
pub const TAG_PG: u64 = 2 << 32;

const RING_ENTRIES: u32 = 4096;
const RING_COMPLETIONS: u32 = 32_768;
/// A request (a few hundred bytes) fits many times; a larger one arrives as several completions.
const RING_BUF_SIZE: usize = 2048;
/// A wait for the rest of a pass ends early once this many completions are in.
const RING_FULL: u32 = 4096;
const IDLE_WAIT_NS: u64 = 50_000_000;
// What a completion is about: the top byte of its user data. The rest is a descriptor or a connection index.
const K_ACCEPT: u64 = 1;
const K_RECV: u64 = 2;
const K_SEND: u64 = 3;
pub const K_PG_RECV: u64 = 4;
pub const K_PG_SEND: u64 = 5;
const K_OPTION: u64 = 6;
static QUICKACK_OFF: i32 = 0;

pub const fn tag(kind: u64, id: usize) -> u64 {
    kind << 56 | id as u64
}

const OPEN: u8 = 1;
/// A request of this connection is at Postgres; later requests wait in the stash.
const BUSY: u8 = 2;
const CLOSE_AFTER_FLUSH: u8 = 4;
const SENT_CONTINUE: u8 = 8;
/// Ring: a receive is armed for this descriptor, or waits in `Loop::starved` to be armed again.
const RX: u8 = 16;
/// Ring: a send is in flight for this descriptor; `out` names its `Tx`.
const TX: u8 = 32;
/// Ring: the connection is over, but the ring still holds a request for its descriptor.
const DEAD: u8 = 64;
/// The client has closed its side. It may still be reading, so what it sent before that is answered.
const HALF: u8 = 128;

/// Per-connection state, indexed by file descriptor. 16 bytes, so a million idle connections cost 16 MB;
/// buffers exist only while a request or response is split across reads or writes.
#[repr(C)]
pub struct Conn {
    flags: u8,
    _pad: [u8; 3],
    /// 1-based index into `Loop::bufs`: request bytes not handled yet.
    stash: u32,
    /// Epoll: 1-based index into `Loop::bufs`, the response bytes the socket did not take.
    /// Ring: 1-based index into `Loop::txs`, the response being sent.
    out: u32,
    /// 1-based index into `Loop::ops`: the request at Postgres.
    op: u32,
}

/// A connection's slot. Every access goes through the pointer, so no reference to a slot is held across
/// calls that may change it (closing a connection deep inside a response, for example).
#[derive(Clone, Copy)]
struct Slot(*mut Conn);

impl Slot {
    #[inline]
    fn has(self, bits: u8) -> bool {
        unsafe { (*self.0).flags & bits != 0 }
    }
    #[inline]
    fn set(self, bits: u8) {
        unsafe { (*self.0).flags |= bits }
    }
    #[inline]
    fn clear(self, bits: u8) {
        unsafe { (*self.0).flags &= !bits }
    }
    #[inline]
    fn get(self) -> &'static Conn {
        unsafe { &*self.0 }
    }
    #[inline]
    fn reset(self, flags: u8) {
        unsafe { *self.0 = Conn { flags, _pad: [0; 3], stash: 0, out: 0, op: 0 } }
    }
    #[inline]
    fn set_stash(self, v: u32) {
        unsafe { (*self.0).stash = v }
    }
    #[inline]
    fn set_out(self, v: u32) {
        unsafe { (*self.0).out = v }
    }
    #[inline]
    fn set_op(self, v: u32) {
        unsafe { (*self.0).op = v }
    }
}

/// The table is shared memory, but a slot is only ever touched by the loop that owns that descriptor.
#[derive(Clone, Copy)]
pub struct Table {
    pub slots: *mut Conn,
    pub len: usize,
}
unsafe impl Send for Table {}

impl Table {
    pub fn new(max_fds: usize) -> Table {
        let bytes = max_fds * mem::size_of::<Conn>();
        let p = unsafe {
            sys::mmap(
                core::ptr::null_mut(),
                bytes,
                sys::PROT_READ | sys::PROT_WRITE,
                sys::MAP_PRIVATE | sys::MAP_ANONYMOUS | sys::MAP_NORESERVE,
                -1,
                0,
            )
        };
        assert!(p as isize != -1, "cannot map the connection table");
        Table { slots: p.cast(), len: max_fds }
    }
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Kind {
    Feed,
    Post,
    Health,
    Like,
    Create,
}

impl Kind {
    pub fn is_write(self) -> bool {
        matches!(self, Kind::Like | Kind::Create)
    }
}

/// Ring: a response on its way out. The kernel reads `cur` until the send's completion arrives, so nothing
/// touches it meanwhile; whatever else the connection is to get waits in `next`.
#[derive(Default)]
struct Tx {
    cur: Vec<u8>,
    sent: usize,
    next: Vec<u8>,
}

/// A request that is waiting for Postgres.
pub struct Op {
    pub kind: Kind,
    /// -1 once the client has gone away; the reply is then dropped.
    pub fd: i32,
    pub gzip: bool,
    pub close: bool,
    /// A write whose group failed: it is sent again alone, between two Syncs.
    pub solo: bool,
    pub attempts: u8,
    pub arrived_ns: u64,
    pub id: u64,
    pub user: u64,
    /// Create: the trimmed body, then the token's username from `name_at` on.
    pub text: Vec<u8>,
    pub name_at: usize,
    pub inserted: bool,
    pub new_id: [u8; 20],
    pub new_id_len: u8,
    pub created_at: i64,
    pub sqlstate: [u8; 5],
}

pub struct Loop {
    pub cfg: &'static Config,
    /// -1 when the loop runs on a ring.
    pub ep: i32,
    pub ring: Option<Ring>,
    txs: Vec<Tx>,
    free_txs: Vec<u32>,
    /// Ring: a Postgres receive filled all the room it had, so more is waiting; the next pass starts at once.
    pub hurry: bool,
    /// Ring: socket options can be set through the ring (false once the kernel has refused one).
    ring_options: bool,
    /// Ring: connections whose receive ended because the kernel had no buffer left. They hold bytes the
    /// kernel has not handed over; each is armed again as buffers come back, in this order.
    starved: VecDeque<i32>,
    table: Table,
    pub now_ns: u64,
    rbuf: Vec<u8>,
    head: Vec<u8>,
    gz: Vec<u8>,
    pub scratch: Vec<u8>,
    gzip: Gzip,
    verifier: Verifier,
    pub ops: Vec<Op>,
    free_ops: Vec<u32>,
    bufs: Vec<Vec<u8>>,
    free_bufs: Vec<u32>,
    pub read_q: VecDeque<u32>,
    pub write_q: VecDeque<u32>,
    pub pgs: Vec<PgConn>,
    pub scram: pg::ScramCache,
    pub next_reconnect_ns: u64,
    /// When a read and a write connection were last seen up.
    pub reader_up_ns: u64,
    pub writer_up_ns: u64,
    /// Clients closed during this pass. The descriptors are released at the end of the pass, so a number
    /// cannot be handed out again (to any loop) while this pass still holds events for its previous life.
    closing: Vec<i32>,
}

const NOT_FOUND: &[u8] = br#"{"error":"not found"}"#;
const INVALID_ID: &[u8] = br#"{"error":"invalid post id"}"#;
const MISSING_TOKEN: &[u8] = br#"{"error":"missing bearer token"}"#;
const INVALID_TOKEN: &[u8] = br#"{"error":"invalid or expired token"}"#;
const INVALID_PAYLOAD: &[u8] = br#"{"error":"invalid token payload"}"#;
const MALFORMED_JSON: &[u8] = br#"{"error":"malformed JSON body"}"#;
const BODY_REQUIRED: &[u8] = br#"{"error":"body is required"}"#;
const BODY_TOO_LONG: &[u8] = br#"{"error":"body must be at most 500 characters"}"#;
const BAD_REQUEST: &[u8] = br#"{"error":"bad request"}"#;
const TOO_LARGE: &[u8] = br#"{"error":"request too large"}"#;
pub const POST_NOT_FOUND: &[u8] = br#"{"error":"post not found"}"#;
pub const INTERNAL: &[u8] = br#"{"error":"internal server error"}"#;

/// The body of a new post: valid JSON, an object whose `body` is a string, trimmed as JavaScript trims,
/// 1 to 500 characters (code points, as Postgres's `char_length` counts them).
fn post_body(raw: &[u8]) -> Result<Vec<u8>, &'static [u8]> {
    let Ok(doc) = json::parse_object(raw, &[b"body"]) else {
        return Err(MALFORMED_JSON);
    };
    let Doc::Object(mut members) = doc else {
        return Err(BODY_REQUIRED);
    };
    let Some(Value::Str(text)) = members.pop().flatten() else {
        return Err(BODY_REQUIRED);
    };
    let text = String::from_utf8(text).map_err(|_| MALFORMED_JSON)?;
    let trimmed = json::trim_js(&text);
    if trimmed.is_empty() {
        return Err(BODY_REQUIRED);
    }
    if trimmed.len() > 500 && trimmed.chars().count() > 500 {
        return Err(BODY_TOO_LONG);
    }
    Ok(trimmed.as_bytes().to_vec())
}

fn listener(host: IpAddr, port: u16) -> i32 {
    let family = if host.is_ipv4() { sys::AF_INET } else { sys::AF_INET6 };
    let fd = unsafe { sys::socket(family, sys::SOCK_STREAM | sys::SOCK_NONBLOCK | sys::SOCK_CLOEXEC, 0) };
    assert!(fd >= 0, "socket: errno {}", sys::errno());
    sys::set_opt(fd, sys::SOL_SOCKET, sys::SO_REUSEADDR, 1);
    // Every loop listens on the same ports; the kernel spreads new connections over the loops.
    sys::set_opt(fd, sys::SOL_SOCKET, sys::SO_REUSEPORT, 1);
    // Accepted sockets inherit these two.
    sys::set_opt(fd, sys::IPPROTO_TCP, sys::TCP_NODELAY, 1);
    sys::set_opt(fd, sys::IPPROTO_TCP, sys::TCP_THIN_LINEAR_TIMEOUTS, 1);
    let rc = match host {
        IpAddr::V4(a) => {
            let sa = sys::SockAddrIn { family: sys::AF_INET as u16, port: port.to_be(), addr: a.octets(), zero: [0; 8] };
            unsafe { sys::bind(fd, (&sa as *const sys::SockAddrIn).cast(), mem::size_of::<sys::SockAddrIn>() as u32) }
        }
        IpAddr::V6(a) => {
            let sa = sys::SockAddrIn6 {
                family: sys::AF_INET6 as u16,
                port: port.to_be(),
                flowinfo: 0,
                addr: a.octets(),
                scope_id: 0,
            };
            unsafe { sys::bind(fd, (&sa as *const sys::SockAddrIn6).cast(), mem::size_of::<sys::SockAddrIn6>() as u32) }
        }
    };
    if rc != 0 || unsafe { sys::listen(fd, 65_535) } != 0 {
        eprintln!("feed: cannot listen on {host}:{port}: errno {}", sys::errno());
        std::process::exit(1);
    }
    // A connection is handed over once its first request is in, so one wake-up serves both.
    sys::set_opt(fd, sys::IPPROTO_TCP, sys::TCP_DEFER_ACCEPT, 1);
    // Delayed-ACK mode from the first packet on: the kernel would ACK a request at once, in a packet of its own;
    // this way the response carries the ACK. Set after `listen` (which resets it) and inherited by accepted
    // sockets, so it already holds for the request that arrives before the connection is handed over.
    sys::set_opt(fd, sys::IPPROTO_TCP, sys::TCP_QUICKACK, 0);
    fd
}

impl Loop {
    /// Must run on the thread that will run the loop: a ring belongs to the thread that made it.
    pub fn new(cfg: &'static Config, table: Table, index: usize) -> Loop {
        let ring = if cfg.ring {
            match Ring::new(RING_ENTRIES, RING_COMPLETIONS, cfg.ring_buffers, RING_BUF_SIZE, cfg.buffer_ring) {
                Ok(ring) => {
                    if index == 0 && !ring.has_buffer_ring() {
                        eprintln!("feed: no buffer ring, receive buffers go back to the kernel one request each");
                    }
                    Some(ring)
                }
                Err(e) => {
                    eprintln!("feed: loop {index} runs on epoll, its io_uring could not be made (errno {e})");
                    None
                }
            }
        } else {
            None
        };
        let ep = if ring.is_none() { unsafe { sys::epoll_create1(sys::EPOLL_CLOEXEC) } } else { -1 };
        assert!(ring.is_some() || ep >= 0, "epoll_create1: errno {}", sys::errno());
        let mut lp = Loop {
            cfg,
            ep,
            ring,
            txs: Vec::new(),
            free_txs: Vec::new(),
            hurry: false,
            ring_options: true,
            starved: VecDeque::new(),
            table,
            now_ns: sys::mono_ns(),
            rbuf: vec![0; READ_BUF],
            head: Vec::with_capacity(256),
            gz: Vec::with_capacity(8 * 1024),
            scratch: Vec::with_capacity(4 * 1024),
            gzip: Gzip::new(),
            verifier: Verifier::new(&cfg.jwt_secret),
            ops: Vec::new(),
            free_ops: Vec::new(),
            bufs: Vec::new(),
            free_bufs: Vec::new(),
            read_q: VecDeque::new(),
            write_q: VecDeque::new(),
            pgs: Vec::new(),
            scram: pg::ScramCache::default(),
            next_reconnect_ns: 0,
            reader_up_ns: sys::mono_ns(),
            writer_up_ns: sys::mono_ns(),
            closing: Vec::new(),
        };
        for &port in &cfg.ports {
            let fd = listener(cfg.host, port);
            match lp.ring.as_mut() {
                Some(ring) => ring.accept(fd, sys::SOCK_CLOEXEC as u32, tag(K_ACCEPT, fd as usize)),
                None => lp.watch(sys::EPOLL_CTL_ADD, fd, TAG_LISTENER | fd as u64, sys::EPOLLIN),
            }
        }
        lp.connect_postgres();
        lp
    }

    pub fn watch(&self, op: i32, fd: i32, tag: u64, events: u32) {
        let mut ev = EpollEvent { events, data: tag };
        unsafe { sys::epoll_ctl(self.ep, op, fd, &mut ev) };
    }

    /// Epoll: watches a client for what its state calls for.
    fn rewatch(&self, fd: i32) {
        let c = self.conn(fd);
        let reading = if c.has(HALF) { 0 } else { sys::EPOLLIN | sys::EPOLLRDHUP };
        let writing = if c.get().out != 0 { sys::EPOLLOUT } else { 0 };
        self.watch(sys::EPOLL_CTL_MOD, fd, fd as u64, reading | writing);
    }

    #[inline]
    fn conn(&self, fd: i32) -> Slot {
        debug_assert!((fd as usize) < self.table.len);
        Slot(unsafe { self.table.slots.add(fd as usize) })
    }

    // ---- small pools ----

    pub fn new_op(&mut self, op: Op) -> u32 {
        match self.free_ops.pop() {
            Some(i) => {
                // keep the old allocation of `text` for the next create
                let mut text = mem::take(&mut self.ops[i as usize].text);
                text.clear();
                self.ops[i as usize] = op;
                if self.ops[i as usize].text.is_empty() {
                    self.ops[i as usize].text = text;
                }
                i
            }
            None => {
                self.ops.push(op);
                (self.ops.len() - 1) as u32
            }
        }
    }

    fn new_buf(&mut self) -> u32 {
        match self.free_bufs.pop() {
            Some(i) => i,
            None => {
                self.bufs.push(Vec::new());
                (self.bufs.len() - 1) as u32
            }
        }
    }

    /// Returns a buffer (by its 1-based index, 0 for none) to the pool.
    fn free_buf(&mut self, index: u32) {
        if index != 0 {
            let b = &mut self.bufs[index as usize - 1];
            if b.capacity() > 16 * 1024 {
                *b = Vec::new();
            } else {
                b.clear();
            }
            self.free_bufs.push(index - 1);
        }
    }

    fn new_tx(&mut self) -> u32 {
        match self.free_txs.pop() {
            Some(i) => i,
            None => {
                self.txs.push(Tx::default());
                (self.txs.len() - 1) as u32
            }
        }
    }

    /// Returns a `Tx` (by its 1-based index, 0 for none) to the pool. Its send must have completed.
    fn free_tx(&mut self, index: u32) {
        if index != 0 {
            let tx = &mut self.txs[index as usize - 1];
            for b in [&mut tx.cur, &mut tx.next] {
                if b.capacity() > 16 * 1024 {
                    *b = Vec::new();
                } else {
                    b.clear();
                }
            }
            self.free_txs.push(index - 1);
        }
    }

    // ---- clients ----

    /// Epoll: takes the connections waiting on a listener.
    fn accept_ready(&mut self, lfd: i32) {
        for _ in 0..EVENTS {
            let fd = unsafe {
                sys::accept4(lfd, core::ptr::null_mut(), core::ptr::null_mut(), sys::SOCK_NONBLOCK | sys::SOCK_CLOEXEC)
            };
            if fd < 0 {
                return;
            }
            if fd as usize >= self.table.len {
                unsafe { sys::close(fd) };
                continue;
            }
            self.conn(fd).reset(OPEN);
            self.watch(sys::EPOLL_CTL_ADD, fd, fd as u64, sys::EPOLLIN | sys::EPOLLRDHUP);
            self.client_readable(fd);
        }
    }

    pub fn close_client(&mut self, fd: i32) {
        let c = self.conn(fd);
        if !c.has(OPEN) {
            return;
        }
        let Conn { flags, stash, out, op, .. } = *c.get();
        if op != 0 {
            self.ops[op as usize - 1].fd = -1;
        }
        self.free_buf(stash);
        if self.ring.is_none() {
            self.free_buf(out);
            c.reset(0);
            self.closing.push(fd);
            return;
        }
        let held = flags & (RX | TX);
        if held == 0 {
            c.reset(0);
            unsafe { sys::close(fd) };
            return;
        }
        // The ring still holds a receive or a send for this descriptor. Shutting the socket down completes
        // them; the descriptor is closed when the last one has reported back (`reap`), so its number cannot
        // be handed out again while a completion for its previous life is still to come.
        c.reset(DEAD | held);
        c.set_out(out);
        unsafe { sys::shutdown(fd, sys::SHUT_RDWR) };
    }

    /// Ring: a request of a closed connection has completed. Closes the descriptor once it was the last.
    fn reap(&mut self, fd: i32) {
        let c = self.conn(fd);
        if !c.has(RX | TX) {
            self.free_tx(c.get().out);
            c.reset(0);
            unsafe { sys::close(fd) };
        }
    }

    /// Epoll: reads what a client sent.
    fn client_readable(&mut self, fd: i32) {
        let c = self.conn(fd);
        if !c.has(OPEN) {
            return;
        }
        let mut rbuf = mem::take(&mut self.rbuf);
        let n = unsafe { sys::recv(fd, rbuf.as_mut_ptr().cast(), rbuf.len(), 0) };
        if n <= 0 {
            self.rbuf = rbuf;
            if n == 0 {
                self.client_eof(fd);
            } else if !matches!(sys::errno(), sys::EAGAIN | sys::EINTR) {
                self.close_client(fd);
            }
            return;
        }
        self.client_data(fd, &rbuf[..n as usize]);
        self.rbuf = rbuf;
    }

    /// The client has closed its side; it may still be reading. Whatever it sent before is answered, and the
    /// connection ends once nothing is left to answer or to send (`settle`).
    fn client_eof(&mut self, fd: i32) {
        let c = self.conn(fd);
        if !c.has(OPEN) || c.has(HALF) {
            return;
        }
        c.set(HALF);
        if self.ring.is_none() {
            // level-triggered: the end of the stream would be reported on every pass
            self.rewatch(fd);
        }
        self.settle(fd);
    }

    /// Ends a half-closed connection that has nothing left to answer or to send. Called wherever a
    /// connection may just have become idle.
    fn settle(&mut self, fd: i32) {
        let c = self.conn(fd);
        if c.get().flags & (OPEN | HALF | BUSY) == OPEN | HALF && c.get().out == 0 {
            self.close_client(fd);
        }
    }

    /// Bytes from a client, however they were read.
    fn client_data(&mut self, fd: i32, data: &[u8]) {
        let c = self.conn(fd);
        // While a request is at Postgres, or a response is still waiting for the socket, later requests
        // only pile up (to a limit): a client that never reads cannot make the server buffer its answers.
        if c.get().stash != 0 || c.has(BUSY) || c.get().out != 0 {
            if c.get().stash == 0 {
                c.set_stash(self.new_buf() + 1);
            }
            let stash = &mut self.bufs[c.get().stash as usize - 1];
            stash.extend_from_slice(data);
            if stash.len() > MAX_STASH {
                self.close_client(fd);
            } else if !c.has(BUSY) && c.get().out == 0 {
                self.drain_stash(fd);
            }
        } else {
            let used = self.process(fd, data);
            if used < data.len() && c.has(OPEN) {
                c.set_stash(self.new_buf() + 1);
                self.bufs[c.get().stash as usize - 1].extend_from_slice(&data[used..]);
            }
        }
    }

    /// Serves the requests waiting in a connection's stash, until one has to wait for Postgres.
    fn drain_stash(&mut self, fd: i32) {
        let c = self.conn(fd);
        let index = c.get().stash;
        let mut stash = mem::take(&mut self.bufs[index as usize - 1]);
        let used = self.process(fd, &stash);
        if !c.has(OPEN) {
            // closed meanwhile: the (then empty) buffer already went back to the pool
            return;
        }
        stash.drain(..used);
        let empty = stash.is_empty();
        self.bufs[index as usize - 1] = stash;
        if empty {
            self.free_buf(index);
            c.set_stash(0);
        }
    }

    /// Handles as many whole requests of `data` as it can. Returns the bytes consumed.
    fn process(&mut self, fd: i32, data: &[u8]) -> usize {
        let c = self.conn(fd);
        let mut used = 0;
        while used < data.len() && c.get().flags & (OPEN | BUSY | CLOSE_AFTER_FLUSH) == OPEN && c.get().out == 0 {
            match http::parse(&data[used..]) {
                Parsed::Complete(req, n) => {
                    used += n;
                    c.clear(SENT_CONTINUE);
                    self.handle(fd, &req);
                }
                Parsed::Partial { wants_continue } => {
                    if wants_continue && !c.has(SENT_CONTINUE) {
                        c.set(SENT_CONTINUE);
                        self.write_out(fd, http::CONTINUE, b"");
                    }
                    break;
                }
                Parsed::Bad => {
                    self.send(fd, Status::BadRequest, BAD_REQUEST, false, true);
                    return data.len();
                }
                Parsed::TooLarge => {
                    self.send(fd, Status::TooLarge, TOO_LARGE, false, true);
                    return data.len();
                }
            }
        }
        used
    }

    fn handle(&mut self, fd: i32, req: &Request) {
        match http::route(req.method, req.path) {
            Route::Feed => self.start(fd, req, Kind::Feed, 0, 0, Vec::new(), 0),
            Route::Health => self.start(fd, req, Kind::Health, 0, 0, Vec::new(), 0),
            Route::Post(segment) => match http::post_id(segment) {
                Some(id) => self.start(fd, req, Kind::Post, id, 0, Vec::new(), 0),
                None => self.send(fd, Status::BadRequest, INVALID_ID, false, req.close),
            },
            // On the two routes below the token is checked before anything else about the request.
            Route::Like(segment) => match self.verifier.verify(req.authorization, sys::wall_secs()) {
                Err(e) => self.unauthorized(fd, e, req.close),
                Ok(user) => match http::post_id(segment) {
                    Some(id) => self.start(fd, req, Kind::Like, id, user.id, Vec::new(), 0),
                    None => self.send(fd, Status::BadRequest, INVALID_ID, false, req.close),
                },
            },
            Route::Create => match self.verifier.verify(req.authorization, sys::wall_secs()) {
                Err(e) => self.unauthorized(fd, e, req.close),
                Ok(user) => match post_body(req.body) {
                    Ok(mut text) => {
                        let name_at = text.len();
                        text.extend_from_slice(&user.name);
                        self.start(fd, req, Kind::Create, 0, user.id, text, name_at)
                    }
                    Err(message) => self.send(fd, Status::BadRequest, message, false, req.close),
                },
            },
            Route::NotFound => self.send(fd, Status::NotFound, NOT_FOUND, false, req.close),
        }
    }

    fn unauthorized(&mut self, fd: i32, e: AuthError, close: bool) {
        let body = match e {
            AuthError::Missing => MISSING_TOKEN,
            AuthError::Invalid => INVALID_TOKEN,
            AuthError::Payload => INVALID_PAYLOAD,
        };
        self.send(fd, Status::Unauthorized, body, false, close);
    }

    /// Queues a request for Postgres. It leaves with the other requests of this pass (`db.rs`, `dispatch`).
    #[allow(clippy::too_many_arguments)]
    fn start(&mut self, fd: i32, req: &Request, kind: Kind, id: u64, user: u64, text: Vec<u8>, name_at: usize) {
        let op = self.new_op(Op {
            kind,
            fd,
            gzip: req.gzip,
            close: req.close,
            solo: false,
            attempts: 0,
            arrived_ns: self.now_ns,
            id,
            user,
            text,
            name_at,
            inserted: false,
            new_id: [0; 20],
            new_id_len: 0,
            created_at: 0,
            sqlstate: [0; 5],
        });
        let c = self.conn(fd);
        c.set_op(op + 1);
        c.set(BUSY);
        if kind.is_write() {
            self.write_q.push_back(op);
        } else {
            self.read_q.push_back(op);
        }
    }

    /// Answers the request `op` was waiting for and returns the op to the pool.
    pub fn finish(&mut self, op: u32, status: Status, body: &[u8]) {
        let o = &self.ops[op as usize];
        let (fd, gzip, close, arrived_ns) = (o.fd, o.gzip, o.close, o.arrived_ns);
        self.free_ops.push(op);
        if fd < 0 {
            return;
        }
        let c = self.conn(fd);
        c.set_op(0);
        c.clear(BUSY);
        self.send(fd, status, body, gzip, close);
        if !c.has(OPEN) {
            return;
        }
        if self.now_ns.saturating_sub(arrived_ns) > QUICKACK_REARM_NS {
            // Under overload this is most responses, so on a ring it must not cost a system call each.
            match self.ring.as_mut() {
                Some(ring) if self.ring_options => {
                    ring.set_option(fd, sys::IPPROTO_TCP, sys::TCP_QUICKACK, &QUICKACK_OFF, tag(K_OPTION, fd as usize));
                }
                _ => {
                    sys::set_opt(fd, sys::IPPROTO_TCP, sys::TCP_QUICKACK, 0);
                }
            }
        }
        if c.get().stash != 0 && !c.has(CLOSE_AFTER_FLUSH) {
            self.drain_stash(fd);
        }
        self.settle(fd);
    }

    /// Sends one response: head and body in a single write, so a small response is a single TCP segment.
    fn send(&mut self, fd: i32, status: Status, body: &[u8], gzip_ok: bool, close: bool) {
        let mut head = mem::take(&mut self.head);
        let mut gz = mem::take(&mut self.gz);
        head.clear();
        if gzip_ok && body.len() >= GZIP_MIN {
            gz.clear();
            self.gzip.compress(body, &mut gz);
            http::head(&mut head, status, gz.len(), true, close);
            self.write_out(fd, &head, &gz);
        } else {
            http::head(&mut head, status, body.len(), false, close);
            self.write_out(fd, &head, body);
        }
        self.head = head;
        self.gz = gz;
        let c = self.conn(fd);
        if close && c.has(OPEN) {
            if c.get().out == 0 {
                self.close_client(fd);
            } else {
                c.set(CLOSE_AFTER_FLUSH);
            }
        }
    }

    fn write_out(&mut self, fd: i32, a: &[u8], b: &[u8]) {
        let c = self.conn(fd);
        if !c.has(OPEN) {
            return;
        }
        if self.ring.is_some() {
            return self.ring_write(fd, a, b);
        }
        let mut written = 0;
        if c.get().out == 0 {
            let iov = [
                sys::IoVec { base: a.as_ptr() as *const c_void, len: a.len() },
                sys::IoVec { base: b.as_ptr() as *const c_void, len: b.len() },
            ];
            let n = unsafe { sys::writev(fd, iov.as_ptr(), 2) };
            if n < 0 {
                if !matches!(sys::errno(), sys::EAGAIN | sys::EINTR) {
                    self.close_client(fd);
                    return;
                }
            } else {
                written = n as usize;
            }
            if written == a.len() + b.len() {
                return;
            }
            c.set_out(self.new_buf() + 1);
            self.rewatch(fd);
        }
        let out = &mut self.bufs[c.get().out as usize - 1];
        out.extend_from_slice(&a[written.min(a.len())..]);
        out.extend_from_slice(&b[written.saturating_sub(a.len())..]);
    }

    /// Epoll: the socket takes bytes again.
    fn client_writable(&mut self, fd: i32) {
        let c = self.conn(fd);
        let index = c.get().out;
        if !c.has(OPEN) || index == 0 {
            return;
        }
        let out = &mut self.bufs[index as usize - 1];
        let n = unsafe { sys::write(fd, out.as_ptr().cast(), out.len()) };
        if n < 0 {
            if !matches!(sys::errno(), sys::EAGAIN | sys::EINTR) {
                self.close_client(fd);
            }
            return;
        }
        out.drain(..n as usize);
        if out.is_empty() {
            self.free_buf(index);
            c.set_out(0);
            if c.has(CLOSE_AFTER_FLUSH) {
                self.close_client(fd);
            } else {
                self.rewatch(fd);
                if c.get().stash != 0 && !c.has(BUSY) {
                    self.drain_stash(fd);
                }
                self.settle(fd);
            }
        }
    }

    /// The loop. A pass serves everything that is ready, sends this pass's reads to Postgres as one batch and
    /// its writes as one group, then sleeps out the rest of `pass_ns`. With a million mostly idle connections
    /// a loop would otherwise wake for nearly every request and every reply; with the fixed cadence one
    /// wake-up serves everything that arrived meanwhile, at the cost of a few milliseconds per request.
    /// A pass that fills its event array goes straight on, so the cadence never caps throughput.
    pub fn run(mut self) -> ! {
        if self.cfg.idle_class {
            // The lowest scheduling class for this thread only: on a shared core the kernel's receive work
            // (ksoftirqd) runs first, so the network card's single queue does not overflow behind a busy loop.
            let priority = 0i32;
            if unsafe { sys::sched_setscheduler(0, sys::SCHED_IDLE, &priority) } != 0 {
                eprintln!("feed: SCHED_IDLE not set (errno {})", sys::errno());
            }
        }
        if self.ring.is_some() {
            self.run_ring()
        }
        let mut events = [EpollEvent { events: 0, data: 0 }; EVENTS];
        let mut listeners = Vec::with_capacity(4);
        loop {
            let n = unsafe { sys::epoll_wait(self.ep, events.as_mut_ptr(), EVENTS as i32, 50) };
            let started = sys::mono_ns();
            self.now_ns = started;
            if n < 0 {
                if sys::errno() == sys::EINTR {
                    continue;
                }
                panic!("epoll_wait: errno {}", sys::errno());
            }
            listeners.clear();
            for ev in &events[..n as usize] {
                let (tag, bits) = (ev.data, ev.events);
                let fd = tag as u32 as i32;
                match tag >> 32 {
                    0 => {
                        if bits & (sys::EPOLLERR | sys::EPOLLHUP) != 0 {
                            self.close_client(fd);
                            continue;
                        }
                        if bits & sys::EPOLLOUT != 0 {
                            self.client_writable(fd);
                        }
                        if bits & sys::EPOLLIN != 0 {
                            self.client_readable(fd);
                        } else if bits & sys::EPOLLRDHUP != 0 {
                            self.client_eof(fd);
                        }
                    }
                    1 => listeners.push(fd),
                    _ => self.pg_event(fd as usize, bits),
                }
            }
            // New connections last: a descriptor closed in this pass cannot be handed out again
            // while events for its previous life are still in the array.
            for &lfd in &listeners {
                self.accept_ready(lfd);
            }
            self.dispatch();
            for fd in self.closing.drain(..) {
                unsafe { sys::close(fd) };
            }
            self.timers();
            if (n as usize) < EVENTS {
                let spent = sys::mono_ns() - started;
                if spent < self.cfg.pass_ns {
                    sys::sleep_ns(self.cfg.pass_ns - spent);
                }
            }
        }
    }

    // ---- ring ----

    /// The same passes on a ring. One call into the kernel submits what the pass queued (this pass's responses,
    /// its batch for Postgres, receives to arm) and waits out the rest of `pass_ns`; the completions that are
    /// in by then are the next pass. A pass in which nothing came in is followed by a wait for the first
    /// completion instead, so an idle loop does not tick.
    fn run_ring(mut self) -> ! {
        let (mut min, mut timeout) = (1, IDLE_WAIT_NS);
        // What a paced call takes beyond its wait: the kernel sends on the way in and receives on the way out.
        // Waits are shortened by this (a running average, at most half a pass), so a pass lasts `pass_ns` from
        // wake-up to wake-up here as it does on epoll.
        let mut around_wait = 0;
        let mut entered = sys::mono_ns();
        loop {
            self.ring.as_mut().unwrap().enter(min, timeout);
            let started = sys::mono_ns();
            if min == RING_FULL {
                let over = (started - entered).saturating_sub(timeout);
                around_wait = ((around_wait * 3 + over) / 4).min(self.cfg.pass_ns / 2);
            }
            self.now_ns = started;
            self.hurry = false;
            let mut came_in = 0u32;
            while let Some(c) = self.ring.as_mut().unwrap().pop() {
                let id = c.user_data as u32 as i32;
                match c.user_data >> 56 {
                    K_ACCEPT => {
                        came_in += 1;
                        self.ring_accepted(id, c.res, c.flags & CQE_MORE != 0);
                    }
                    K_RECV => {
                        came_in += 1;
                        self.ring_received(id, c.res, c.flags);
                    }
                    K_SEND => self.ring_sent(id, c.res),
                    K_PG_RECV => {
                        came_in += 1;
                        self.ring_pg_received(id as usize, c.res);
                    }
                    K_PG_SEND => self.ring_pg_sent(id as usize, c.res),
                    // Only a refused request reports back: this kernel sets no socket options through a
                    // ring (before 6.7), so from now on the system call does it. A request for a socket
                    // that has closed meanwhile fails too, which says nothing about the kernel.
                    K_OPTION if c.res == -sys::EOPNOTSUPP || c.res == -sys::EINVAL => self.ring_options = false,
                    _ => {}
                }
            }
            self.dispatch();
            self.timers();
            self.ring_feed();
            entered = sys::mono_ns();
            let spent = entered - started + around_wait;
            (min, timeout) = if self.hurry || self.ring.as_ref().unwrap().backlogged() {
                (0, 0)
            } else if came_in == 0 || self.cfg.pass_ns == 0 {
                (1, IDLE_WAIT_NS)
            } else if spent < self.cfg.pass_ns {
                (RING_FULL, self.cfg.pass_ns - spent)
            } else {
                (0, 0)
            };
        }
    }

    fn ring_accepted(&mut self, lfd: i32, res: i32, more: bool) {
        if res >= 0 {
            let fd = res;
            if fd as usize >= self.table.len {
                unsafe { sys::close(fd) };
            } else {
                self.conn(fd).reset(OPEN);
                // The listener hands a connection over with its first request already in (TCP_DEFER_ACCEPT).
                // Reading it here, once per connection, serves it in this pass instead of the next.
                let mut rbuf = mem::take(&mut self.rbuf);
                let n = unsafe { sys::recv(fd, rbuf.as_mut_ptr().cast(), rbuf.len(), sys::MSG_DONTWAIT) };
                if n > 0 {
                    self.client_data(fd, &rbuf[..n as usize]);
                }
                self.rbuf = rbuf;
                if self.conn(fd).has(OPEN) {
                    self.ring_arm(fd);
                }
            }
        }
        if !more {
            // the kernel ended the multishot accept (no descriptors left, say): ask again
            self.ring.as_mut().unwrap().accept(lfd, sys::SOCK_CLOEXEC as u32, tag(K_ACCEPT, lfd as usize));
        }
    }

    fn ring_arm(&mut self, fd: i32) {
        self.conn(fd).set(RX);
        self.ring.as_mut().unwrap().receive_provided(fd, tag(K_RECV, fd as usize));
    }

    /// End of a pass: every buffer the pass was handed is back with the kernel, so that many starved
    /// connections can receive again. Each has bytes waiting and takes a buffer the moment it is armed; arming
    /// more than there are buffers would only fail again.
    fn ring_feed(&mut self) {
        for _ in 0..self.cfg.ring_buffers {
            let Some(fd) = self.starved.pop_front() else { return };
            let c = self.conn(fd);
            if c.has(DEAD) {
                c.clear(RX);
                self.reap(fd);
            } else {
                self.ring.as_mut().unwrap().receive_provided(fd, tag(K_RECV, fd as usize));
            }
            // what they hold is overdue: the next pass starts at once
            self.hurry = true;
        }
    }

    fn ring_received(&mut self, fd: i32, res: i32, flags: u32) {
        let c = self.conn(fd);
        if flags & CQE_BUFFER != 0 {
            let bid = (flags >> 16) as u16;
            if res > 0 && c.has(OPEN) {
                // The buffer is the kernel's again only after `recycle`, and nothing below keeps the slice.
                let data = unsafe { self.ring.as_ref().unwrap().buffer(bid, res as usize) };
                self.client_data(fd, data);
            }
            self.ring.as_mut().unwrap().recycle(bid);
        }
        if flags & CQE_MORE != 0 {
            return;
        }
        // The receive is over: the client closed (0), the socket failed, or the kernel stopped it for a reason
        // of its own and it only needs to be armed again: at once after a full completion queue, in its turn
        // (`ring_feed`) when no buffer was left.
        if res == -sys::ENOBUFS {
            return self.starved.push_back(fd);
        }
        c.clear(RX);
        if c.has(OPEN) {
            if res > 0 {
                self.ring_arm(fd);
            } else if res == 0 {
                self.client_eof(fd);
            } else {
                self.close_client(fd);
            }
        } else if c.has(DEAD) {
            self.reap(fd);
        }
    }

    fn ring_write(&mut self, fd: i32, a: &[u8], b: &[u8]) {
        let c = self.conn(fd);
        if c.has(TX) {
            let next = &mut self.txs[c.get().out as usize - 1].next;
            next.extend_from_slice(a);
            next.extend_from_slice(b);
            return;
        }
        let t = self.new_tx();
        let tx = &mut self.txs[t as usize];
        tx.cur.extend_from_slice(a);
        tx.cur.extend_from_slice(b);
        tx.sent = 0;
        let (at, len) = (tx.cur.as_ptr(), tx.cur.len());
        c.set_out(t + 1);
        c.set(TX);
        self.ring.as_mut().unwrap().send(fd, at, len, tag(K_SEND, fd as usize));
    }

    fn ring_sent(&mut self, fd: i32, res: i32) {
        let c = self.conn(fd);
        if c.has(DEAD) {
            c.clear(TX);
            return self.reap(fd);
        }
        let index = c.get().out;
        let tx = &mut self.txs[index as usize - 1];
        if res <= 0 {
            self.free_tx(index);
            c.set_out(0);
            c.clear(TX);
            return self.close_client(fd);
        }
        tx.sent += res as usize;
        if tx.sent >= tx.cur.len() {
            if tx.next.is_empty() {
                self.free_tx(index);
                c.set_out(0);
                c.clear(TX);
                if c.has(CLOSE_AFTER_FLUSH) {
                    self.close_client(fd);
                } else {
                    if c.get().stash != 0 && !c.has(BUSY) {
                        self.drain_stash(fd);
                    }
                    self.settle(fd);
                }
                return;
            }
            mem::swap(&mut tx.cur, &mut tx.next);
            tx.next.clear();
            tx.sent = 0;
        }
        // What was queued behind the send, or the rest after a short count (the socket failed part-way; the
        // retry reports why).
        let (at, len) = (unsafe { tx.cur.as_ptr().add(tx.sent) }, tx.cur.len() - tx.sent);
        self.ring.as_mut().unwrap().send(fd, at, len, tag(K_SEND, fd as usize));
    }
}
