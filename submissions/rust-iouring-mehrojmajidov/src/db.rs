//! The Postgres side of a loop: its connections, what each owes, and the reply stream.
//!
//! Reads and writes use separate connections, so a commit never holds up a read.
//! - Reads: the reads of a pass go to one connection as Bind/Execute pairs under one Sync. That is one packet
//!   out, one wake-up of one backend, and results that come back in full packets. Each read is answered as
//!   soon as its own CommandComplete arrives.
//! - Writes: the writes of a pass form one group under one Sync, which is one implicit transaction and one
//!   commit. Every write of the group is answered only when that Sync's ReadyForQuery arrives, which Postgres
//!   sends after the commit.

use crate::http::Status;
use crate::json;
use crate::pg;
use crate::reactor::{tag, Kind, Loop, INTERNAL, K_PG_RECV, K_PG_SEND, POST_NOT_FOUND, TAG_PG};
use crate::sys;
use crate::time::{format_pg_timestamp, write_pg_timestamp, Day};
use std::collections::VecDeque;
use std::mem;
use std::time::Duration;

/// Reads under one Sync at most. More go to the next connection.
const READ_BATCH: usize = 64;
/// Writes in one commit at most.
const WRITE_GROUP: usize = 128;
/// A read is sent again at most this often (after a failed batch or a lost connection).
const MAX_ATTEMPTS: u8 = 3;
/// A connection that owes replies and stays silent gets a lone Sync after this long, then with doubling gaps.
/// If a packet was lost, the extra traffic lets TCP notice before its retransmit timer would.
const NUDGE_FIRST_NS: u64 = 50_000_000;
const NUDGE_MAX_NS: u64 = 800_000_000;
/// After this much silence a connection that owes replies is declared lost.
const WATCHDOG_NS: u64 = 10_000_000_000;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(1);
/// With no connection of the kind a request needs, it waits this long for a reconnect before it is refused.
const OUTAGE_GRACE_NS: u64 = 2_000_000_000;
/// No single reply message comes near this; a connection that needs more is not speaking the protocol.
const MAX_REPLY_BUFFER: usize = 16 << 20;

pub enum Slot {
    Op(u32),
    /// A Sync we sent; Postgres answers it with ReadyForQuery.
    Sync,
}

pub struct PgConn {
    /// -1 while the connection is down.
    pub fd: i32,
    pub writer: bool,
    out: Vec<u8>,
    out_at: usize,
    wants_out: bool,
    inb: Vec<u8>,
    in_len: usize,
    // Ring: a receive is armed into `inb` (with `rx_space` bytes of room), and a send of `fly` is in flight.
    // The kernel uses both buffers until the completions arrive, even after the connection is dropped.
    rx: bool,
    rx_space: usize,
    tx: bool,
    fly: Vec<u8>,
    fly_at: usize,
    /// Ring: the descriptor of a dropped connection, kept open until its receive and its send have reported
    /// back. A request still queued for it names the descriptor by number; closed earlier, the number could go
    /// to a new socket and the request would then read or write that one.
    old_fd: i32,
    /// The replies Postgres owes on this connection, in order.
    queue: VecDeque<Slot>,
    /// Requests among them (the connection's load).
    owed: usize,
    /// The response being built from the rows of the request at the front.
    body: Vec<u8>,
    rows: u32,
    /// The date text of the feed being written; see `Day`.
    day: Day,
    // The frame (everything up to the next Sync) being answered:
    done: Vec<u32>,
    skipped: Vec<u32>,
    failed: Option<u32>,
    frame_error: bool,
    commit_failed: bool,
    last_rx_ns: u64,
    owed_since_ns: u64,
    last_nudge_ns: u64,
    nudge_gap_ns: u64,
    retry_at_ns: u64,
}

impl PgConn {
    fn new(writer: bool) -> PgConn {
        PgConn {
            fd: -1,
            writer,
            out: Vec::with_capacity(8 * 1024),
            out_at: 0,
            wants_out: false,
            // A read connection's buffer holds the whole reply to a full batch (64 feeds are about 175 KB),
            // so one receive takes it and the pass after it is on time. Untouched pages cost nothing.
            inb: vec![0; if writer { 64 * 1024 } else { 256 * 1024 }],
            in_len: 0,
            rx: false,
            rx_space: 0,
            tx: false,
            fly: Vec::with_capacity(8 * 1024),
            fly_at: 0,
            old_fd: -1,
            queue: VecDeque::new(),
            owed: 0,
            body: Vec::with_capacity(8 * 1024),
            rows: 0,
            day: Day::new(),
            done: Vec::new(),
            skipped: Vec::new(),
            failed: None,
            frame_error: false,
            commit_failed: false,
            last_rx_ns: 0,
            owed_since_ns: 0,
            last_nudge_ns: 0,
            nudge_gap_ns: NUDGE_FIRST_NS,
            retry_at_ns: 0,
        }
    }

