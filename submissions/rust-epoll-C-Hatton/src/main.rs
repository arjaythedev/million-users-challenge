//! The feed API: one epoll loop per core, hand-written HTTP/1.1 and Postgres wire protocol.
//! See README.md for the design.

mod http;
mod json;
mod jwt;
mod pg;

use std::collections::VecDeque;
use std::net::Ipv4Addr;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

const MAX_EVENTS: usize = 1024;
/// Reads per Sync (one pack, one round trip).
const READ_PACK: usize = 64;
/// Writes per Sync (one implicit transaction, one commit).
const WRITE_GROUP: usize = 128;
/// A loop pass lasts at least this long, so a busy thread wakes once per window and the requests that arrived
/// meanwhile share one Postgres round trip.
const WINDOW: Duration = Duration::from_micros(2000);
/// Gzip only bodies at least this big (in practice the feed).
const GZIP_MIN: usize = 1024;
/// Postgres connections in total, whatever the core count (rule 5 allows 512).
const PG_READ_TOTAL: usize = 64;
const PG_WRITE_TOTAL: usize = 16;

static START: OnceLock<Instant> = OnceLock::new();

// ---------------------------------------------------------------------------------------------------------
// clients

const USED: u16 = 1;
const BUSY: u16 = 2; // waiting for Postgres; later input waits in `inbuf`
const GZIP: u16 = 4; // the request being served accepts gzip
const CLOSE: u16 = 8; // close after the response to the request being served
const WANT_OUT: u16 = 16; // EPOLLOUT registered, `outbuf` holds unsent bytes
const CONT_SENT: u16 = 32; // "100 Continue" sent for the request being received
const EOF: u16 = 64; // the peer shut down its sending side: answer what was received, then close

/// The epoll events a connection waits for.
#[inline]
fn interest(flags: u16) -> u32 {
    (if flags & EOF != 0 { 0 } else { libc::EPOLLIN as u32 }) | if flags & WANT_OUT != 0 { libc::EPOLLOUT as u32 } else { 0 }
}

struct Conn {
    fd: i32,
    gn: u32,
    flags: u16,
    inbuf: Option<Box<Vec<u8>>>,
    outbuf: Option<Box<Vec<u8>>>,
}

const GEN_MASK: u32 = 0x3FFF_FFFF;
const LIS_TAG: u64 = 1 << 63;
const PG_TAG: u64 = 1 << 62;

#[inline]
fn key_of(idx: u32, gn: u32) -> u64 {
    ((gn & GEN_MASK) as u64) << 32 | idx as u64
}

struct Clients {
    ep: i32,
    slab: Vec<Conn>,
    free: Vec<u32>,
    out: Vec<u8>,
    gz: libdeflater::Compressor,
    gzbuf: Vec<u8>,
}

fn status_line(status: u16) -> &'static [u8] {
    match status {
        200 => b"HTTP/1.1 200 OK\r\n",
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    }
}

fn ep_ctl(ep: i32, op: i32, fd: i32, events: u32, data: u64) {
    let mut ev = libc::epoll_event { events, u64: data };
    unsafe { libc::epoll_ctl(ep, op, fd, &mut ev) };
}

#[inline]
fn errno() -> i32 {
    std::io::Error::last_os_error().raw_os_error().unwrap_or(0)
}

impl Clients {
    #[inline]
    fn get(&self, key: u64) -> Option<usize> {
        let idx = key as u32 as usize;
        let c = self.slab.get(idx)?;
        (c.flags & USED != 0 && (c.gn & GEN_MASK) == (key >> 32) as u32).then_some(idx)
    }

    fn alloc(&mut self, fd: i32) -> u64 {
        let idx = match self.free.pop() {
            Some(i) => i as usize,
            None => {
                self.slab.push(Conn { fd: -1, gn: 0, flags: 0, inbuf: None, outbuf: None });
                self.slab.len() - 1
            }
        };
        let c = &mut self.slab[idx];
        c.fd = fd;
        c.flags = USED;
        key_of(idx as u32, c.gn)
    }

    fn close(&mut self, idx: usize) {
        let c = &mut self.slab[idx];
        if c.flags & USED == 0 {
            return;
        }
        unsafe { libc::close(c.fd) };
        c.fd = -1;
        c.flags = 0;
        c.gn = c.gn.wrapping_add(1);
        c.inbuf = None;
        c.outbuf = None;
        self.free.push(idx as u32);
    }

    /// Writes `data` (or queues it behind unsent bytes). Returns false if the connection was closed.
    fn send(&mut self, idx: usize, data: &[u8]) -> bool {
        let ep = self.ep;
        let c = &mut self.slab[idx];
        if let Some(ob) = c.outbuf.as_mut() {
            ob.extend_from_slice(data);
            return true;
        }
        let r = unsafe { libc::write(c.fd, data.as_ptr().cast(), data.len()) };
        if r as usize == data.len() {
            return true;
        }
        if r < 0 && errno() != libc::EAGAIN {
            self.close(idx);
            return false;
        }
        let w = r.max(0) as usize;
        c.outbuf = Some(Box::new(data[w..].to_vec()));
        c.flags |= WANT_OUT;
        ep_ctl(ep, libc::EPOLL_CTL_MOD, c.fd, interest(c.flags), key_of(idx as u32, c.gn));
        true
    }

