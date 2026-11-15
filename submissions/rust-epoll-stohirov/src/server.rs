use crate::gzip::Gzip;
use crate::http::{self, Method, Parsed, Req};
use crate::json::{self, Kind, Slot};
use crate::jwt::{Auth, Jwt};
use crate::pg::{self, DbConfig};
use crate::sys;
use std::collections::VecDeque;

pub struct Config {
    pub readers: usize,
    pub writers: usize,
    pub pack_max: usize,
    pub hold_ns: u64,
    pub gzip_min: usize,
    pub start_ns: u64,
    pub pass_ns: u64,
    pub edge: bool,
    pub thin: bool,
    pub sched_idle: bool,
    pub timer_slack_ns: u64,
    pub probe_ns: u64,
}

const TAG_LISTEN: u64 = 1 << 40;
const TAG_PG: u64 = 2 << 40;

const READ_CAP: usize = 64 * 1024;
const MAX_PENDING: usize = 64 * 1024;
const HEADROOM: usize = 256;
const MAX_EVENTS: usize = 1024;
const ACCEPT_BURST: usize = 256;
const MAX_GROUP: usize = 512;
const MAX_IN_FLIGHT: u32 = 2;
const LATE_ACK_MS: u32 = 25;
const RECONNECT_NS: u64 = 1_000_000_000;
const IDLE_WAIT_NS: i64 = 1_000_000_000;

const OPEN: u32 = 1;
const BUSY: u32 = 2;
const CLOSE_AFTER: u32 = 4;
const GZIP: u32 = 8;
const CLOSING: u32 = 16;
const WANT_OUT: u32 = 32;
const PAUSED: u32 = 64;
const PEER_DONE: u32 = 128;

#[repr(C)]
pub struct Conn {
    flags: u32,
    since_ms: u32,
    out_from: u32,
    _pad: u32,
    inb: Option<Box<Vec<u8>>>,
    outb: Option<Box<Vec<u8>>>,
}

#[derive(Clone, Copy)]
pub struct Table {
    ptr: *mut Conn,
    len: usize,
}

unsafe impl Send for Table {}

impl Table {
    pub fn new(len: usize) -> Table {
        Table { ptr: sys::map_zeroed(len * std::mem::size_of::<Conn>()) as *mut Conn, len }
    }
}

#[derive(Clone, Copy)]
struct Waiter {
    fd: i32,
    kind: u8,
    last: bool,
    id: i64,
}

struct Job {
    fd: i32,
    kind: u8,
    inserted: bool,
    user_id: i64,
    post_id: i64,
    new_id: i64,
    ts: i64,
    body: Vec<u8>,
    user: Vec<u8>,
}

struct Pg {
    fd: i32,
    epoch: u32,
    out: Vec<u8>,
    sent: usize,
    sealed: usize,
    want_write: bool,
    inb: Vec<u8>,
    waiters: VecDeque<Waiter>,
    open: usize,
    open_since: u64,
    in_flight: u32,
    body: Vec<u8>,
    rows: u32,
    err: Option<([u8; 5], String)>,
    group: Vec<u32>,
    k: usize,
    failed_at: Option<usize>,
    last_error: String,
    syncs: VecDeque<bool>,
    last_rx: u64,
    probes: u32,
    last_probe: u64,
}

impl Pg {
    fn new() -> Pg {
        let mut body = Vec::with_capacity(64 * 1024);
        body.resize(HEADROOM, 0);
        Pg {
            fd: -1,
            epoch: 0,
            out: Vec::with_capacity(64 * 1024),
            sent: 0,
            sealed: 0,
            want_write: false,
            inb: Vec::with_capacity(256 * 1024),
            waiters: VecDeque::with_capacity(256),
            open: 0,
            open_since: 0,
            in_flight: 0,
            body,
            rows: 0,
            err: None,
            group: Vec::with_capacity(MAX_GROUP),
            k: 0,
            failed_at: None,
            last_error: "not connected".into(),
            syncs: VecDeque::with_capacity(16),
            last_rx: 0,
            probes: 0,
            last_probe: 0,
        }
    }

    fn seal(&mut self) {
        if self.open == 0 {
            return;
        }
        self.out.extend_from_slice(&pg::SYNC);
        if let Some(w) = self.waiters.back_mut() {
            w.last = true;
        }
        self.open = 0;
        self.in_flight += 1;
        self.sealed = self.out.len();
        self.syncs.push_back(false);
    }

    fn probe(&mut self) {
        self.out.splice(self.sealed..self.sealed, pg::SYNC);
        self.sealed += pg::SYNC.len();
        self.syncs.push_back(true);
    }
}

const S200: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
const S201: &[u8] = b"HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n";
const S400: &[u8] = b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\n";
const S401: &[u8] = b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n";
const S404: &[u8] = b"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\n";
const S500: &[u8] = b"HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\n";
const S503: &[u8] = b"HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n";

const E_NOT_FOUND: &[u8] = b"{\"error\":\"not found\"}";
const E_INVALID_ID: &[u8] = b"{\"error\":\"invalid post id\"}";
const E_NO_POST: &[u8] = b"{\"error\":\"post not found\"}";
const E_MISSING_TOKEN: &[u8] = b"{\"error\":\"missing bearer token\"}";
const E_INVALID_TOKEN: &[u8] = b"{\"error\":\"invalid or expired token\"}";
const E_BAD_PAYLOAD: &[u8] = b"{\"error\":\"invalid token payload\"}";
const E_MALFORMED: &[u8] = b"{\"error\":\"malformed JSON body\"}";
const E_BODY_REQUIRED: &[u8] = b"{\"error\":\"body is required\"}";
const E_BODY_TOO_LONG: &[u8] = b"{\"error\":\"body must be at most 500 characters\"}";
const E_INTERNAL: &[u8] = b"{\"error\":\"internal server error\"}";