    fn up(&self) -> bool {
        self.fd >= 0
    }

    /// Ring: closes a dropped connection's descriptor once the ring holds nothing for it any more.
    fn release(&mut self) {
        if self.old_fd >= 0 && !self.rx && !self.tx {
            unsafe { sys::close(self.old_fd) };
            self.old_fd = -1;
        }
    }

    /// Marks the start of a stretch in which this connection owes replies.
    fn begin_owing(&mut self, now_ns: u64) {
        if self.queue.is_empty() {
            self.owed_since_ns = now_ns;
            self.nudge_gap_ns = NUDGE_FIRST_NS;
        }
    }
}

/// Copies a number's digits: up to 16 bytes as two moves that may overlap, without a call.
///
/// # Safety
/// `at` has room for `bytes`.
#[inline(always)]
unsafe fn put_short(at: *mut u8, bytes: &[u8]) -> *mut u8 {
    let (from, n) = (bytes.as_ptr(), bytes.len());
    if n >= 8 {
        if n <= 16 {
            (at as *mut u64).write_unaligned((from as *const u64).read_unaligned());
            (at.add(n - 8) as *mut u64).write_unaligned((from.add(n - 8) as *const u64).read_unaligned());
        } else {
            core::ptr::copy_nonoverlapping(from, at, n);
        }
    } else if n >= 4 {
        (at as *mut u32).write_unaligned((from as *const u32).read_unaligned());
        (at.add(n - 4) as *mut u32).write_unaligned((from.add(n - 4) as *const u32).read_unaligned());
    } else if n >= 2 {
        (at as *mut u16).write_unaligned((from as *const u16).read_unaligned());
        (at.add(n - 2) as *mut u16).write_unaligned((from.add(n - 2) as *const u16).read_unaligned());
    } else if n == 1 {
        *at = *from;
    }
    at.add(n)
}

fn post_json(out: &mut Vec<u8>, row: &[u8], day: &mut Day) -> Option<()> {
    let [id, body, created_at, username, like_count] = pg::columns::<5>(row)?;
    let micros = i64::from_be_bytes(created_at.try_into().ok()?);
    // One reservation for the whole row, then plain writes. The fixed text is 83 bytes, and `escape_to` wants
    // six bytes per byte of a string and 16 to spare.
    out.reserve(128 + id.len() + like_count.len() + 6 * (body.len() + username.len()));
    // SAFETY: every write below stays inside what was reserved.
    unsafe {
        let start = out.as_mut_ptr().add(out.len());
        let mut at = start;
        macro_rules! put {
            ($bytes:expr) => {{
                let bytes: &[u8] = $bytes;
                core::ptr::copy_nonoverlapping(bytes.as_ptr(), at, bytes.len());
                at = at.add(bytes.len());
            }};
        }
        put!(b"{\"id\":");
        at = put_short(at, id);
        put!(b",\"body\":\"");
        at = json::escape_to(at, body);
        put!(b"\",\"created_at\":\"");
        write_pg_timestamp(day, micros, at);
        at = at.add(24);
        put!(b"\",\"author\":\"");
        at = json::escape_to(at, username);
        put!(b"\",\"like_count\":");
        at = put_short(at, like_count);
        put!(b"}");
        out.set_len(out.len() + at.offset_from(start) as usize);
    }
    Some(())
}

impl Loop {
    pub fn connect_postgres(&mut self) {
        for i in 0..self.cfg.readers + self.cfg.writers {
            self.pgs.push(PgConn::new(i >= self.cfg.readers));
        }
        self.connect_missing();
    }

    /// Start-up: connects every connection that is down, in order, and stops at the first that fails (the
    /// database is not reachable; the caller or the timer tries again).
    pub fn connect_missing(&mut self) {
        for ci in 0..self.pgs.len() {
            if !self.pgs[ci].up() && !self.reconnect(ci) {
                break;
            }
        }
    }