    fn on_writable(&mut self, idx: usize) {
        let ep = self.ep;
        let c = &mut self.slab[idx];
        let Some(ob) = c.outbuf.as_mut() else { return };
        let r = unsafe { libc::write(c.fd, ob.as_ptr().cast(), ob.len()) };
        if r < 0 {
            if errno() != libc::EAGAIN {
                self.close(idx);
            }
            return;
        }
        ob.drain(..r as usize);
        if ob.is_empty() {
            c.outbuf = None;
            c.flags &= !WANT_OUT;
            if c.flags & (CLOSE | EOF) != 0 && c.flags & BUSY == 0 && c.inbuf.is_none() {
                self.close(idx);
                return;
            }
            ep_ctl(ep, libc::EPOLL_CTL_MOD, c.fd, interest(c.flags), key_of(idx as u32, c.gn));
        }
    }

    /// Sends a JSON response to the request a connection is serving. Connections with input waiting behind
    /// that request go on `resume`.
    fn respond(&mut self, key: u64, status: u16, body: &[u8], resume: &mut Vec<u64>) {
        let Some(idx) = self.get(key) else { return };
        let flags = self.slab[idx].flags;
        self.slab[idx].flags &= !(BUSY | CONT_SENT);
        let mut out = std::mem::take(&mut self.out);
        out.clear();
        out.extend_from_slice(status_line(status));
        out.extend_from_slice(b"Content-Type: application/json\r\n");
        let mut payload = body;
        if flags & GZIP != 0 && body.len() >= GZIP_MIN {
            let bound = self.gz.gzip_compress_bound(body.len());
            if self.gzbuf.len() < bound {
                self.gzbuf.resize(bound, 0);
            }
            if let Ok(n) = self.gz.gzip_compress(body, &mut self.gzbuf) {
                payload = &self.gzbuf[..n];
                out.extend_from_slice(b"Content-Encoding: gzip\r\n");
            }
        }
        if flags & CLOSE != 0 {
            out.extend_from_slice(b"Connection: close\r\n");
        }
        out.extend_from_slice(b"Content-Length: ");
        json::int(&mut out, payload.len() as i64);
        out.extend_from_slice(b"\r\n\r\n");
        out.extend_from_slice(payload);
        let alive = self.send(idx, &out);
        self.out = out;
        if !alive {
            return;
        }
        let c = &self.slab[idx];
        if flags & CLOSE != 0 {
            if c.outbuf.is_none() {
                self.close(idx);
            }
            return;
        }
        if c.inbuf.is_some() || flags & EOF != 0 {
            resume.push(key); // more input to serve, or a half-closed peer to close once all is answered
        }
    }

    /// Closes now, or once the unsent bytes are out.
    fn close_when_sent(&mut self, idx: usize) {
        if self.slab[idx].outbuf.is_none() {
            self.close(idx);
        } else {
            self.slab[idx].flags |= CLOSE;
        }
    }