#[inline]
fn head(h: &mut [u8; 192], status: &[u8], len: usize, gzip: bool, close: bool) -> usize {
    let mut n = 0;
    let mut put = |s: &[u8]| {
        h[n..n + s.len()].copy_from_slice(s);
        n += s.len();
    };
    put(status);
    if gzip {
        put(b"Content-Encoding: gzip\r\n");
    }
    if close {
        put(b"Connection: close\r\n");
    }
    put(b"Content-Length: ");
    let mut d = [0u8; 20];
    let mut p = 20;
    let mut v = len;
    loop {
        p -= 1;
        d[p] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    put(&d[p..]);
    put(b"\r\n\r\n");
    n
}

fn simple(o: &mut Vec<u8>, status: &[u8], body: &[u8], close: bool) {
    let mut h = [0u8; 192];
    let n = head(&mut h, status, body.len(), false, close);
    o.extend_from_slice(&h[..n]);
    o.extend_from_slice(body);
}

fn parse_id(b: &[u8]) -> i64 {
    if b.is_empty() {
        return -1;
    }
    let mut v: i64 = 0;
    let mut overflow = false;
    for &c in b {
        if !c.is_ascii_digit() {
            return -1;
        }
        if !overflow {
            match v.checked_mul(10).and_then(|x| x.checked_add((c - b'0') as i64)) {
                Some(x) => v = x,
                None => overflow = true,
            }
        }
    }
    if overflow {
        -2
    } else if v > 0 {
        v
    } else {
        -1
    }
}

pub struct Loop {
    id: usize,
    cfg: &'static Config,
    db: DbConfig,
    table: Table,
    ep: i32,
    listeners: Vec<i32>,
    waiter: sys::Waiter,
    pgs: Vec<Pg>,
    readers: usize,
    client_events: u32,
    reread: Vec<i32>,
    jwt: Jwt,
    gzip: Gzip,
    feed_bind: Vec<u8>,
    ping_bind: Vec<u8>,
    rbuf: Vec<u8>,
    obuf: Vec<u8>,
    gzbuf: Vec<u8>,
    scratch: Vec<u8>,
    chunk: Vec<u8>,
    slots: [Slot; 1],
    key: Vec<u8>,
    jobs: Vec<Job>,
    free_jobs: Vec<u32>,
    writes: VecDeque<u32>,
    writes_since: u64,
    hurry: bool,
    kick: bool,
    bufs: Vec<Box<Vec<u8>>>,
    now: u64,
    now_ms: u32,
    connect_failed: bool,
    last_connect_fail: u64,
    next_revive: u64,
    last_accept_error: u64,
    nodelay_checked: bool,
    nodelay_inherited: bool,
    thin_inherited: bool,
}

impl Loop {
    pub fn new(id: usize, cfg: &'static Config, db: DbConfig, secret: &[u8], table: Table, listeners: Vec<i32>) -> Loop {
        let mut rbuf = vec![0u8; MAX_PENDING + READ_CAP];
        rbuf.shrink_to_fit();
        Loop {
            id,
            cfg,
            db,
            table,
            ep: sys::epoll_create(),
            listeners,
            waiter: sys::Waiter::new(),
            pgs: (0..cfg.readers + cfg.writers).map(|_| Pg::new()).collect(),
            readers: cfg.readers,
            client_events: if cfg.edge {
                sys::EPOLLIN | sys::EPOLLOUT | sys::EPOLLRDHUP | sys::EPOLLET
            } else {
                sys::EPOLLIN
            },
            reread: Vec::new(),
            jwt: Jwt::new(secret),
            gzip: Gzip::new(),
            feed_bind: pg::bind_no_params(pg::FEED),
            ping_bind: pg::bind_no_params(pg::PING),
            rbuf,
            obuf: Vec::with_capacity(64 * 1024),
            gzbuf: Vec::with_capacity(HEADROOM + crate::gzip::bound(256 * 1024)),
            scratch: Vec::with_capacity(8 * 1024),
            chunk: Vec::with_capacity(4096),
            slots: Default::default(),
            key: Vec::with_capacity(64),
            jobs: Vec::new(),
            free_jobs: Vec::new(),
            writes: VecDeque::with_capacity(MAX_GROUP),
            writes_since: 0,
            hurry: false,
            kick: false,
            bufs: Vec::new(),
            now: sys::now_ns(),
            now_ms: 0,
            connect_failed: false,
            last_connect_fail: 0,
            next_revive: 0,
            last_accept_error: 0,
            nodelay_checked: false,
            nodelay_inherited: false,
            thin_inherited: false,
        }
    }

    #[inline(always)]
    fn conn(&self, fd: i32) -> &'static mut Conn {
        debug_assert!((fd as usize) < self.table.len);
        unsafe { &mut *self.table.ptr.add(fd as usize) }
    }

    #[inline]
    fn tick(&mut self) {
        self.now = sys::now_ns();
        self.now_ms = (self.now / 1_000_000) as u32;
    }

    pub fn run(mut self) -> ! {
        for (i, &fd) in self.listeners.iter().enumerate() {
            sys::epoll_ctl(self.ep, sys::CTL_ADD, fd, sys::EPOLLIN, TAG_LISTEN | i as u64);
        }
        sys::tune_thread(self.cfg.sched_idle, self.cfg.timer_slack_ns);
        self.tick();
        for i in 0..self.pgs.len() {
            self.reconnect(i);
        }
        let mut events = vec![libc::epoll_event { events: 0, u64: 0 }; MAX_EVENTS];
        let mut wait = IDLE_WAIT_NS;
        loop {
            let mut n = self.waiter.wait(self.ep, &mut events, wait);
            self.tick();
            let woke = self.now;
            if n < 0 {
                let e = sys::errno();
                assert!(e == sys::EINTR, "epoll_wait: errno {e}");
                n = 0;
            }
            for k in 0..n as usize {
                let e = events[k];
                let (data, ev) = (e.u64, e.events);
                if data >= TAG_PG {
                    self.on_pg((data - TAG_PG) as usize, ev);
                } else if data >= TAG_LISTEN {
                    self.on_listen((data - TAG_LISTEN) as usize);
                } else {
                    self.on_client(data as i32, ev);
                }
            }
            while let Some(fd) = self.reread.pop() {
                self.on_client(fd, sys::EPOLLIN);
            }
            let sleep = self.cfg.pass_ns > 0 && n > 0 && (n as usize) < MAX_EVENTS;
            if sleep {
                self.hurry = true;
            }
            self.kick = false;
            let due = self.pump_writes().min(self.flush_reads());
            self.hurry = false;
            for i in 0..self.pgs.len() {
                self.flush_pg(i);
            }
            if self.now >= self.next_revive {
                self.revive();
            }
            if self.cfg.probe_ns > 0 {
                self.probe_silent();
            }
            if sleep && !self.kick {
                let spent = sys::now_ns() - woke;
                if spent < self.cfg.pass_ns {
                    std::thread::sleep(std::time::Duration::from_nanos(self.cfg.pass_ns - spent));
                }
            }
            wait = if self.kick {
                0
            } else if due == u64::MAX {
                IDLE_WAIT_NS
            } else {
                (due.saturating_sub(sys::now_ns()) as i64).min(IDLE_WAIT_NS)
            };
        }
    }

    fn on_listen(&mut self, idx: usize) {
        let lfd = self.listeners[idx];
        for _ in 0..ACCEPT_BURST {
            let fd = sys::accept(lfd);
            if fd < 0 {
                let e = sys::errno();
                if e == sys::EINTR || e == libc::ECONNABORTED {
                    continue;
                }
                if e != sys::EAGAIN && self.now - self.last_accept_error > 1_000_000_000 {
                    self.last_accept_error = self.now;
                    eprintln!("loop {}: accept: errno {e}", self.id);
                }
                return;
            }
            if fd as usize >= self.table.len {
                sys::close(fd);
                continue;
            }
            if !self.nodelay_checked {
                self.nodelay_checked = true;
                self.nodelay_inherited = sys::get_int(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY) > 0;
                self.thin_inherited = sys::get_int(fd, libc::IPPROTO_TCP, sys::TCP_THIN_LINEAR_TIMEOUTS) > 0;
                if self.id == 0 {
                    let how = |b: bool| if b { "inherited from the listening socket" } else { "set per socket" };
                    eprintln!("TCP_NODELAY {}; TCP_THIN_LINEAR_TIMEOUTS {}", how(self.nodelay_inherited), how(self.thin_inherited));
                }
            }
            if !self.nodelay_inherited {
                sys::set_nodelay(fd);
            }
            if self.cfg.thin && !self.thin_inherited {
                sys::set_thin(fd);
            }
            unsafe {
                std::ptr::write(
                    self.table.ptr.add(fd as usize),
                    Conn { flags: OPEN, since_ms: 0, out_from: 0, _pad: 0, inb: None, outb: None },
                );
            }
            if self.cfg.edge {
                self.on_client(fd, sys::EPOLLIN);
                if self.conn(fd).flags & OPEN != 0 && sys::epoll_ctl(self.ep, sys::CTL_ADD, fd, self.client_events, fd as u64) != 0 {
                    self.close(fd);
                }
                continue;
            }
            if sys::epoll_ctl(self.ep, sys::CTL_ADD, fd, sys::EPOLLIN, fd as u64) != 0 {
                self.conn(fd).flags = 0;
                sys::close(fd);
                continue;
            }
            self.on_client(fd, sys::EPOLLIN);
        }
    }

    fn take_buf(&mut self) -> Box<Vec<u8>> {
        self.bufs.pop().unwrap_or_else(|| Box::new(Vec::with_capacity(4096)))
    }

    fn put_buf(&mut self, mut b: Box<Vec<u8>>) {
        if b.capacity() <= 256 * 1024 && self.bufs.len() < 4096 {
            b.clear();
            self.bufs.push(b);
        }
    }

    fn rearm(&self, fd: i32) {
        if self.cfg.edge {
            return;
        }
        let f = self.conn(fd).flags;
        let mut ev = 0;
        if f & (PAUSED | PEER_DONE) == 0 {
            ev |= sys::EPOLLIN;
        }
        if f & WANT_OUT != 0 {
            ev |= sys::EPOLLOUT;
        }
        sys::epoll_ctl(self.ep, sys::CTL_MOD, fd, ev, fd as u64);
    }

    fn unpause(&mut self, fd: i32) {
        self.conn(fd).flags &= !PAUSED;
        if self.cfg.edge {
            self.reread.push(fd);
        } else {
            self.rearm(fd);
        }
    }

    fn close(&mut self, fd: i32) {
        let c = self.conn(fd);
        if c.flags & OPEN == 0 {
            return;
        }
        if let Some(b) = c.inb.take() {
            self.put_buf(b);
        }
        if let Some(b) = c.outb.take() {
            self.put_buf(b);
        }
        if c.flags & BUSY != 0 {
            c.flags = CLOSING | BUSY;
            sys::epoll_ctl(self.ep, sys::CTL_DEL, fd, 0, 0);
            return;
        }
        c.flags = 0;
        sys::close(fd);
    }

    fn on_client(&mut self, fd: i32, ev: u32) {
        let c = self.conn(fd);
        if c.flags & OPEN == 0 {
            return;
        }
        if ev & sys::EPOLLERR != 0 {
            self.close(fd);
            return;
        }
        if ev & sys::EPOLLOUT != 0 && c.flags & WANT_OUT != 0 && !self.flush_out(fd) && self.conn(fd).flags & OPEN == 0 {
            return;
        }
        if ev & (sys::EPOLLIN | sys::EPOLLHUP | sys::EPOLLRDHUP) == 0 {
            return;
        }
        while self.read_client(fd) && self.cfg.edge {}
    }

    fn read_client(&mut self, fd: i32) -> bool {
        let c = self.conn(fd);
        if c.flags & OPEN == 0 || c.flags & (PAUSED | PEER_DONE) != 0 {
            return false;
        }
        if c.flags & (BUSY | WANT_OUT) != 0 {
            let mut b = c.inb.take().unwrap_or_else(|| self.take_buf());
            if b.len() >= MAX_PENDING {
                c.inb = Some(b);
                c.flags |= PAUSED;
                self.rearm(fd);
                return false;
            }
            let len = b.len();
            b.reserve(16 * 1024);
            let room = (b.capacity() - len).min(MAX_PENDING - len);
            let n = unsafe { libc::read(fd, b.as_mut_ptr().add(len) as *mut libc::c_void, room) };
            if n > 0 {
                unsafe { b.set_len(len + n as usize) };
            }
            let c = self.conn(fd);
            if b.is_empty() {
                self.put_buf(b);
            } else {
                c.inb = Some(b);
            }
            if n == 0 {
                c.flags |= PEER_DONE;
                self.rearm(fd);
            } else if n < 0 && sys::errno() != sys::EAGAIN {
                self.close(fd);
            }
            return n > 0 && n as usize == room;
        }

        let mut rbuf = std::mem::take(&mut self.rbuf);
        let mut held = 0;
        if let Some(b) = c.inb.take() {
            held = b.len();
            rbuf[..held].copy_from_slice(&b);
            self.put_buf(b);
        }
        let n = sys::read(fd, &mut rbuf[held..held + READ_CAP]);
        if n <= 0 {
            if n == 0 || sys::errno() != sys::EAGAIN {
                self.rbuf = rbuf;
                self.close(fd);
                return false;
            }
            if held == 0 {
                self.rbuf = rbuf;
                return false;
            }
        }
        let total = held + n.max(0) as usize;
        self.serve(fd, &rbuf[..total]);
        self.rbuf = rbuf;
        n as usize == READ_CAP
    }

    fn serve(&mut self, fd: i32, data: &[u8]) {
        let mut obuf = std::mem::take(&mut self.obuf);
        obuf.clear();
        let mut at = 0;
        while at < data.len() {
            let mut chunk = std::mem::take(&mut self.chunk);
            let parsed = http::parse(&data[at..], &mut chunk);
            match parsed {
                Parsed::Incomplete => {
                    self.chunk = chunk;
                    if data.len() - at >= MAX_PENDING {
                        self.conn(fd).flags |= CLOSE_AFTER;
                        simple(&mut obuf, b"HTTP/1.1 413 Content Too Large\r\nContent-Type: application/json\r\n",
                            b"{\"error\":\"request too large\"}", true);
                        at = data.len();
                    }
                    break;
                }
                Parsed::Bad(status, body) => {
                    self.chunk = chunk;
                    self.conn(fd).flags |= CLOSE_AFTER;
                    let mut line = Vec::with_capacity(80);
                    line.extend_from_slice(b"HTTP/1.1 ");
                    line.extend_from_slice(status);
                    line.extend_from_slice(b"\r\nContent-Type: application/json\r\n");
                    simple(&mut obuf, &line, body, true);
                    at = data.len();
                    break;
                }
                Parsed::Ok(r) => {
                    let c = self.conn(fd);
                    c.flags = (c.flags & !GZIP) | if r.gzip { GZIP } else { 0 } | if r.close { CLOSE_AFTER } else { 0 };
                    let req = &data[at..at + r.total];
                    self.route(fd, req, &r, &chunk, &mut obuf);
                    self.chunk = chunk;
                    at += r.total;
                    let f = self.conn(fd).flags;
                    if f & CLOSE_AFTER != 0 {
                        at = data.len();
                        break;
                    }
                    if f & BUSY != 0 {
                        break;
                    }
                }
            }
        }
        if at < data.len() {
            let mut b = self.take_buf();
            b.extend_from_slice(&data[at..]);
            self.conn(fd).inb = Some(b);
        }
        if !obuf.is_empty() {
            self.send(fd, &obuf);
        }
        self.obuf = obuf;
        let c = self.conn(fd);
        if c.flags & OPEN == 0 {
            return;
        }
        if c.flags & (BUSY | WANT_OUT) == 0 && (c.flags & CLOSE_AFTER != 0 || (c.flags & PEER_DONE != 0)) {
            self.close(fd);
        } else if c.flags & PAUSED != 0 && c.inb.as_ref().is_none_or(|b| b.len() < MAX_PENDING) {
            self.unpause(fd);
        }
    }

    fn send(&mut self, fd: i32, data: &[u8]) {
        let c = self.conn(fd);
        if c.flags & OPEN == 0 {
            return;
        }
        if let Some(ob) = c.outb.as_mut() {
            ob.extend_from_slice(data);
            return;
        }
        let mut n = sys::send(fd, data);
        if n == data.len() as isize {
            return;
        }
        if n < 0 {
            if sys::errno() != sys::EAGAIN {
                self.close(fd);
                return;
            }
            n = 0;
        }
        let mut b = self.take_buf();
        b.extend_from_slice(&data[n as usize..]);
        let c = self.conn(fd);
        c.outb = Some(b);
        c.out_from = 0;
        c.flags |= WANT_OUT;
        self.rearm(fd);
    }

    fn flush_out(&mut self, fd: i32) -> bool {
        let c = self.conn(fd);
        let ob = c.outb.as_ref().unwrap();
        let n = sys::send(fd, &ob[c.out_from as usize..]);
        if n < 0 {
            if sys::errno() != sys::EAGAIN {
                self.close(fd);
            }
            return false;
        }
        c.out_from += n as u32;
        if (c.out_from as usize) < ob.len() {
            return false;
        }
        let b = c.outb.take().unwrap();
        c.out_from = 0;
        c.flags &= !WANT_OUT;
        self.put_buf(b);
        let c = self.conn(fd);
        if c.flags & BUSY == 0 && c.flags & (CLOSE_AFTER | PEER_DONE) != 0 && c.inb.is_none() {
            self.close(fd);
            return false;
        }
        self.rearm(fd);
        if c.flags & BUSY == 0 {
            self.resume(fd);
        }
        false
    }

    fn resume(&mut self, fd: i32) {
        let c = self.conn(fd);
        if c.flags & OPEN == 0 || c.flags & (BUSY | WANT_OUT) != 0 {
            return;
        }
        if let Some(b) = c.inb.take() {
            self.serve(fd, &b);
            self.put_buf(b);
        } else if c.flags & (PEER_DONE | CLOSE_AFTER) != 0 {
            self.close(fd);
        } else if c.flags & PAUSED != 0 {
            self.unpause(fd);
        }
    }

    fn route(&mut self, fd: i32, b: &[u8], r: &Req, chunk: &[u8], o: &mut Vec<u8>) {
        let path = &b[r.path.0..r.path.1];
        let close = r.close;
        match r.method {
            Method::Get => {
                if path == b"/feed" {
                    self.queue_read(fd, pg::FEED, 0, o);
                } else if path.len() > 7 && path.starts_with(b"/posts/") && !path[7..].contains(&b'/') {
                    match parse_id(&path[7..]) {
                        -1 => simple(o, S400, E_INVALID_ID, close),
                        -2 => simple(o, S404, E_NO_POST, close),
                        id => self.queue_read(fd, pg::POST, id, o),
                    }
                } else if path == b"/health" {
                    self.queue_read(fd, pg::PING, 0, o);
                } else {
                    simple(o, S404, E_NOT_FOUND, close);
                }
            }
            Method::Post => {
                if path.len() > 12 && path.starts_with(b"/posts/") && path.ends_with(b"/like")
                    && !path[7..path.len() - 5].contains(&b'/')
                {
                    if !self.auth(b, r, o) {
                        return;
                    }
                    match parse_id(&path[7..path.len() - 5]) {
                        -1 => simple(o, S400, E_INVALID_ID, close),
                        -2 => simple(o, S404, E_NO_POST, close),
                        id => {
                            let j = self.alloc_job(fd, pg::LIKE);
                            let job = &mut self.jobs[j as usize];
                            job.user_id = self.jwt.user_id;
                            job.post_id = id;
                            self.queue_write(fd, j);
                        }
                    }
                } else if path == b"/posts" {
                    if !self.auth(b, r, o) {
                        return;
                    }
                    let body = if r.chunked { chunk } else { &b[r.body.0..r.body.1] };
                    self.create(fd, body, close, o);
                } else {
                    simple(o, S404, E_NOT_FOUND, close);
                }
            }
            Method::Other => simple(o, S404, E_NOT_FOUND, close),
        }
    }

    fn auth(&mut self, b: &[u8], r: &Req, o: &mut Vec<u8>) -> bool {
        let header = r.auth.map(|(s, e)| &b[s..e]);
        let e = match self.jwt.verify(header, sys::unix_secs()) {
            Auth::Ok => return true,
            Auth::Missing => E_MISSING_TOKEN,
            Auth::Invalid => E_INVALID_TOKEN,
            Auth::BadPayload => E_BAD_PAYLOAD,
        };
        simple(o, S401, e, r.close);
        false
    }

    fn create(&mut self, fd: i32, body: &[u8], close: bool, o: &mut Vec<u8>) {
        if std::str::from_utf8(body).is_err() {
            return simple(o, S400, E_MALFORMED, close);
        }
        let is_object = match json::parse_top(body, &[b"body"], &mut self.slots, &mut self.key) {
            Ok(v) => v,
            Err(_) => return simple(o, S400, E_MALFORMED, close),
        };
        if !is_object || self.slots[0].kind != Kind::Str {
            return simple(o, S400, E_BODY_REQUIRED, close);
        }
        let s = unsafe { std::str::from_utf8_unchecked(&self.slots[0].str) };
        let t = json::trim(s);
        if t.is_empty() {
            return simple(o, S400, E_BODY_REQUIRED, close);
        }
        if t.len() > 500 && t.chars().count() > 500 {
            return simple(o, S400, E_BODY_TOO_LONG, close);
        }
        let (from, len) = (t.as_ptr() as usize - s.as_ptr() as usize, t.len());
        let j = self.alloc_job(fd, pg::CREATE);
        let job = &mut self.jobs[j as usize];
        job.body.clear();
        job.body.extend_from_slice(&self.slots[0].str[from..from + len]);
        job.user.clear();
        job.user.extend_from_slice(&self.jwt.username);
        job.user_id = self.jwt.user_id;
        self.queue_write(fd, j);
    }

    fn prefer(a: &Pg, b: &Pg) -> bool {
        let af = a.open > 0 && a.in_flight < MAX_IN_FLIGHT;
        let bf = b.open > 0 && b.in_flight < MAX_IN_FLIGHT;
        if af != bf {
            return af;
        }
        if a.in_flight != b.in_flight {
            return a.in_flight < b.in_flight;
        }
        if (a.open > 0) != (b.open > 0) {
            return a.open > 0;
        }
        a.waiters.len() < b.waiters.len()
    }

    fn pick_reader(&mut self) -> Option<usize> {
        let mut best: Option<usize> = None;
        for i in 0..self.readers {
            if self.pgs[i].fd < 0 {
                continue;
            }
            if best.is_none_or(|b| Self::prefer(&self.pgs[i], &self.pgs[b])) {
                best = Some(i);
            }
        }
        if best.is_some() {
            return best;
        }
        (0..self.readers).find(|&i| self.reconnect(i))
    }

    fn queue_read(&mut self, fd: i32, kind: u8, id: i64, o: &mut Vec<u8>) {
        let i = match self.pick_reader() {
            Some(i) => i,
            None => {
                let close = self.conn(fd).flags & CLOSE_AFTER != 0;
                if kind == pg::PING {
                    let msg = self.pgs[0].last_error.clone();
                    degraded(o, &msg, close);
                } else {
                    simple(o, S500, E_INTERNAL, close);
                }
                return;
            }
        };
        self.push_read(i, Waiter { fd, kind, last: false, id });
        let c = self.conn(fd);
        c.flags |= BUSY;
        c.since_ms = self.now_ms;
    }

    fn push_read(&mut self, i: usize, w: Waiter) {
        self.kick = true;
        let pg = &mut self.pgs[i];
        match w.kind {
            pg::FEED => pg.out.extend_from_slice(&self.feed_bind),
            pg::POST => pg::bind_post(&mut pg.out, w.id),
            _ => pg.out.extend_from_slice(&self.ping_bind),
        }
        pg.waiters.push_back(w);
        if pg.open == 0 {
            pg.open_since = self.now;
        }
        pg.open += 1;
        if pg.open >= self.cfg.pack_max {
            pg.seal();
        }
    }

    fn flush_reads(&mut self) -> u64 {
        let mut due = u64::MAX;
        for i in 0..self.readers {
            let pg = &mut self.pgs[i];
            if pg.fd < 0 || pg.open == 0 {
                continue;
            }
            if pg.open < self.cfg.pack_max {
                if pg.in_flight >= MAX_IN_FLIGHT {
                    continue;
                }
                let d = pg.open_since + self.cfg.hold_ns;
                if !self.hurry && self.now < d {
                    due = due.min(d);
                    continue;
                }
            }
            pg.seal();
        }
        due
    }

    fn alloc_job(&mut self, fd: i32, kind: u8) -> u32 {
        let j = match self.free_jobs.pop() {
            Some(j) => j,
            None => {
                self.jobs.push(Job {
                    fd: -1,
                    kind: 0,
                    inserted: false,
                    user_id: 0,
                    post_id: 0,
                    new_id: 0,
                    ts: 0,
                    body: Vec::with_capacity(512),
                    user: Vec::with_capacity(32),
                });
                (self.jobs.len() - 1) as u32
            }
        };
        let job = &mut self.jobs[j as usize];
        job.fd = fd;
        job.kind = kind;
        job.inserted = false;
        j
    }

    fn queue_write(&mut self, fd: i32, j: u32) {
        self.kick = true;
        if self.writes.is_empty() {
            self.writes_since = self.now;
        }
        self.writes.push_back(j);
        let c = self.conn(fd);
        c.flags |= BUSY;
        c.since_ms = self.now_ms;
    }

    fn pump_writes(&mut self) -> u64 {
        if self.writes.is_empty() {
            return u64::MAX;
        }
        let due = self.writes_since + self.cfg.hold_ns;
        if !self.hurry && self.now < due && self.writes.len() < MAX_GROUP {
            return due;
        }
        let writers = self.readers..self.pgs.len();
        while !self.writes.is_empty() {
            let w = match writers.clone().find(|&i| self.pgs[i].fd >= 0 && self.pgs[i].group.is_empty()) {
                Some(w) => w,
                None if writers.clone().any(|i| self.pgs[i].fd >= 0) => return u64::MAX,
                None => match writers.clone().find(|&i| self.reconnect(i)) {
                    Some(w) => w,
                    None => {
                        let msg = self.pgs[self.readers].last_error.clone();
                        while let Some(j) = self.writes.pop_front() {
                            self.fail_job(j, &msg);
                        }
                        self.writes_since = 0;
                        return u64::MAX;
                    }
                },
            };
            let pg = &mut self.pgs[w];
            let n = self.writes.len().min(MAX_GROUP);
            for _ in 0..n {
                let j = self.writes.pop_front().unwrap();
                let job = &self.jobs[j as usize];
                if job.kind == pg::CREATE {
                    pg::bind_create(&mut pg.out, job.user_id, &job.body);
                } else {
                    pg::bind_like(&mut pg.out, job.user_id, job.post_id);
                }
                pg.group.push(j);
            }
            pg.out.extend_from_slice(&pg::SYNC);
            pg.sealed = pg.out.len();
            pg.syncs.push_back(false);
            pg.k = 0;
            pg.failed_at = None;
        }
        self.writes_since = 0;
        u64::MAX
    }

    fn probe_silent(&mut self) {
        let now = self.now;
        let base = self.cfg.probe_ns;
        for pg in self.pgs.iter_mut() {
            if pg.fd < 0 || !pg.syncs.iter().any(|&p| !p) || pg.sent < pg.sealed {
                continue;
            }
            let wait = base << pg.probes.min(4);
            if now.saturating_sub(pg.last_rx) >= wait && now.saturating_sub(pg.last_probe) >= wait {
                pg.probe();
                pg.probes += 1;
                pg.last_probe = now;
                self.kick = true;
            }
        }
        if self.kick {
            for i in 0..self.pgs.len() {
                self.flush_pg(i);
            }
        }
    }

    fn revive(&mut self) {
        self.next_revive = self.now + RECONNECT_NS;
        for i in 0..self.pgs.len() {
            if self.pgs[i].fd < 0 && !self.reconnect(i) {
                return;
            }
        }
    }

    fn reconnect(&mut self, i: usize) -> bool {
        if self.pgs[i].fd >= 0 {
            return true;
        }
        let now = sys::now_ns();
        if self.connect_failed && now - self.last_connect_fail < RECONNECT_NS {
            return false;
        }
        match pg::connect(&self.db) {
            Ok(fd) => {
                self.connect_failed = false;
                if self.cfg.thin {
                    sys::set_thin(fd);
                }
                sys::clear_quickack(fd);
                let pg = &mut self.pgs[i];
                pg.fd = fd;
                pg.want_write = false;
                pg.last_rx = self.now;
                pg.probes = 0;
                sys::epoll_ctl(self.ep, sys::CTL_ADD, fd, sys::EPOLLIN, TAG_PG | i as u64);
                true
            }
            Err(e) => {
                eprintln!("loop {}: postgres: {e}", self.id);
                self.pgs[i].last_error = e;
                self.connect_failed = true;
                self.last_connect_fail = sys::now_ns();
                false
            }
        }
    }

    fn flush_pg(&mut self, i: usize) {
        let pg = &mut self.pgs[i];
        if pg.fd < 0 {
            return;
        }
        while pg.sent < pg.sealed {
            let n = sys::send(pg.fd, &pg.out[pg.sent..pg.sealed]);
            if n < 0 {
                let e = sys::errno();
                if e == sys::EINTR {
                    continue;
                }
                if e == sys::EAGAIN {
                    break;
                }
                self.fail(i, format!("write: errno {e}"));
                return;
            }
            pg.sent += n as usize;
        }
        if pg.sent == pg.sealed && pg.sent > 0 {
            let open = pg.out.len() - pg.sent;
            pg.out.copy_within(pg.sent.., 0);
            pg.out.truncate(open);
            pg.sent = 0;
            pg.sealed = 0;
        }
        let want = pg.sent < pg.sealed;
        if want != pg.want_write {
            pg.want_write = want;
            let ev = sys::EPOLLIN | if want { sys::EPOLLOUT } else { 0 };
            sys::epoll_ctl(self.ep, sys::CTL_MOD, pg.fd, ev, TAG_PG | i as u64);
        }
    }

    fn fail(&mut self, i: usize, error: String) {
        eprintln!("loop {}: postgres connection lost: {error}", self.id);
        let pg = &mut self.pgs[i];
        if pg.fd >= 0 {
            sys::close(pg.fd);
        }
        pg.fd = -1;
        pg.epoch = pg.epoch.wrapping_add(1);
        pg.last_error = error.clone();
        let waiters = std::mem::take(&mut pg.waiters);
        let group = std::mem::take(&mut pg.group);
        pg.out.clear();
        pg.sent = 0;
        pg.sealed = 0;
        pg.want_write = false;
        pg.inb.clear();
        pg.open = 0;
        pg.in_flight = 0;
        pg.body.truncate(HEADROOM);
        pg.rows = 0;
        pg.err = None;
        pg.k = 0;
        pg.failed_at = None;
        pg.syncs.clear();
        for w in waiters {
            self.finish_error(w.fd, w.kind, &error);
        }
        for j in group {
            self.fail_job(j, &error);
        }
    }

    fn on_pg(&mut self, i: usize, ev: u32) {
        if ev & sys::EPOLLOUT != 0 {
            self.flush_pg(i);
        }
        let pg = &mut self.pgs[i];
        if pg.fd < 0 || ev & (sys::EPOLLIN | sys::EPOLLERR | sys::EPOLLHUP) == 0 {
            return;
        }
        let len = pg.inb.len();
        pg.inb.reserve(64 * 1024);
        let room = pg.inb.capacity() - len;
        let n = unsafe { libc::read(pg.fd, pg.inb.as_mut_ptr().add(len) as *mut libc::c_void, room) };
        if n > 0 {
            unsafe { pg.inb.set_len(len + n as usize) };
            pg.last_rx = self.now;
            pg.probes = 0;
            sys::clear_quickack(pg.fd);
        } else if n == 0 {
            self.fail(i, "connection closed by the database".into());
            return;
        } else {
            let e = sys::errno();
            if e != sys::EAGAIN && e != sys::EINTR {
                self.fail(i, format!("read: errno {e}"));
            }
            return;
        }
        self.parse_pg(i);
    }

    fn parse_pg(&mut self, i: usize) {
        let epoch = self.pgs[i].epoch;
        let reader = i < self.readers;
        let mut inb = std::mem::take(&mut self.pgs[i].inb);
        let mut p = 0;
        while inb.len() - p >= 5 {
            let n = pg::be32(&inb, p + 1) as usize;
            if inb.len() - p < 1 + n {
                break;
            }
            let t = inb[p];
            let m = &inb[p + 5..p + 1 + n];
            p += 1 + n;
            if reader {
                self.on_read_message(i, t, m);
            } else {
                self.on_write_message(i, t, m);
            }
            if self.pgs[i].epoch != epoch {
                inb.clear();
                self.pgs[i].inb = inb;
                return;
            }
        }
        let rest = inb.len() - p;
        inb.copy_within(p.., 0);
        inb.truncate(rest);
        self.pgs[i].inb = inb;
    }

    fn on_read_message(&mut self, i: usize, t: u8, m: &[u8]) {
        match t {
            b'D' => {
                let pg = &mut self.pgs[i];
                let kind = match pg.waiters.front() {
                    Some(w) => w.kind,
                    None => return,
                };
                if kind == pg::PING {
                    return;
                }
                if pg.rows > 0 {
                    pg.body.push(b',');
                }
                pg.rows += 1;
                post_json(&mut pg.body, m);
            }
            b'C' | b's' => {
                if let Some(w) = self.pgs[i].waiters.pop_front() {
                    self.finish_read(i, w);
                }
            }
            b'E' => {
                let pg = &mut self.pgs[i];
                let f = pg::error_fields(m);
                if pg.waiters.is_empty() {
                    eprintln!("loop {}: postgres: {} ({})", self.id, f.1, String::from_utf8_lossy(&f.0));
                } else if pg.err.is_none() {
                    pg.err = Some(f);
                }
            }
            b'Z' => {
                let pg = &mut self.pgs[i];
                if pg.syncs.pop_front() == Some(true) {
                    return;
                }
                pg.in_flight = pg.in_flight.saturating_sub(1);
                let Some((code, msg)) = pg.err.take() else { return };
                pg.body.truncate(HEADROOM);
                pg.rows = 0;
                let mut first = true;
                while let Some(w) = self.pgs[i].waiters.pop_front() {
                    if first {
                        eprintln!("loop {}: postgres: {msg} ({})", self.id, String::from_utf8_lossy(&code));
                        self.finish_error(w.fd, w.kind, &msg);
                        first = false;
                    } else {
                        self.retry_read(w);
                    }
                    if w.last {
                        break;
                    }
                }
            }
            _ => {}
        }
    }

    fn retry_read(&mut self, mut w: Waiter) {
        self.hurry = true;
        if self.conn(w.fd).flags & CLOSING != 0 {
            self.finish_begin(w.fd);
            return;
        }
        match self.pick_reader() {
            Some(i) => {
                w.last = false;
                self.push_read(i, w);
            }
            None => self.finish_error(w.fd, w.kind, "database connection is down"),
        }
    }

    fn on_write_message(&mut self, i: usize, t: u8, m: &[u8]) {
        let pg = &mut self.pgs[i];
        match t {
            b'D' => {
                if let Some(&j) = pg.group.get(pg.k) {
                    let job = &mut self.jobs[j as usize];
                    if m.len() >= 26 {
                        job.new_id = pg::be64(m, 6);
                        job.ts = pg::be64(m, 18);
                        job.inserted = true;
                    }
                }
            }
            b'C' => {
                if let Some(&j) = pg.group.get(pg.k) {
                    let job = &mut self.jobs[j as usize];
                    if job.kind == pg::LIKE {
                        job.inserted = m.len() >= 2 && m[m.len() - 2] == b'1';
                    }
                    pg.k += 1;
                }
            }
            b'E' => {
                let f = pg::error_fields(m);
                if pg.group.is_empty() {
                    eprintln!("loop {}: postgres: {} ({})", self.id, f.1, String::from_utf8_lossy(&f.0));
                } else if pg.failed_at.is_none() {
                    pg.failed_at = Some(pg.k);
                    pg.err = Some(f);
                }
            }
            b'Z' => {
                if pg.syncs.pop_front() == Some(true) {
                    return;
                }
                if !pg.group.is_empty() {
                    self.resolve_group(i);
                }
            }
            _ => {}
        }
    }

    fn resolve_group(&mut self, i: usize) {
        let pg = &mut self.pgs[i];
        let mut group = std::mem::take(&mut pg.group);
        let failed = pg.failed_at.take();
        let err = pg.err.take();
        pg.k = 0;
        match failed {
            None => {
                for &j in &group {
                    self.answer_job(j);
                }
            }
            Some(k) if k >= group.len() => {
                let msg = err.map(|e| e.1).unwrap_or_default();
                eprintln!("loop {}: postgres: commit failed: {msg}", self.id);
                for &j in &group {
                    self.fail_job(j, &msg);
                }
            }
            Some(k) => {
                let bad = group[k];
                let (code, msg) = err.unwrap_or(([b'?'; 5], String::new()));
                if self.jobs[bad as usize].kind == pg::LIKE && &code == b"23503" {
                    let fd = self.jobs[bad as usize].fd;
                    self.free_jobs.push(bad);
                    if self.finish_begin(fd) {
                        let mut o = std::mem::take(&mut self.scratch);
                        o.clear();
                        simple(&mut o, S404, E_NO_POST, self.conn(fd).flags & CLOSE_AFTER != 0);
                        self.send(fd, &o);
                        self.scratch = o;
                        self.finish_end(fd);
                    }
                } else {
                    eprintln!("loop {}: postgres: {msg} ({})", self.id, String::from_utf8_lossy(&code));
                    self.fail_job(bad, &msg);
                }
                for &j in group.iter().rev() {
                    if j != bad {
                        self.writes.push_front(j);
                    }
                }
                if self.writes_since == 0 {
                    self.writes_since = self.now;
                }
                self.hurry = true;
                self.kick = true;
            }
        }
        group.clear();
        if self.pgs[i].group.is_empty() {
            self.pgs[i].group = group;
        }
    }

    fn finish_begin(&mut self, fd: i32) -> bool {
        let c = self.conn(fd);
        c.flags &= !BUSY;
        if c.flags & CLOSING != 0 {
            c.flags = 0;
            sys::close(fd);
            return false;
        }
        c.flags & OPEN != 0
    }

    fn finish_end(&mut self, fd: i32) {
        let c = self.conn(fd);
        if c.flags & OPEN == 0 {
            return;
        }
        if self.now_ms.wrapping_sub(c.since_ms) >= LATE_ACK_MS {
            sys::clear_quickack(fd);
        }
        if c.flags & WANT_OUT != 0 {
            return;
        }
        self.resume(fd);
    }

    fn finish_read(&mut self, i: usize, w: Waiter) {
        let mut body = std::mem::take(&mut self.pgs[i].body);
        let rows = std::mem::replace(&mut self.pgs[i].rows, 0);
        if self.finish_begin(w.fd) {
            let fd = w.fd;
            match w.kind {
                pg::FEED => self.respond_framed(fd, S200, &mut body, b"{\"posts\":[", b"]}"),
                pg::POST if rows > 0 => self.respond_framed(fd, S200, &mut body, b"{\"post\":", b"}"),
                pg::POST => self.respond_simple(fd, S404, E_NO_POST),
                _ => {
                    let up = (sys::now_ns() - self.cfg.start_ns) / 1_000_000_000;
                    let mut o = std::mem::take(&mut self.scratch);
                    o.clear();
                    o.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                    json::put_u64(&mut o, up);
                    o.push(b'}');
                    self.respond_simple(fd, S200, &o);
                    self.scratch = o;
                }
            }
            self.finish_end(fd);
        }
        body.truncate(HEADROOM);
        self.pgs[i].body = body;
    }

    fn finish_error(&mut self, fd: i32, kind: u8, msg: &str) {
        if !self.finish_begin(fd) {
            return;
        }
        if kind == pg::PING {
            let mut o = Vec::new();
            degraded(&mut o, msg, self.conn(fd).flags & CLOSE_AFTER != 0);
            self.send(fd, &o);
        } else {
            self.respond_simple(fd, S500, E_INTERNAL);
        }
        self.finish_end(fd);
    }

    fn answer_job(&mut self, j: u32) {
        let fd = self.jobs[j as usize].fd;
        self.free_jobs.push(j);
        if !self.finish_begin(fd) {
            return;
        }
        let job = &self.jobs[j as usize];
        if job.kind == pg::CREATE {
            if !job.inserted {
                self.respond_simple(fd, S500, E_INTERNAL);
            } else {
                let mut o = std::mem::take(&mut self.scratch);
                o.clear();
                o.resize(HEADROOM, 0);
                o.extend_from_slice(b"{\"id\":");
                json::put_i64(&mut o, job.new_id);
                o.extend_from_slice(b",\"body\":\"");
                json::put_escaped(&mut o, &job.body);
                o.extend_from_slice(b"\",\"created_at\":\"");
                json::put_timestamp(&mut o, job.ts);
                o.extend_from_slice(b"\",\"author\":\"");
                json::put_escaped(&mut o, &job.user);
                o.extend_from_slice(b"\",\"like_count\":0}");
                self.respond_framed(fd, S201, &mut o, b"{\"post\":", b"}");
                self.scratch = o;
            }
        } else {
            let mut o = std::mem::take(&mut self.scratch);
            o.clear();
            o.extend_from_slice(if job.inserted {
                b"{\"liked\":true,\"already_liked\":false,\"post_id\":"
            } else {
                b"{\"liked\":true,\"already_liked\":true,\"post_id\":"
            });
            json::put_i64(&mut o, job.post_id);
            o.push(b'}');
            self.respond_simple(fd, if job.inserted { S201 } else { S200 }, &o);
            self.scratch = o;
        }
        self.finish_end(fd);
    }

    fn fail_job(&mut self, j: u32, _msg: &str) {
        let fd = self.jobs[j as usize].fd;
        self.free_jobs.push(j);
        if self.finish_begin(fd) {
            self.respond_simple(fd, S500, E_INTERNAL);
            self.finish_end(fd);
        }
    }

    fn respond_simple(&mut self, fd: i32, status: &[u8], body: &[u8]) {
        let mut h = [0u8; 192];
        let close = self.conn(fd).flags & CLOSE_AFTER != 0;
        let n = head(&mut h, status, body.len(), false, close);
        let mut o = std::mem::take(&mut self.obuf);
        let mark = o.len();
        o.extend_from_slice(&h[..n]);
        o.extend_from_slice(body);
        self.send(fd, &o[mark..]);
        o.truncate(mark);
        self.obuf = o;
    }

    fn respond_framed(&mut self, fd: i32, status: &[u8], buf: &mut Vec<u8>, prefix: &[u8], suffix: &[u8]) {
        buf.extend_from_slice(suffix);
        let start = HEADROOM - prefix.len();
        buf[start..HEADROOM].copy_from_slice(prefix);
        let f = self.conn(fd).flags;
        let close = f & CLOSE_AFTER != 0;
        let mut h = [0u8; 192];
        if f & GZIP != 0 && buf.len() - start >= self.cfg.gzip_min {
            let mut g = std::mem::take(&mut self.gzbuf);
            g.clear();
            g.resize(HEADROOM, 0);
            self.gzip.compress(&buf[start..], &mut g);
            let n = head(&mut h, status, g.len() - HEADROOM, true, close);
            g[HEADROOM - n..HEADROOM].copy_from_slice(&h[..n]);
            self.send(fd, &g[HEADROOM - n..]);
            self.gzbuf = g;
        } else {
            let n = head(&mut h, status, buf.len() - start, false, close);
            buf[start - n..start].copy_from_slice(&h[..n]);
            self.send(fd, &buf[start - n..]);
        }
    }
}