    /// A connection of each kind that is configured is up: every kind of request can be served.
    pub fn postgres_up(&self) -> bool {
        [true, false].iter().all(|&writer| !self.pgs.iter().any(|p| p.writer == writer) || self.pgs.iter().any(|p| p.writer == writer && p.up()))
    }

    fn reconnect(&mut self, ci: usize) -> bool {
        match pg::connect(&self.cfg.pg, &mut self.scram, CONNECT_TIMEOUT) {
            Ok(fd) => {
                let p = &mut self.pgs[ci];
                p.fd = fd;
                p.last_rx_ns = self.now_ns;
                if self.ring.is_some() {
                    p.fly.clear();
                    self.pg_arm(ci);
                } else {
                    self.watch(sys::EPOLL_CTL_ADD, fd, TAG_PG | ci as u64, sys::EPOLLIN | sys::EPOLLRDHUP);
                }
                true
            }
            Err(e) => {
                eprintln!("feed: Postgres connection failed: {e}");
                self.pgs[ci].retry_at_ns = self.now_ns + 1_000_000_000;
                false
            }
        }
    }

    pub fn pg_event(&mut self, ci: usize, bits: u32) {
        if !self.pgs[ci].up() {
            return;
        }
        if bits & (sys::EPOLLERR | sys::EPOLLHUP) != 0 {
            return self.pg_fail(ci, "socket error");
        }
        if bits & sys::EPOLLOUT != 0 {
            self.pg_flush(ci);
        }
        if bits & (sys::EPOLLIN | sys::EPOLLRDHUP) != 0 && self.pgs[ci].up() {
            self.pg_readable(ci);
        }
    }

    /// Epoll: reads what Postgres sent.
    fn pg_readable(&mut self, ci: usize) {
        loop {
            if !self.pg_room(ci) {
                return;
            }
            let p = &mut self.pgs[ci];
            let space = p.inb.len() - p.in_len;
            let n = unsafe { sys::recv(p.fd, p.inb[p.in_len..].as_mut_ptr().cast(), space, 0) };
            if n <= 0 {
                if n < 0 && matches!(sys::errno(), sys::EAGAIN | sys::EINTR) {
                    return;
                }
                return self.pg_fail(ci, if n == 0 { "closed by Postgres" } else { "read error" });
            }
            if !self.pg_ingest(ci, n as usize) || (n as usize) < space {
                // dropped, or the socket had less than we asked for: it is drained
                return;
            }
        }
    }

    /// Makes sure the reply buffer has room. False: it could not grow further and the connection was dropped.
    fn pg_room(&mut self, ci: usize) -> bool {
        let p = &mut self.pgs[ci];
        if p.in_len == p.inb.len() {
            if p.inb.len() >= MAX_REPLY_BUFFER {
                self.pg_fail(ci, "oversized reply");
                return false;
            }
            let doubled = p.inb.len() * 2;
            p.inb.resize(doubled, 0);
        }
        true
    }

    /// `n` more bytes of the reply stream are in the buffer: handles the whole messages among them.
    /// False: the stream made no sense and the connection was dropped.
    fn pg_ingest(&mut self, ci: usize, n: usize) -> bool {
        let p = &mut self.pgs[ci];
        let mut inb = mem::take(&mut p.inb);
        let len = p.in_len + n;
        p.last_rx_ns = self.now_ns;
        p.nudge_gap_ns = NUDGE_FIRST_NS;
        let mut at = 0;
        let mut problem = None;
        while len - at >= 5 {
            let size = u32::from_be_bytes([inb[at + 1], inb[at + 2], inb[at + 3], inb[at + 4]]) as usize;
            if size < 4 {
                problem = Some("bad message length");
                break;
            }
            if len - at < 1 + size {
                break;
            }
            if self.pg_message(ci, inb[at], &inb[at + 5..at + 1 + size]).is_none() {
                problem = Some("unexpected reply");
                break;
            }
            at += 1 + size;
        }
        if problem.is_none() {
            inb.copy_within(at..len, 0);
        }
        let p = &mut self.pgs[ci];
        p.inb = inb;
        if let Some(why) = problem {
            self.pg_fail(ci, why);
            return false;
        }
        p.in_len = len - at;
        true
    }