    fn error(&mut self, key: u64, status: u16, msg: &str, resume: &mut Vec<u64>) {
        let mut b = Vec::with_capacity(48);
        b.extend_from_slice(b"{\"error\":\"");
        b.extend_from_slice(msg.as_bytes());
        b.extend_from_slice(b"\"}");
        self.respond(key, status, &b, resume);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Postgres connections

struct Create {
    user_id: i64,
    body: String,
    author: String,
}

enum Op {
    Sync,
    Health,
    Feed,
    Post(i64),
    Create(Box<Create>),
    Like { user_id: i64, post_id: i64 },
}

struct Pending {
    key: u64,
    op: Op,
    /// Times this read was sent again after its connection failed.
    tries: u8,
    /// A write retried alone (its own Sync) after its group rolled back.
    solo: bool,
}

impl Pending {
    fn new(key: u64, op: Op) -> Self {
        Pending { key, op, tries: 0, solo: false }
    }
    fn is_write(&self) -> bool {
        matches!(self.op, Op::Create(_) | Op::Like { .. })
    }
}

enum WRes {
    Created { id: [u8; 20], id_len: u8, ts: i64 },
    Liked(bool),
    /// SQLSTATE and whether the violated constraint is the post foreign key.
    Failed { post_fk: bool },
    Skipped,
}

struct Pg {
    fd: i32,
    alive: bool,
    write: bool,
    want_out: bool,
    retry_at: Instant,
    wbuf: Vec<u8>,
    rbuf: Vec<u8>,
    inflight: VecDeque<Pending>,
    /// Requests in flight, not counting Sync markers.
    load: usize,
    /// Rows of the read being answered, built as JSON.
    body: Vec<u8>,
    rows: u32,
    /// The row returned by the INSERT being answered.
    created: Option<([u8; 20], u8, i64)>,
    /// The current write group's results, answered once its Sync's ReadyForQuery (the commit) arrives.
    group: Vec<(Pending, WRes)>,
    /// An ErrorResponse arrived in the current Sync group.
    group_err: bool,
    /// The error came at commit time: nothing in the group can be trusted.
    group_fatal: bool,
}

struct Queues {
    read: Vec<Pending>,
    write: Vec<Pending>,
    resume: Vec<u64>,
}

/// One DataRow of a post query, appended as a JSON post object.
fn render_post(out: &mut Vec<u8>, row: &[u8]) -> Option<()> {
    let mut cols: [&[u8]; 5] = [&[]; 5];
    if row.len() < 2 || u16::from_be_bytes([row[0], row[1]]) != 5 {
        return None;
    }
    let mut p = 2;
    for c in cols.iter_mut() {
        let len = i32::from_be_bytes(row.get(p..p + 4)?.try_into().ok()?);
        p += 4;
        if len < 0 {
            return None;
        }
        *c = row.get(p..p + len as usize)?;
        p += len as usize;
    }
    if cols[2].len() != 8 {
        return None;
    }
    out.extend_from_slice(b"{\"id\":");
    out.extend_from_slice(cols[0]);
    out.extend_from_slice(b",\"body\":\"");
    json::esc(out, cols[1]);
    out.extend_from_slice(b"\",\"created_at\":\"");
    json::ts(out, i64::from_be_bytes(cols[2].try_into().ok()?));
    out.extend_from_slice(b"\",\"author\":\"");
    json::esc(out, cols[3]);
    out.extend_from_slice(b"\",\"like_count\":");
    out.extend_from_slice(cols[4]);
    out.push(b'}');
    Some(())
}

/// The RETURNING row of the INSERT into posts: id as text, created_at binary.
fn parse_created(row: &[u8]) -> Option<([u8; 20], u8, i64)> {
    if row.len() < 2 || u16::from_be_bytes([row[0], row[1]]) != 2 {
        return None;
    }
    let l0 = i32::from_be_bytes(row.get(2..6)?.try_into().ok()?);
    if !(1..=20).contains(&l0) {
        return None;
    }
    let l0 = l0 as usize;
    let id_txt = row.get(6..6 + l0)?;
    let p = 6 + l0;
    let l1 = i32::from_be_bytes(row.get(p..p + 4)?.try_into().ok()?);
    if l1 != 8 {
        return None;
    }
    let ts = i64::from_be_bytes(row.get(p + 4..p + 12)?.try_into().ok()?);
    let mut id = [0u8; 20];
    id[..l0].copy_from_slice(id_txt);
    Some((id, l0 as u8, ts))
}

fn error_fields(p: &[u8]) -> (bool, bool) {
    // (is foreign key violation 23503, constraint mentions post_id)
    let mut fk = false;
    let mut post = false;
    for f in p.split(|&c| c == 0) {
        if let Some((&t, v)) = f.split_first() {
            match t {
                b'C' => fk = v == b"23503",
                b'n' => post = memchr::memmem::find(v, b"post").is_some(),
                _ => {}
            }
        }
    }
    (fk, fk && post)
}

impl Pg {
    fn dead() -> Self {
        Pg {
            fd: -1,
            alive: false,
            write: false,
            want_out: false,
            retry_at: Instant::now(),
            wbuf: Vec::new(),
            rbuf: Vec::new(),
            inflight: VecDeque::new(),
            load: 0,
            body: Vec::with_capacity(8192),
            rows: 0,
            created: None,
            group: Vec::new(),
            group_err: false,
            group_fatal: false,
        }
    }

    fn push(&mut self, p: Pending) {
        if !matches!(p.op, Op::Sync) {
            self.load += 1;
        }
        self.inflight.push_back(p);
    }

    fn pop(&mut self) -> Option<Pending> {
        let p = self.inflight.pop_front()?;
        if !matches!(p.op, Op::Sync) {
            self.load -= 1;
        }
        Some(p)
    }

    fn encode(&mut self, p: &Pending) {
        match &p.op {
            Op::Feed => pg::enc_feed(&mut self.wbuf),
            Op::Post(id) => pg::enc_post(&mut self.wbuf, *id),
            Op::Health => pg::enc_health(&mut self.wbuf),
            Op::Create(c) => pg::enc_create(&mut self.wbuf, c.user_id, c.body.as_bytes()),
            Op::Like { user_id, post_id } => pg::enc_like(&mut self.wbuf, *user_id, *post_id),
            Op::Sync => pg::enc_sync(&mut self.wbuf),
        }
    }

    fn sync(&mut self) {
        pg::enc_sync(&mut self.wbuf);
        self.push(Pending::new(0, Op::Sync));
    }

    /// Handles one backend message. Err means the stream makes no sense: drop the connection.
    fn on_msg(&mut self, cl: &mut Clients, q: &mut Queues, t: u8, p: &[u8]) -> Result<(), ()> {
        match t {
            b'2' | b'1' | b'3' | b'n' | b'N' | b'S' | b'A' | b'K' => Ok(()),
            b'D' => {
                let head = self.inflight.front().ok_or(())?;
                match head.op {
                    Op::Feed => {
                        if self.rows == 0 {
                            self.body.clear();
                            self.body.extend_from_slice(b"{\"posts\":[");
                        } else {
                            self.body.push(b',');
                        }
                        render_post(&mut self.body, p).ok_or(())?;
                    }
                    Op::Post(_) => {
                        self.body.clear();
                        self.body.extend_from_slice(b"{\"post\":");
                        render_post(&mut self.body, p).ok_or(())?;
                        self.body.push(b'}');
                    }
                    Op::Create(_) => self.created = Some(parse_created(p).ok_or(())?),
                    Op::Health => {}
                    _ => return Err(()),
                }
                self.rows += 1;
                Ok(())
            }
            b'C' => {
                let head = self.pop().ok_or(())?;
                let rows = std::mem::take(&mut self.rows);
                match head.op {
                    Op::Feed => {
                        if rows == 0 {
                            self.body.clear();
                            self.body.extend_from_slice(b"{\"posts\":[");
                        }
                        self.body.extend_from_slice(b"]}");
                        cl.respond(head.key, 200, &self.body, &mut q.resume);
                    }
                    Op::Post(_) => {
                        if rows == 0 {
                            cl.error(head.key, 404, "post not found", &mut q.resume);
                        } else {
                            cl.respond(head.key, 200, &self.body, &mut q.resume);
                        }
                    }
                    Op::Health => {
                        let mut b = Vec::with_capacity(64);
                        b.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                        json::int(&mut b, START.get().map_or(0, |s| s.elapsed().as_secs()) as i64);
                        b.push(b'}');
                        cl.respond(head.key, 200, &b, &mut q.resume);
                    }
                    Op::Create(_) => {
                        let (id, id_len, ts) = self.created.take().ok_or(())?;
                        self.group.push((head, WRes::Created { id, id_len, ts }));
                    }
                    Op::Like { .. } => {
                        // "INSERT 0 1\0" or "INSERT 0 0\0"
                        let inserted = p.len() >= 2 && p[p.len() - 2] == b'1';
                        self.group.push((head, WRes::Liked(inserted)));
                    }
                    Op::Sync => return Err(()),
                }
                Ok(())
            }
            b'E' => {
                self.rows = 0;
                self.created = None;
                let (_, post_fk) = error_fields(p);
                match self.inflight.front().map(|h| matches!(h.op, Op::Sync)) {
                    None => return Err(()),
                    Some(true) => {
                        // failed at commit
                        self.group_err = true;
                        self.group_fatal = true;
                    }
                    Some(false) => {
                        let head = self.pop().ok_or(())?;
                        self.group_err = true;
                        if head.is_write() {
                            self.group.push((head, WRes::Failed { post_fk }));
                        } else {
                            eprintln!("read failed: {}", pg::error_text(p));
                            cl.error(head.key, 500, "internal server error", &mut q.resume);
                        }
                    }
                }
                Ok(())
            }
            b'Z' => {
                // everything up to this group's Sync marker was skipped after an error
                loop {
                    let p = self.pop().ok_or(())?;
                    if matches!(p.op, Op::Sync) {
                        break;
                    }
                    if p.is_write() {
                        self.group.push((p, WRes::Skipped));
                    } else {
                        retry_read(cl, q, p);
                    }
                }
                self.finish_group(cl, q);
                Ok(())
            }
            _ => Err(()),
        }
    }

    /// The group's Sync came back: its writes are committed (or rolled back, if anything failed).
    fn finish_group(&mut self, cl: &mut Clients, q: &mut Queues) {
        let err = std::mem::take(&mut self.group_err);
        let fatal = std::mem::take(&mut self.group_fatal);
        let mut b = Vec::with_capacity(700);
        for (p, r) in self.group.drain(..) {
            if fatal {
                cl.error(p.key, 500, "internal server error", &mut q.resume);
                continue;
            }
            match r {
                WRes::Failed { post_fk } => {
                    if post_fk {
                        cl.error(p.key, 404, "post not found", &mut q.resume);
                    } else {
                        cl.error(p.key, 500, "internal server error", &mut q.resume);
                    }
                }
                _ if err => {
                    // rolled back with the group: run again alone
                    let mut p = p;
                    p.solo = true;
                    q.write.push(p);
                }
                WRes::Created { id, id_len, ts } => {
                    let Op::Create(c) = &p.op else { continue };
                    b.clear();
                    b.extend_from_slice(b"{\"post\":{\"id\":");
                    b.extend_from_slice(&id[..id_len as usize]);
                    b.extend_from_slice(b",\"body\":\"");
                    json::esc(&mut b, c.body.as_bytes());
                    b.extend_from_slice(b"\",\"created_at\":\"");
                    json::ts(&mut b, ts);
                    b.extend_from_slice(b"\",\"author\":\"");
                    json::esc(&mut b, c.author.as_bytes());
                    b.extend_from_slice(b"\",\"like_count\":0}}");
                    cl.respond(p.key, 201, &b, &mut q.resume);
                }
                WRes::Liked(inserted) => {
                    let Op::Like { post_id, .. } = p.op else { continue };
                    b.clear();
                    b.extend_from_slice(b"{\"liked\":true,\"already_liked\":");
                    b.extend_from_slice(if inserted { b"false" } else { b"true" });
                    b.extend_from_slice(b",\"post_id\":");
                    json::int(&mut b, post_id);
                    b.push(b'}');
                    cl.respond(p.key, if inserted { 201 } else { 200 }, &b, &mut q.resume);
                }
                WRes::Skipped => {
                    // skipped without an error in the group cannot happen; be safe
                    cl.error(p.key, 500, "internal server error", &mut q.resume);
                }
            }
        }
    }
}

/// A read whose reply was lost (its connection failed, or an earlier read of its pack failed) is sent again,
/// at most 3 times. Reads have no side effects.
fn retry_read(cl: &mut Clients, q: &mut Queues, mut p: Pending) {
    if p.tries < 3 {
        p.tries += 1;
        q.read.push(p);
    } else if matches!(p.op, Op::Health) {
        cl.error_health(p.key, "database query failed", &mut q.resume);
    } else {
        cl.error(p.key, 500, "internal server error", &mut q.resume);
    }
}

impl Clients {
    fn error_health(&mut self, key: u64, msg: &str, resume: &mut Vec<u64>) {
        let mut b = Vec::with_capacity(96);
        b.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
        json::esc(&mut b, msg.as_bytes());
        b.extend_from_slice(b"\"}");
        self.respond(key, 503, &b, resume);
    }
}

// ---------------------------------------------------------------------------------------------------------
// the worker: one per core

struct Worker {
    ep: i32,
    lis: [i32; 2],
    cl: Clients,
    pgs: Vec<Pg>,
    n_read: usize,
    q: Queues,
    cfg: &'static Config,
    jwt: jwt::Verifier,
    rbuf: Vec<u8>,
    now_unix: f64,
    any_dead: bool,
}

struct Config {
    pg: pg::PgConfig,
    secret: Vec<u8>,
    host: Ipv4Addr,
    ports: [u16; 2],
}

fn set_int(fd: i32, level: i32, opt: i32, v: i32) {
    unsafe {
        libc::setsockopt(fd, level, opt, (&v as *const i32).cast(), 4);
    }
}

fn listener(host: Ipv4Addr, port: u16) -> i32 {
    unsafe {
        let fd = libc::socket(libc::AF_INET, libc::SOCK_STREAM | libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC, 0);
        assert!(fd >= 0, "socket: {}", std::io::Error::last_os_error());
        set_int(fd, libc::SOL_SOCKET, libc::SO_REUSEADDR, 1);
        set_int(fd, libc::SOL_SOCKET, libc::SO_REUSEPORT, 1);
        // A new connection reaches accept() with its first request already received.
        set_int(fd, libc::IPPROTO_TCP, libc::TCP_DEFER_ACCEPT, 10);
        // inherited by accepted sockets
        set_int(fd, libc::IPPROTO_TCP, libc::TCP_NODELAY, 1);
        let addr = libc::sockaddr_in {
            sin_family: libc::AF_INET as u16,
            sin_port: port.to_be(),
            sin_addr: libc::in_addr { s_addr: u32::from(host).to_be() },
            sin_zero: [0; 8],
        };
        let r = libc::bind(fd, (&addr as *const libc::sockaddr_in).cast(), std::mem::size_of_val(&addr) as u32);
        assert!(r == 0, "bind {host}:{port}: {}", std::io::Error::last_os_error());
        let r = libc::listen(fd, 65535);
        assert!(r == 0, "listen: {}", std::io::Error::last_os_error());
        fd
    }
}

fn unix_now() -> f64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_REALTIME_COARSE, &mut ts) };
    ts.tv_sec as f64 + ts.tv_nsec as f64 * 1e-9
}

