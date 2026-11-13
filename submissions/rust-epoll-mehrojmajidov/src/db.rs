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
use crate::reactor::{Kind, Loop, INTERNAL, POST_NOT_FOUND, TAG_PG};
use crate::sys;
use crate::time::format_pg_timestamp;
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
    /// The replies Postgres owes on this connection, in order.
    queue: VecDeque<Slot>,
    /// Requests among them (the connection's load).
    owed: usize,
    /// The response being built from the rows of the request at the front.
    body: Vec<u8>,
    rows: u32,
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
            inb: vec![0; 64 * 1024],
            in_len: 0,
            queue: VecDeque::new(),
            owed: 0,
            body: Vec::with_capacity(8 * 1024),
            rows: 0,
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

    /// Marks the start of a stretch in which this connection owes replies.
    fn begin_owing(&mut self, now_ns: u64) {
        if self.queue.is_empty() {
            self.owed_since_ns = now_ns;
            self.nudge_gap_ns = NUDGE_FIRST_NS;
        }
    }
}

fn post_json(out: &mut Vec<u8>, row: &[u8]) -> Option<()> {
    let [id, body, created_at, username, like_count] = pg::columns::<5>(row)?;
    let micros = i64::from_be_bytes(created_at.try_into().ok()?);
    out.extend_from_slice(b"{\"id\":");
    out.extend_from_slice(id);
    out.extend_from_slice(b",\"body\":\"");
    json::escape_into(out, body);
    out.extend_from_slice(b"\",\"created_at\":\"");
    out.extend_from_slice(&format_pg_timestamp(micros));
    out.extend_from_slice(b"\",\"author\":\"");
    json::escape_into(out, username);
    out.extend_from_slice(b"\",\"like_count\":");
    out.extend_from_slice(like_count);
    out.push(b'}');
    Some(())
}

impl Loop {
    pub fn connect_postgres(&mut self) {
        for i in 0..self.cfg.readers + self.cfg.writers {
            self.pgs.push(PgConn::new(i >= self.cfg.readers));
        }
        for ci in 0..self.pgs.len() {
            if !self.reconnect(ci) {
                // The database is not reachable: the timer keeps trying, one connection at a time.
                break;
            }
        }
    }

    fn reconnect(&mut self, ci: usize) -> bool {
        match pg::connect(&self.cfg.pg, &mut self.scram, CONNECT_TIMEOUT) {
            Ok(fd) => {
                let p = &mut self.pgs[ci];
                p.fd = fd;
                p.last_rx_ns = self.now_ns;
                self.watch(sys::EPOLL_CTL_ADD, fd, TAG_PG | ci as u64, sys::EPOLLIN | sys::EPOLLRDHUP);
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

    fn pg_readable(&mut self, ci: usize) {
        let mut inb = mem::take(&mut self.pgs[ci].inb);
        let mut len = self.pgs[ci].in_len;
        let fd = self.pgs[ci].fd;
        loop {
            if len == inb.len() {
                if inb.len() >= MAX_REPLY_BUFFER {
                    self.pgs[ci].inb = inb;
                    return self.pg_fail(ci, "oversized reply");
                }
                inb.resize(inb.len() * 2, 0);
            }
            let space = inb.len() - len;
            let n = unsafe { sys::recv(fd, inb[len..].as_mut_ptr().cast(), space, 0) };
            if n <= 0 {
                if n < 0 && matches!(sys::errno(), sys::EAGAIN | sys::EINTR) {
                    break;
                }
                self.pgs[ci].inb = inb;
                return self.pg_fail(ci, if n == 0 { "closed by Postgres" } else { "read error" });
            }
            len += n as usize;
            let p = &mut self.pgs[ci];
            p.last_rx_ns = self.now_ns;
            p.nudge_gap_ns = NUDGE_FIRST_NS;

            let mut at = 0;
            while len - at >= 5 {
                let size = u32::from_be_bytes([inb[at + 1], inb[at + 2], inb[at + 3], inb[at + 4]]) as usize;
                if size < 4 {
                    self.pgs[ci].inb = inb;
                    return self.pg_fail(ci, "bad message length");
                }
                if len - at < 1 + size {
                    break;
                }
                if self.pg_message(ci, inb[at], &inb[at + 5..at + 1 + size]).is_none() {
                    self.pgs[ci].inb = inb;
                    return self.pg_fail(ci, "unexpected reply");
                }
                at += 1 + size;
            }
            inb.copy_within(at..len, 0);
            len -= at;
            if (n as usize) < space {
                // the socket had less than we asked for: it is drained
                break;
            }
        }
        let p = &mut self.pgs[ci];
        p.inb = inb;
        p.in_len = len;
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
                        p.body.extend_from_slice(if p.rows == 0 { b"{\"posts\":[" } else { b"," });
                        post_json(&mut p.body, payload)?;
                    }
                    Kind::Post => {
                        p.body.extend_from_slice(b"{\"post\":");
                        post_json(&mut p.body, payload)?;
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
            // CommandComplete, or (never sent to us today) PortalSuspended and EmptyQueryResponse
            b'C' | b's' | b'I' => {
                let p = &mut self.pgs[ci];
                let Some(Slot::Op(op)) = p.queue.pop_front() else {
                    return None;
                };
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
        unsafe { sys::close(p.fd) };
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
        while !self.read_q.is_empty() {
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
                let Some(op) = self.read_q.pop_front() else { break };
                if self.ops[op as usize].fd < 0 {
                    self.finish(op, Status::Internal, INTERNAL);
                    continue;
                }
                self.encode(ci, op);
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
            if let Some(ci) = self.pgs.iter().position(|p| !p.up() && now >= p.retry_at_ns) {
                // Blocking, once per connection: after an outage the first success brings the rest up quickly.
                let ok = self.reconnect(ci);
                self.next_reconnect_ns = now + if ok { 20_000_000 } else { 1_000_000_000 };
            }
        }
    }
}