    /// Ring: arms one receive into the free part of the reply buffer.
    fn pg_arm(&mut self, ci: usize) {
        if !self.pg_room(ci) {
            return;
        }
        let p = &mut self.pgs[ci];
        p.rx = true;
        p.rx_space = p.inb.len() - p.in_len;
        let at = unsafe { p.inb.as_mut_ptr().add(p.in_len) };
        self.ring.as_mut().unwrap().receive(p.fd, at, p.rx_space, tag(K_PG_RECV, ci));
    }

    /// Asks for the records of the first operations a connection's next replies belong to.
    #[inline]
    pub fn expect_replies(&self, ci: usize) {
        if let Some(p) = self.pgs.get(ci) {
            for slot in p.queue.iter().take(4) {
                if let Slot::Op(op) = slot {
                    self.expect_op(*op);
                }
            }
        }
    }

    pub fn ring_pg_received(&mut self, ci: usize, res: i32) {
        let p = &mut self.pgs[ci];
        p.rx = false;
        if !p.up() {
            // the connection was dropped while this receive was out
            return p.release();
        }
        if res <= 0 {
            return self.pg_fail(ci, if res == 0 { "closed by Postgres" } else { "read error" });
        }
        if res as usize == p.rx_space {
            self.hurry = true;
        }
        if self.pg_ingest(ci, res as usize) {
            self.pg_arm(ci);
        }
    }

    pub fn ring_pg_sent(&mut self, ci: usize, res: i32) {
        let p = &mut self.pgs[ci];
        p.tx = false;
        if !p.up() {
            return p.release();
        }
        if res <= 0 {
            return self.pg_fail(ci, "write error");
        }
        p.fly_at += res as usize;
        if p.fly_at < p.fly.len() {
            // a short count: the socket failed part-way, and sending the rest reports why
            p.tx = true;
            let at = unsafe { p.fly.as_ptr().add(p.fly_at) };
            return self.ring.as_mut().unwrap().send(p.fd, at, p.fly.len() - p.fly_at, tag(K_PG_SEND, ci));
        }
        p.fly.clear();
        self.pg_flush(ci);
    }