/// Leading and trailing whitespace as JavaScript's String.prototype.trim sees it.
fn js_trim(s: &str) -> &str {
    s.trim_matches(|c: char| c.is_whitespace() || c == '\u{FEFF}')
}

impl Worker {
    fn new(cfg: &'static Config, n_read: usize, n_write: usize) -> Self {
        let ep = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
        assert!(ep >= 0);
        let mut w = Worker {
            ep,
            lis: [-1, -1],
            cl: Clients {
                ep,
                slab: Vec::with_capacity(1024),
                free: Vec::new(),
                out: Vec::with_capacity(16384),
                gz: libdeflater::Compressor::new(libdeflater::CompressionLvl::new(1).unwrap()),
                gzbuf: vec![0; 16384],
            },
            pgs: Vec::new(),
            n_read,
            q: Queues { read: Vec::new(), write: Vec::new(), resume: Vec::new() },
            cfg,
            jwt: jwt::Verifier::new(&cfg.secret),
            rbuf: vec![0; 65536],
            now_unix: unix_now(),
            any_dead: false,
        };
        for i in 0..n_read + n_write {
            let mut p = Pg::dead();
            p.write = i >= n_read;
            w.pgs.push(p);
            w.reconnect(i);
        }
        for (k, &port) in cfg.ports.iter().enumerate() {
            let fd = listener(cfg.host, port);
            ep_ctl(ep, libc::EPOLL_CTL_ADD, fd, libc::EPOLLIN as u32, LIS_TAG | k as u64);
            w.lis[k] = fd;
        }
        w
    }