fn degraded(o: &mut Vec<u8>, msg: &str, close: bool) {
    let mut body = Vec::with_capacity(64 + msg.len());
    body.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    json::put_escaped(&mut body, msg.as_bytes());
    body.extend_from_slice(b"\"}");
    simple(o, S503, &body, close);
}

#[inline]
fn post_json(o: &mut Vec<u8>, m: &[u8]) {
    let mut p = 2;
    let col = |p: &mut usize| -> (usize, usize) {
        let n = pg::be32(m, *p).max(0) as usize;
        let at = *p + 4;
        *p = at + n;
        (at, n)
    };
    let (id_at, id_n) = col(&mut p);
    let (body_at, body_n) = col(&mut p);
    let (ts_at, _) = col(&mut p);
    let (user_at, user_n) = col(&mut p);
    let (likes_at, likes_n) = col(&mut p);
    o.extend_from_slice(b"{\"id\":");
    o.extend_from_slice(&m[id_at..id_at + id_n]);
    o.extend_from_slice(b",\"body\":\"");
    json::put_escaped(o, &m[body_at..body_at + body_n]);
    o.extend_from_slice(b"\",\"created_at\":\"");
    json::put_timestamp(o, pg::be64(m, ts_at));
    o.extend_from_slice(b"\",\"author\":\"");
    json::put_escaped(o, &m[user_at..user_at + user_n]);
    o.extend_from_slice(b"\",\"like_count\":");
    o.extend_from_slice(&m[likes_at..likes_at + likes_n]);
    o.push(b'}');
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(id: i64, body: &str, ts: i64, user: &str, likes: i64) -> Vec<u8> {
        let mut m = vec![0, 5];
        for col in [id.to_string().into_bytes(), body.as_bytes().to_vec(), ts.to_be_bytes().to_vec(), user.as_bytes().to_vec(), likes.to_string().into_bytes()] {
            m.extend_from_slice(&(col.len() as i32).to_be_bytes());
            m.extend_from_slice(&col);
        }
        m
    }

    #[test]
    #[ignore]
    fn feed_cost() {
        let rows: Vec<Vec<u8>> = (0..20)
            .map(|i| {
                let user = format!("golden_ember_{}", 1000 + i * 37);
                row(500_100 - i, &format!("{user} says hi at 2026-11-15T11:20:{:02}.{:03}Z (VU {}, iter {})", i, i * 7, 1000 + i * 37, i % 5),
                    (1_763_205_600 + i) * 1_000_000 + 123_456, &user, i % 3)
            })
            .collect();
        let mut g = Gzip::new();
        let mut body = vec![0u8; HEADROOM];
        let mut out = Vec::new();
        let n = 200_000;
        let mut json_ns = 0u128;
        let t = std::time::Instant::now();
        for _ in 0..n {
            let t0 = std::time::Instant::now();
            body.truncate(HEADROOM);
            for (k, r) in rows.iter().enumerate() {
                if k > 0 {
                    body.push(b',');
                }
                post_json(&mut body, r);
            }
            json_ns += t0.elapsed().as_nanos();
            out.clear();
            g.compress(&body[HEADROOM..], &mut out);
        }
        let total = t.elapsed().as_nanos() / n as u128;
        eprintln!("feed: {} bytes -> {} gzipped; json {} ns, gzip {} ns per feed", body.len() - HEADROOM, out.len(),
            json_ns / n as u128, total - json_ns / n as u128);
    }
}