    /// One message of the reply stream. `None`: the stream no longer makes sense and the connection is dropped.
    fn pg_message(&mut self, ci: usize, ty: u8, payload: &[u8]) -> Option<()> {
        match ty {
            b'D' => {
                let p = &mut self.pgs[ci];
                let op = match p.queue.front() {
                    Some(Slot::Op(op)) => *op,
                    _ => return None,
                };
                let o = &mut self.ops[op as usize];
                match o.kind {
                    Kind::Feed => {
                        if p.rows == 0 {
                            p.body.extend_from_slice(b"{\"posts\":[");
                            p.day = Day::new();
                        } else {
                            p.body.push(b',');
                        }
                        post_json(&mut p.body, payload, &mut p.day)?;
                    }
                    Kind::Post => {
                        p.body.extend_from_slice(b"{\"post\":");
                        post_json(&mut p.body, payload, &mut Day::new())?;
                    }
                    Kind::Create => {
                        let [id, created_at] = pg::columns::<2>(payload)?;
                        o.new_id.get_mut(..id.len())?.copy_from_slice(id);
                        o.new_id_len = id.len() as u8;
                        o.created_at = i64::from_be_bytes(created_at.try_into().ok()?);
                    }
                    Kind::Health | Kind::Like => {}
                }
                p.rows += 1;
            }
            // The end of a statement's reply: PortalSuspended after a read's full count of rows, CommandComplete
            // otherwise (EmptyQueryResponse is never sent to us).
            b'C' | b's' | b'I' => {
                let p = &mut self.pgs[ci];
                let Some(Slot::Op(op)) = p.queue.pop_front() else {
                    return None;
                };
                // Further down the queue: the record of the operation four replies on, and the connection's
                // slot of the one two replies on, whose record was asked for two replies ago.
                let op_at = |k: usize| match p.queue.get(k) {
                    Some(Slot::Op(later)) => Some(*later),
                    _ => None,
                };
                let (near, far) = (op_at(1), op_at(3));
                if let Some(later) = far {
                    self.expect_op(later);
                }
                if let Some(later) = near {
                    let fd = self.ops[later as usize].fd;
                    self.expect(fd);
                }
                let p = &mut self.pgs[ci];
                p.owed -= 1;
                let rows = mem::take(&mut p.rows);
                match self.ops[op as usize].kind {
                    Kind::Feed | Kind::Post => {
                        let mut body = mem::take(&mut p.body);
                        if self.ops[op as usize].kind == Kind::Feed {
                            body.extend_from_slice(if rows == 0 { b"{\"posts\":[]}" } else { b"]}" });
                            self.finish(op, Status::Ok, &body);
                        } else if rows == 0 {
                            self.finish(op, Status::NotFound, POST_NOT_FOUND);
                        } else {
                            body.push(b'}');
                            self.finish(op, Status::Ok, &body);
                        }
                        body.clear();
                        self.pgs[ci].body = body;
                    }
                    Kind::Health => self.finish_health(op, None),
                    Kind::Like => {
                        self.ops[op as usize].inserted = payload == b"INSERT 0 1\0";
                        p.done.push(op);
                    }
                    Kind::Create => p.done.push(op),
                }
            }
            b'E' => {
                let (severity, code, message) = pg::error_fields(payload);
                if severity == b"FATAL" || severity == b"PANIC" {
                    eprintln!("feed: Postgres: {}", String::from_utf8_lossy(message));
                    return None;
                }
                let p = &mut self.pgs[ci];
                p.frame_error = true;
                p.body.clear();
                p.rows = 0;
                match p.queue.front() {
                    Some(Slot::Op(_)) => {
                        // This request failed. Postgres skips everything else up to the Sync.
                        let Some(Slot::Op(op)) = p.queue.pop_front() else { unreachable!() };
                        p.owed -= 1;
                        let state = &mut self.ops[op as usize].sqlstate;
                        if code.len() == 5 {
                            state.copy_from_slice(code);
                        }
                        p.failed = Some(op);
                        while let Some(Slot::Op(_)) = p.queue.front() {
                            let Some(Slot::Op(next)) = p.queue.pop_front() else { unreachable!() };
                            p.owed -= 1;
                            p.skipped.push(next);
                        }
                    }
                    // The error came from the Sync itself: the commit failed.
                    Some(Slot::Sync) => p.commit_failed = true,
                    None => return None,
                }
            }
            b'Z' => {
                let Some(Slot::Sync) = self.pgs[ci].queue.pop_front() else {
                    return None;
                };
                self.finish_frame(ci);
            }
            // BindComplete, ParseComplete, CloseComplete, NoData, NoticeResponse, ParameterStatus, NotificationResponse
            b'2' | b'1' | b'3' | b'n' | b'N' | b'S' | b'A' => {}
            _ => return None,
        }
        Some(())
    }

    /// A Sync's ReadyForQuery arrived: its implicit transaction is over, committed unless something failed.
    fn finish_frame(&mut self, ci: usize) {
        let p = &mut self.pgs[ci];
        let (done, skipped) = (mem::take(&mut p.done), mem::take(&mut p.skipped));
        let (failed, error, commit_failed) = (p.failed.take(), mem::take(&mut p.frame_error), mem::take(&mut p.commit_failed));
        if !error {
            for &op in &done {
                self.finish_committed(op);
            }
        } else {
            if let Some(op) = failed {
                self.finish_failed(op);
            }
            for &op in done.iter().chain(&skipped) {
                if commit_failed {
                    self.finish(op, Status::Internal, INTERNAL);
                } else {
                    // Rolled back with the group, or never run: each goes again.
                    self.requeue(op);
                }
            }
        }
        // hand the (empty) vectors back so their allocations are reused
        let p = &mut self.pgs[ci];
        let (mut done, mut skipped) = (done, skipped);
        done.clear();
        skipped.clear();
        p.done = done;
        p.skipped = skipped;
    }

    fn finish_committed(&mut self, op: u32) {
        let mut body = mem::take(&mut self.scratch);
        body.clear();
        let o = &self.ops[op as usize];
        let status = match o.kind {
            Kind::Like => {
                body.extend_from_slice(b"{\"liked\":true,\"already_liked\":");
                body.extend_from_slice(if o.inserted { b"false" } else { b"true" });
                body.extend_from_slice(b",\"post_id\":");
                json::push_u64(&mut body, o.id);
                body.push(b'}');
                if o.inserted { Status::Created } else { Status::Ok }
            }
            _ => {
                body.extend_from_slice(b"{\"post\":{\"id\":");
                body.extend_from_slice(&o.new_id[..o.new_id_len as usize]);
                body.extend_from_slice(b",\"body\":\"");
                json::escape_into(&mut body, &o.text[..o.name_at]);
                body.extend_from_slice(b"\",\"created_at\":\"");
                body.extend_from_slice(&format_pg_timestamp(o.created_at));
                body.extend_from_slice(b"\",\"author\":\"");
                json::escape_into(&mut body, &o.text[o.name_at..]);
                body.extend_from_slice(b"\",\"like_count\":0}}");
                Status::Created
            }
        };
        self.finish(op, status, &body);
        self.scratch = body;
    }