    fn reconnect(&mut self, i: usize) {
        let pg = &mut self.pgs[i];
        match pg::connect(&self.cfg.pg) {
            Ok(fd) => {
                pg.fd = fd;
                pg.alive = true;
                pg.want_out = false;
                ep_ctl(self.ep, libc::EPOLL_CTL_ADD, fd, libc::EPOLLIN as u32, PG_TAG | i as u64);
            }
            Err(e) => {
                eprintln!("postgres connect failed: {e}");
                pg.retry_at = Instant::now() + Duration::from_secs(1);
                self.any_dead = true;
            }
        }
    }

    fn pg_fail(&mut self, i: usize, why: &str) {
        let pg = &mut self.pgs[i];
        if !pg.alive {
            return;
        }
        eprintln!("postgres connection lost: {why}");
        unsafe { libc::close(pg.fd) };
        pg.fd = -1;
        pg.alive = false;
        pg.want_out = false;
        pg.wbuf.clear();
        pg.rbuf.clear();
        pg.rows = 0;
        pg.created = None;
        pg.group_err = false;
        pg.group_fatal = false;
        pg.retry_at = Instant::now(); // try again at once, then once a second
        self.any_dead = true;
        // writes on a lost connection may or may not have committed: never sent twice
        for (p, _) in pg.group.drain(..) {
            self.cl.error(p.key, 500, "internal server error", &mut self.q.resume);
        }
        while let Some(p) = pg.pop() {
            match p.op {
                Op::Sync => {}
                _ if p.is_write() => self.cl.error(p.key, 500, "internal server error", &mut self.q.resume),
                _ => retry_read(&mut self.cl, &mut self.q, p),
            }
        }
    }