    fn finish_failed(&mut self, op: u32) {
        let o = &self.ops[op as usize];
        match o.kind {
            // A like on a post that does not exist fails its foreign key.
            Kind::Like if &o.sqlstate == b"23503" => self.finish(op, Status::NotFound, POST_NOT_FOUND),
            Kind::Health => self.finish_health(op, Some("query failed")),
            _ => self.finish(op, Status::Internal, INTERNAL),
        }
    }

    fn finish_health(&mut self, op: u32, error: Option<&str>) {
        let mut body = mem::take(&mut self.scratch);
        body.clear();
        match error {
            None => {
                body.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                json::push_u64(&mut body, (self.now_ns - self.cfg.started_ns) / 1_000_000_000);
                body.push(b'}');
                self.finish(op, Status::Ok, &body);
            }
            Some(e) => {
                body.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
                json::escape_into(&mut body, e.as_bytes());
                body.extend_from_slice(b"\"}");
                self.finish(op, Status::Unavailable, &body);
            }
        }
        self.scratch = body;
    }

    /// A request that could not be sent or lost its connection before any reply.
    fn finish_unanswered(&mut self, op: u32) {
        match self.ops[op as usize].kind {
            Kind::Health => self.finish_health(op, Some("no database connection")),
            _ => self.finish(op, Status::Internal, INTERNAL),
        }
    }

    /// Sends a request again that Postgres did not run (or rolled back): a read on another batch,
    /// a write alone between two Syncs so that it commits or fails on its own.
    fn requeue(&mut self, op: u32) {
        let o = &mut self.ops[op as usize];
        if o.fd < 0 {
            return self.finish(op, Status::Internal, INTERNAL);
        }
        o.attempts += 1;
        if o.attempts > MAX_ATTEMPTS {
            return self.finish_unanswered(op);
        }
        if o.kind.is_write() {
            o.solo = true;
            self.write_q.push_back(op);
        } else {
            self.read_q.push_back(op);
        }
    }

    fn pg_fail(&mut self, ci: usize, why: &str) {
        let p = &mut self.pgs[ci];
        if !p.up() {
            return;
        }
        eprintln!("feed: Postgres connection lost ({why}), {} replies owed", p.owed);
        if self.ring.is_some() {
            // completes the receive and the send the ring may hold for this socket (`rx`, `tx`)
            unsafe { sys::shutdown(p.fd, sys::SHUT_RDWR) };
            p.old_fd = p.fd;
            p.release();
        } else {
            unsafe { sys::close(p.fd) };
        }
        p.fd = -1;
        p.retry_at_ns = self.now_ns;
        p.out.clear();
        p.out_at = 0;
        p.wants_out = false;
        p.in_len = 0;
        p.body.clear();
        p.rows = 0;
        p.owed = 0;
        p.frame_error = false;
        p.commit_failed = false;
        let queue = mem::take(&mut p.queue);
        let done = mem::take(&mut p.done);
        let skipped = mem::take(&mut p.skipped);
        let failed = p.failed.take();

        if let Some(op) = failed {
            self.finish_failed(op);
        }
        // Writes that reached CommandComplete may have been committed: never sent twice, they answer 500.
        for op in done {
            self.finish(op, Status::Internal, INTERNAL);
        }
        let unanswered = skipped.into_iter().chain(queue.into_iter().filter_map(|s| match s {
            Slot::Op(op) => Some(op),
            Slot::Sync => None,
        }));
        for op in unanswered {
            if self.ops[op as usize].kind.is_write() {
                self.finish(op, Status::Internal, INTERNAL);
            } else {
                self.requeue(op);
            }
        }
    }

    fn encode(&mut self, ci: usize, op: u32) {
        let o = &self.ops[op as usize];
        let out = &mut self.pgs[ci].out;
        match o.kind {
            Kind::Feed => pg::feed(out),
            Kind::Post => pg::post(out, o.id),
            Kind::Health => pg::health(out),
            Kind::Like => pg::like(out, o.user, o.id),
            Kind::Create => pg::create(out, o.user, &o.text[..o.name_at]),
        }
        let p = &mut self.pgs[ci];
        p.queue.push_back(Slot::Op(op));
        p.owed += 1;
    }

    fn end_frame(&mut self, ci: usize) {
        let p = &mut self.pgs[ci];
        pg::sync(&mut p.out);
        p.queue.push_back(Slot::Sync);
    }

    /// Sends what this pass collected: reads as batches, writes as one group per free write connection.
    pub fn dispatch(&mut self) {
        // Off by default (`FEED_READS_IN_FLIGHT`). With a limit on the reads at Postgres, a backlog waits here,
        // where its order can still be chosen: once the oldest read has waited `backlog_age_ns`, the newest go
        // first and every eighth is taken from the old end. A burst then makes its own requests late, not every
        // request that arrives while it drains. Local stall tests showed no gain, so it stays an experiment.
        let limit = self.cfg.reads_in_flight;
        let mut flying = if limit == 0 { 0 } else { self.pgs.iter().filter(|p| !p.writer).map(|p| p.owed).sum() };
        let newest_first = limit != 0
            && self.read_q.front().is_some_and(|&op| self.now_ns.saturating_sub(self.ops[op as usize].arrived_ns) >= self.cfg.backlog_age_ns);
        let mut taken = 0usize;
        while !self.read_q.is_empty() && (limit == 0 || flying < limit) {
            let reader = self.pgs.iter().enumerate().filter(|(_, p)| !p.writer && p.up()).min_by_key(|(_, p)| p.owed);
            let Some(ci) = reader.map(|(ci, _)| ci) else {
                if self.now_ns.saturating_sub(self.reader_up_ns) > OUTAGE_GRACE_NS {
                    while let Some(op) = self.read_q.pop_front() {
                        self.finish_unanswered(op);
                    }
                }
                break;
            };
            self.pgs[ci].begin_owing(self.now_ns);
            for _ in 0..READ_BATCH {
                if limit != 0 && flying >= limit {
                    break;
                }
                taken += 1;
                let next = if newest_first && taken % 8 != 0 { self.read_q.pop_back() } else { self.read_q.pop_front() };
                let Some(op) = next else { break };
                if self.ops[op as usize].fd < 0 {
                    self.finish(op, Status::Internal, INTERNAL);
                    continue;
                }
                self.encode(ci, op);
                flying += 1;
            }
            self.end_frame(ci);
            self.pg_flush(ci);
        }

        while !self.write_q.is_empty() {
            // A write connection carries one group at a time: while its commit is out, new writes wait
            // and leave together as the next group.
            let Some(ci) = self.pgs.iter().position(|p| p.writer && p.up() && p.queue.is_empty()) else {
                if self.now_ns.saturating_sub(self.writer_up_ns) > OUTAGE_GRACE_NS {
                    while let Some(op) = self.write_q.pop_front() {
                        self.finish_unanswered(op);
                    }
                }
                break;
            };
            self.pgs[ci].begin_owing(self.now_ns);
            let mut in_frame = 0;
            for _ in 0..WRITE_GROUP {
                let Some(op) = self.write_q.pop_front() else { break };
                if self.ops[op as usize].fd < 0 {
                    self.finish(op, Status::Internal, INTERNAL);
                    continue;
                }
                let solo = self.ops[op as usize].solo;
                if solo && in_frame > 0 {
                    self.end_frame(ci);
                    in_frame = 0;
                }
                self.encode(ci, op);
                in_frame += 1;
                if solo {
                    self.end_frame(ci);
                    in_frame = 0;
                }
            }
            if in_frame > 0 {
                self.end_frame(ci);
            }
            self.pg_flush(ci);
        }
    }