    fn run(&mut self) -> ! {
        let mut events = vec![libc::epoll_event { events: 0, u64: 0 }; MAX_EVENTS];
        loop {
            let timeout = if self.any_dead { 100 } else { -1 };
            let n = unsafe { libc::epoll_wait(self.ep, events.as_mut_ptr(), MAX_EVENTS as i32, timeout) };
            let t0 = Instant::now();
            self.now_unix = unix_now();
            let n = n.max(0) as usize;
            for ev in &events[..n] {
                let (e, d) = (ev.events, ev.u64);
                if d & LIS_TAG != 0 {
                    self.accept((d & 1) as usize);
                } else if d & PG_TAG != 0 {
                    let i = (d & 0xFFFF) as usize;
                    if e & libc::EPOLLOUT as u32 != 0 {
                        self.pg_write(i);
                    }
                    if e & (libc::EPOLLIN | libc::EPOLLERR | libc::EPOLLHUP) as u32 != 0 {
                        self.pg_read(i);
                    }
                } else if let Some(idx) = self.cl.get(d) {
                    if e & libc::EPOLLOUT as u32 != 0 {
                        self.cl.on_writable(idx);
                    }
                    if e & (libc::EPOLLIN | libc::EPOLLERR | libc::EPOLLHUP) as u32 != 0 {
                        self.client_read(d);
                    }
                }
            }
            self.after_pass();
            if self.any_dead {
                self.maybe_reconnect();
            }
            if n < MAX_EVENTS {
                let el = t0.elapsed();
                if el < WINDOW {
                    std::thread::sleep(WINDOW - el);
                }
            }
        }
    }

    fn after_pass(&mut self) {
        // input that waited behind an answered request
        while !self.q.resume.is_empty() {
            let keys = std::mem::take(&mut self.q.resume);
            for key in keys {
                let Some(idx) = self.cl.get(key) else { continue };
                let c = &mut self.cl.slab[idx];
                if c.flags & BUSY != 0 {
                    continue;
                }
                if let Some(buf) = c.inbuf.take() {
                    self.drive(key, &buf);
                } else if c.flags & EOF != 0 {
                    self.cl.close_when_sent(idx);
                }
            }
        }
        self.flush_queues();
        for i in 0..self.pgs.len() {
            if !self.pgs[i].wbuf.is_empty() && !self.pgs[i].want_out {
                self.pg_write(i);
            }
        }
    }

    fn maybe_reconnect(&mut self) {
        let now = Instant::now();
        self.any_dead = false;
        for i in 0..self.pgs.len() {
            if !self.pgs[i].alive {
                if self.pgs[i].retry_at <= now {
                    // one blocking attempt per pass
                    self.reconnect(i);
                    if !self.pgs[i].alive {
                        // failed: back off this thread's other attempts too
                        for p in self.pgs.iter_mut().filter(|p| !p.alive) {
                            p.retry_at = now + Duration::from_secs(1);
                        }
                        self.any_dead = true;
                        return;
                    }
                }
                self.any_dead |= !self.pgs[i].alive;
            }
        }
    }

    fn pick(&self, write: bool) -> Option<usize> {
        let range = if write { self.n_read..self.pgs.len() } else { 0..self.n_read };
        let mut best: Option<usize> = None;
        for i in range {
            let p = &self.pgs[i];
            if !p.alive {
                continue;
            }
            if write && p.load == 0 {
                return Some(i); // an idle write connection: the group never waits behind another commit
            }
            if best.is_none_or(|b| p.load < self.pgs[b].load) {
                best = Some(i);
            }
        }
        best
    }

    fn flush_queues(&mut self) {
        let mut reads = std::mem::take(&mut self.q.read);
        let mut it = reads.drain(..).peekable();
        while it.peek().is_some() {
            let Some(i) = self.pick(false) else {
                for p in it.by_ref() {
                    if matches!(p.op, Op::Health) {
                        self.cl.error_health(p.key, "no database connection", &mut self.q.resume);
                    } else {
                        self.cl.error(p.key, 500, "internal server error", &mut self.q.resume);
                    }
                }
                break;
            };
            let pg = &mut self.pgs[i];
            for p in it.by_ref().take(READ_PACK) {
                pg.encode(&p);
                pg.push(p);
            }
            pg.sync();
        }
        drop(it);
        if self.q.read.is_empty() {
            self.q.read = reads; // keep the allocation
        }

        let mut writes = std::mem::take(&mut self.q.write);
        let mut group = 0;
        let mut target: Option<usize> = None;
        for p in writes.drain(..) {
            if p.solo && group > 0 {
                // a write retried alone must not join the open group: close that group first
                if let Some(i) = target {
                    self.pgs[i].sync();
                }
                group = 0;
                target = None;
            }
            if target.is_none() {
                target = self.pick(true);
            }
            let Some(i) = target else {
                self.cl.error(p.key, 500, "internal server error", &mut self.q.resume);
                continue;
            };
            let pg = &mut self.pgs[i];
            pg.encode(&p);
            let solo = p.solo;
            pg.push(p);
            group += 1;
            if solo || group == WRITE_GROUP {
                pg.sync();
                group = 0;
                target = None;
            }
        }
        if let (Some(i), true) = (target, group > 0) {
            self.pgs[i].sync();
        }
        if self.q.write.is_empty() {
            self.q.write = writes;
        }
    }

    fn pg_write(&mut self, i: usize) {
        let pg = &mut self.pgs[i];
        if !pg.alive || pg.wbuf.is_empty() {
            return;
        }
        let r = unsafe { libc::write(pg.fd, pg.wbuf.as_ptr().cast(), pg.wbuf.len()) };
        if r < 0 {
            if errno() != libc::EAGAIN {
                let e = std::io::Error::last_os_error().to_string();
                self.pg_fail(i, &e);
            }
            return;
        }
        pg.wbuf.drain(..r as usize);
        let want = !pg.wbuf.is_empty();
        if want != pg.want_out {
            pg.want_out = want;
            let ev = if want { libc::EPOLLIN | libc::EPOLLOUT } else { libc::EPOLLIN } as u32;
            ep_ctl(self.ep, libc::EPOLL_CTL_MOD, pg.fd, ev, PG_TAG | i as u64);
        }
    }

    fn pg_read(&mut self, i: usize) {
        let pg = &mut self.pgs[i];
        if !pg.alive {
            return;
        }
        let mut rb = std::mem::take(&mut pg.rbuf);
        let mut failed: Option<String> = None;
        loop {
            if rb.capacity() - rb.len() < 32768 {
                rb.reserve(65536);
            }
            let spare = rb.capacity() - rb.len();
            let r = unsafe { libc::read(pg.fd, rb.as_mut_ptr().add(rb.len()).cast(), spare) };
            if r > 0 {
                unsafe { rb.set_len(rb.len() + r as usize) };
                if (r as usize) < spare {
                    break;
                }
            } else if r == 0 {
                failed = Some("closed by the server".into());
                break;
            } else {
                let e = errno();
                if e != libc::EAGAIN {
                    failed = Some(std::io::Error::from_raw_os_error(e).to_string());
                }
                break;
            }
        }
        // process whole messages (also the ones before a failure, e.g. a FATAL ErrorResponse)
        let mut pos = 0;
        while rb.len() - pos >= 5 {
            let t = rb[pos];
            let len = u32::from_be_bytes([rb[pos + 1], rb[pos + 2], rb[pos + 3], rb[pos + 4]]) as usize;
            if len < 4 {
                failed = Some("protocol error".into());
                break;
            }
            if rb.len() - pos < 1 + len {
                break;
            }
            let payload = &rb[pos + 5..pos + 1 + len];
            pos += 1 + len;
            if pg.on_msg(&mut self.cl, &mut self.q, t, payload).is_err() {
                failed = Some(format!("unexpected message {:?}", t as char));
                break;
            }
        }
        rb.drain(..pos);
        pg.rbuf = rb;
        if let Some(why) = failed {
            self.pg_fail(i, &why);
        }
    }

    fn accept(&mut self, k: usize) {
        for _ in 0..256 {
            let fd = unsafe {
                libc::accept4(self.lis[k], std::ptr::null_mut(), std::ptr::null_mut(), libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC)
            };
            if fd < 0 {
                return;
            }
            let key = self.cl.alloc(fd);
            ep_ctl(self.ep, libc::EPOLL_CTL_ADD, fd, libc::EPOLLIN as u32, key);
            // TCP_DEFER_ACCEPT: the first request is usually here already
            self.client_read(key);
        }
    }

    fn client_read(&mut self, key: u64) {
        let Some(idx) = self.cl.get(key) else { return };
        let fd = self.cl.slab[idx].fd;
        let mut buf = std::mem::take(&mut self.rbuf);
        let r = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
        if r <= 0 {
            if r == 0 && self.cl.slab[idx].flags & BUSY != 0 {
                // half-close with a request in flight: answer it (and anything queued behind it) first
                let c = &mut self.cl.slab[idx];
                c.flags |= EOF;
                ep_ctl(self.ep, libc::EPOLL_CTL_MOD, c.fd, interest(c.flags), key);
            } else if r == 0 || errno() != libc::EAGAIN {
                self.cl.close(idx);
            }
            self.rbuf = buf;
            return;
        }
        let n = r as usize;
        if let Some(mut pending) = self.cl.slab[idx].inbuf.take() {
            pending.extend_from_slice(&buf[..n]);
            self.rbuf = buf;
            if pending.len() > http::MAX_HEAD + http::MAX_BODY {
                self.cl.close(idx);
                return;
            }
            self.drive(key, &pending);
        } else {
            self.drive(key, &buf[..n]);
            self.rbuf = buf;
        }
    }

    /// Parses and serves the requests in `buf`, one at a time; what cannot be served yet waits in `inbuf`.
    fn drive(&mut self, key: u64, buf: &[u8]) {
        let mut off = 0;
        loop {
            let Some(idx) = self.cl.get(key) else { return };
            if off == buf.len() {
                let c = &self.cl.slab[idx];
                if c.flags & EOF != 0 && c.flags & BUSY == 0 {
                    self.cl.close_when_sent(idx);
                }
                return;
            }
            if self.cl.slab[idx].flags & (BUSY | CLOSE) != 0 {
                if self.cl.slab[idx].flags & BUSY != 0 {
                    self.cl.slab[idx].inbuf = Some(Box::new(buf[off..].to_vec()));
                }
                return;
            }
            match http::parse(&buf[off..]) {
                http::Parsed::Partial(expect) => {
                    let c = &mut self.cl.slab[idx];
                    if c.flags & EOF != 0 {
                        self.cl.close_when_sent(idx);
                        return;
                    }
                    c.inbuf = Some(Box::new(buf[off..].to_vec()));
                    if expect && c.flags & CONT_SENT == 0 {
                        c.flags |= CONT_SENT;
                        self.cl.send(idx, b"HTTP/1.1 100 Continue\r\n\r\n");
                    }
                    return;
                }
                http::Parsed::Bad => {
                    let c = &mut self.cl.slab[idx];
                    c.flags = (c.flags & !GZIP) | CLOSE;
                    self.cl.error(key, 400, "bad request", &mut self.q.resume);
                    return;
                }
                http::Parsed::Done(req, used) => {
                    off += used;
                    self.handle(key, idx, &req);
                }
            }
        }
    }