    fn pg_flush(&mut self, ci: usize) {
        let p = &mut self.pgs[ci];
        if !p.up() {
            return;
        }
        if let Some(ring) = self.ring.as_mut() {
            // One send in flight per connection: the kernel reads `fly` until the completion arrives, and
            // what is encoded meanwhile collects in `out` (`ring_pg_sent` sends it on).
            if !p.tx && !p.out.is_empty() {
                mem::swap(&mut p.out, &mut p.fly);
                p.fly_at = 0;
                p.tx = true;
                ring.send(p.fd, p.fly.as_ptr(), p.fly.len(), tag(K_PG_SEND, ci));
            }
            return;
        }
        while p.out_at < p.out.len() {
            let rest = &p.out[p.out_at..];
            let n = unsafe { sys::write(p.fd, rest.as_ptr().cast(), rest.len()) };
            if n < 0 {
                match sys::errno() {
                    sys::EINTR => continue,
                    sys::EAGAIN => break,
                    _ => return self.pg_fail(ci, "write error"),
                }
            }
            p.out_at += n as usize;
        }
        let pending = p.out_at < p.out.len();
        if !pending {
            p.out.clear();
            p.out_at = 0;
        }
        if pending != p.wants_out {
            p.wants_out = pending;
            let events = sys::EPOLLIN | sys::EPOLLRDHUP | if pending { sys::EPOLLOUT } else { 0 };
            let fd = p.fd;
            self.watch(sys::EPOLL_CTL_MOD, fd, TAG_PG | ci as u64, events);
        }
    }

    /// Runs once per pass: nudges and the watchdog for silent connections, and reconnects.
    pub fn timers(&mut self) {
        let now = self.now_ns;
        for ci in 0..self.pgs.len() {
            let p = &mut self.pgs[ci];
            if p.up() {
                if p.writer {
                    self.writer_up_ns = now;
                } else {
                    self.reader_up_ns = now;
                }
            }
            if !p.up() || p.queue.is_empty() {
                continue;
            }
            let heard = p.last_rx_ns.max(p.owed_since_ns);
            if now.saturating_sub(heard) >= WATCHDOG_NS {
                self.pg_fail(ci, "no reply for 10 s");
            } else if now.saturating_sub(heard.max(p.last_nudge_ns)) >= p.nudge_gap_ns {
                p.last_nudge_ns = now;
                p.nudge_gap_ns = (p.nudge_gap_ns * 2).min(NUDGE_MAX_NS);
                self.end_frame(ci);
                self.pg_flush(ci);
            }
        }
        if now >= self.next_reconnect_ns {
            // Ring: not before the old socket's receive and send have reported back; they use this slot's buffers.
            if let Some(ci) = self.pgs.iter().position(|p| !p.up() && now >= p.retry_at_ns && !p.rx && !p.tx) {
                // Blocking, once per connection: after an outage the first success brings the rest up quickly.
                let ok = self.reconnect(ci);
                self.next_reconnect_ns = now + if ok { 20_000_000 } else { 1_000_000_000 };
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(columns: &[&[u8]]) -> Vec<u8> {
        let mut r = (columns.len() as u16).to_be_bytes().to_vec();
        for c in columns {
            r.extend_from_slice(&(c.len() as u32).to_be_bytes());
            r.extend_from_slice(c);
        }
        r
    }

    #[test]
    fn a_post_as_json() {
        // 2025-12-31T23:59:19.409Z
        let created: i64 = 9_496 * 86_400_000_000 + (23 * 3600 + 59 * 60 + 19) * 1_000_000 + 409_000;
        // numbers of every length the short copy tells apart, strings around the 16-byte steps of the escaping
        for digits in 1..=20 {
            for text in [0, 1, 15, 16, 17, 40, 300] {
                let id = "1234567890".repeat(2)[..digits].to_string();
                let likes = "9876543210"[..digits.min(10)].to_string();
                let body = "a\"b\\c\n".repeat(50)[..text].to_string();
                let name = "nebula_".repeat(50)[..text].to_string();
                let mut out = b"[".to_vec();
                post_json(&mut out, &row(&[id.as_bytes(), body.as_bytes(), &created.to_be_bytes(), name.as_bytes(), likes.as_bytes()]), &mut Day::new()).unwrap();
                let escaped = body.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n");
                let want = format!(r#"[{{"id":{id},"body":"{escaped}","created_at":"2025-12-31T23:59:19.409Z","author":"{name}","like_count":{likes}}}"#);
                assert_eq!(String::from_utf8(out).unwrap(), want);
            }
        }
        assert!(post_json(&mut Vec::new(), &row(&[b"1", b"x"]), &mut Day::new()).is_none(), "a row of two columns");
        assert!(post_json(&mut Vec::new(), &row(&[b"1", b"x", b"short", b"n", b"0"]), &mut Day::new()).is_none(), "a timestamp of five bytes");
    }
}