    fn handle(&mut self, key: u64, idx: usize, req: &http::Req) {
        {
            let c = &mut self.cl.slab[idx];
            c.flags &= !(GZIP | CLOSE);
            if req.gzip {
                c.flags |= GZIP;
            }
            if req.close {
                c.flags |= CLOSE;
            }
        }
        let path = req.path;
        let get = req.method == b"GET";
        let post = req.method == b"POST";
        if get && path == b"/feed" {
            return self.queue_read(key, idx, Op::Feed);
        }
        if let Some(rest) = path.strip_prefix(b"/posts/") {
            if get {
                return match jwt::parse_pos_int(rest) {
                    Some(id) => self.queue_read(key, idx, Op::Post(id)),
                    None => self.cl.error(key, 400, "invalid post id", &mut self.q.resume),
                };
            }
            if post {
                if let Some(id) = rest.strip_suffix(b"/like") {
                    let (user_id, _) = match self.auth(key, req) {
                        Some(a) => a,
                        None => return,
                    };
                    return match jwt::parse_pos_int(id) {
                        Some(post_id) => self.queue_write(key, idx, Op::Like { user_id, post_id }),
                        None => self.cl.error(key, 400, "invalid post id", &mut self.q.resume),
                    };
                }
            }
        } else if post && path == b"/posts" {
            let Some((user_id, author)) = self.auth(key, req) else { return };
            let mut body: Option<String> = None;
            let mut is_str = false;
            let parsed = json::parse_object(req.body, |k, v| {
                if k == b"body" {
                    match v {
                        json::Val::Str(s) => {
                            body = Some(s);
                            is_str = true;
                        }
                        _ => {
                            body = None;
                            is_str = false;
                        }
                    }
                }
            });
            if parsed.is_err() {
                return self.cl.error(key, 400, "malformed JSON body", &mut self.q.resume);
            }
            let body = match body {
                Some(b) if is_str => b,
                _ => return self.cl.error(key, 400, "body is required", &mut self.q.resume),
            };
            let t = js_trim(&body);
            if t.is_empty() {
                return self.cl.error(key, 400, "body is required", &mut self.q.resume);
            }
            if t.chars().count() > 500 {
                return self.cl.error(key, 400, "body must be at most 500 characters", &mut self.q.resume);
            }
            let body = if t.len() == body.len() { body } else { t.to_string() };
            return self.queue_write(key, idx, Op::Create(Box::new(Create { user_id, body, author })));
        } else if get && path == b"/health" {
            if !self.pgs[..self.n_read].iter().any(|p| p.alive) {
                return self.cl.error_health(key, "no database connection", &mut self.q.resume);
            }
            return self.queue_read(key, idx, Op::Health);
        }
        self.cl.error(key, 404, "not found", &mut self.q.resume)
    }

    fn auth(&mut self, key: u64, req: &http::Req) -> Option<(i64, String)> {
        let msg = match self.jwt.check(req.auth, self.now_unix) {
            jwt::Auth::Ok { user_id, username } => return Some((user_id, username)),
            jwt::Auth::Missing => "missing bearer token",
            jwt::Auth::Invalid => "invalid or expired token",
            jwt::Auth::BadPayload => "invalid token payload",
        };
        self.cl.error(key, 401, msg, &mut self.q.resume);
        None
    }

    fn queue_read(&mut self, key: u64, idx: usize, op: Op) {
        self.cl.slab[idx].flags |= BUSY;
        self.q.read.push(Pending::new(key, op));
    }

    fn queue_write(&mut self, key: u64, idx: usize, op: Op) {
        self.cl.slab[idx].flags |= BUSY;
        self.q.write.push(Pending::new(key, op));
    }
}

fn main() {
    START.get_or_init(Instant::now);
    let env = |k: &str| std::env::var(k).unwrap_or_else(|_| panic!("{k} is not set"));
    let host: Ipv4Addr = std::env::var("HOST").unwrap_or_else(|_| "0.0.0.0".into()).parse().expect("HOST must be IPv4");
    let port: u16 = env("PORT").parse().expect("PORT");
    let extra: u16 = env("EXTRA_PORT").parse().expect("EXTRA_PORT");
    let cfg: &'static Config = Box::leak(Box::new(Config {
        pg: pg::parse_url(&env("DATABASE_URL")),
        secret: env("JWT_SECRET").into_bytes(),
        host,
        ports: [port, extra],
    }));
    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };

    let threads = std::thread::available_parallelism().map_or(1, |n| n.get());
    let n_read = (PG_READ_TOTAL / threads).max(2);
    let n_write = (PG_WRITE_TOTAL / threads).max(1);
    eprintln!(
        "feed: {threads} threads, {n_read} read + {n_write} write Postgres connections each, ports {port} and {extra}"
    );
    let handles: Vec<_> = (0..threads)
        .map(|t| {
            std::thread::Builder::new()
                .name(format!("loop-{t}"))
                .spawn(move || Worker::new(cfg, n_read, n_write).run())
                .expect("spawn")
        })
        .collect();
    for h in handles {
        let _ = h.join();
    }
}
