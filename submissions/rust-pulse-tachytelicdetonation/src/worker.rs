use crate::fmt::{self, DayCache};
use crate::gzip::Gz;
use crate::http::{self, Method, Parsed, Req};
use crate::json::{self, Val};
use crate::jwt::{AuthErr, Jwt};
use crate::pg::{self, PgCfg};
use crate::sys::{self, mono_ns, unix_s};
use crate::uring::{self, Cqe, Ring};
use std::collections::VecDeque;
use std::sync::atomic::{AtomicUsize, Ordering};

pub struct Config {
    pub pg: PgCfg,
    pub jwt: Jwt,
    pub ip: [u8; 4],
    pub ports: Vec<u16>,
    pub reads: usize,
    pub writes: usize,
    pub batch_ns: u64,
    pub min_complete: u32,
    pub epoll: bool,
    pub epoll_batch_ns: u64,
    pub stats: bool,
    pub defer: i32,
    pub sched_idle: bool,
    pub read_batch: usize,
    pub write_batch: usize,
    pub gz_fixed: bool,
    pub bufs: u32,
    pub buf_size: usize,
    pub max_fixed: u32,
}

pub struct Shared {
    pub cfg: Config,
    pub n: usize,
    pub ready: AtomicUsize,
    pub start_s: i64,
}

const OPEN: u16 = 1;
const FIXED: u16 = 2;
const BUSY: u16 = 4;
const SENDING: u16 = 8;
const CLOSING: u16 = 16;
const CLOSE_AFTER: u16 = 32;
const RECV: u16 = 64;
const INSEND: u16 = 128;
const CANCELED: u16 = 256;
const REARM: u16 = 512;
const DRAIN: u16 = 1024;

const K_ACCEPT: u64 = 1;
const K_CONN: u64 = 2;
const K_SEND: u64 = 3;
const K_CANCEL: u64 = 4;
const K_CLOSE: u64 = 5;
const K_PGRECV: u64 = 6;
const K_PGSEND: u64 = 7;

const HEADROOM: usize = 192;
const MAX_INBUF: usize = 256 * 1024;

#[inline]
fn ud(k: u64, gen: u32, idx: u32) -> u64 {
    (k << 56) | (((gen & 0xFF_FFFF) as u64) << 32) | idx as u64
}

#[derive(Default)]
struct CBufs {
    inb: Vec<u8>,
    out: Vec<u8>,
    off: usize,
    next: Vec<u8>,
}

struct Conn {
    fd: i32,
    gen: u32,
    fl: u16,
    b: Option<Box<CBufs>>,
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
enum Kind {
    Feed,
    Post,
    Health,
    Create,
    Like,
}

struct Item {
    conn: u32,
    gen: u32,
    kind: Kind,
    gz: bool,
    tries: u8,
    tag: u8,
    a: i64,
    b: i64,
    r_id: i64,
    r_ts: i64,
    ulen: u32,
    data: Vec<u8>,
}

impl Item {
    fn new(conn: u32, gen: u32, kind: Kind, gz: bool, a: i64, b: i64) -> Item {
        Item { conn, gen, kind, gz, tries: 0, tag: 0, a, b, r_id: 0, r_ts: 0, ulen: 0, data: Vec::new() }
    }
}

struct Seg {
    items: Vec<Item>,
    cur: usize,
    err: Option<(usize, [u8; 5])>,
    write: bool,
}

#[derive(PartialEq, Clone, Copy)]
enum PgSt {
    Up,
    Draining,
    Down,
}

struct Pg {
    fd: i32,
    write: bool,
    st: PgSt,
    epoch: u32,
    rx: Vec<u8>,
    rxoff: usize,
    tx: Vec<u8>,
    txing: Vec<u8>,
    txoff: usize,
    sending: bool,
    recv: bool,
    want_out: bool,
    segs: VecDeque<Seg>,
    load: usize,
    last_rx: u64,
    retry_at: u64,
    scratch: Vec<u8>,
    rows: u32,
    started: bool,
}

#[derive(Default)]
struct Stats {
    passes: u64,
    events: u64,
    reqs: u64,
    reads: u64,
    read_syncs: u64,
    writes: u64,
    write_syncs: u64,
    nobufs: u64,
    gz_calls: u64,
    gz_in: u64,
    gz_out: u64,
    gz_ns: u64,
    pg_errors: u64,
}

pub struct Worker {
    id: usize,
    sh: &'static Shared,
    conns: Vec<Conn>,
    free: Vec<u32>,
    live: usize,
    bufpool: Vec<Box<CBufs>>,
    vpool: Vec<Vec<u8>>,
    segpool: Vec<Vec<Item>>,
    pgs: Vec<Pg>,
    pg_new: Vec<usize>,
    readq: Vec<Item>,
    writeq: Vec<Item>,
    soloq: Vec<Item>,
    want_send: Vec<u32>,
    want_close: Vec<u32>,
    rearm: Vec<u32>,
    drain: Vec<u32>,
    now_ns: u64,
    now_s: i64,
    last_tick: u64,
    gz: Gz,
    name: Vec<u8>,
    tmp: Vec<u8>,
    dc: DayCache,
    ready: bool,
    listeners: Vec<i32>,
    cpu: i32,
    stats: Stats,
    last_stats: u64,
    cpu_ns_at: u64,
    mode: &'static str,
}

fn status_line(s: u16) -> &'static [u8] {
    match s {
        200 => b"HTTP/1.1 200 OK\r\n",
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    }
}

fn head(out: &mut Vec<u8>, status: u16, len: usize, gz: bool, close: bool) {
    out.extend_from_slice(status_line(status));
    out.extend_from_slice(b"Content-Type: application/json\r\n");
    if gz {
        out.extend_from_slice(b"Content-Encoding: gzip\r\n");
    }
    if close {
        out.extend_from_slice(b"Connection: close\r\n");
    }
    out.extend_from_slice(b"Content-Length: ");
    fmt::itoa(out, len as i64);
    out.extend_from_slice(b"\r\n\r\n");
}

fn parse_id(s: &[u8]) -> Option<i64> {
    if s.is_empty() || s.len() > 19 {
        return None;
    }
    let mut v: i64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as i64)?;
    }
    if v == 0 {
        None
    } else {
        Some(v)
    }
}

#[inline]
fn be32(b: &[u8], i: usize) -> Option<i32> {
    b.get(i..i + 4).map(|s| i32::from_be_bytes([s[0], s[1], s[2], s[3]]))
}

fn col<'a>(b: &'a [u8], i: &mut usize) -> Option<&'a [u8]> {
    let l = be32(b, *i)?;
    *i += 4;
    if l < 0 {
        return Some(&[]);
    }
    let s = b.get(*i..*i + l as usize)?;
    *i += l as usize;
    Some(s)
}

fn row_json(out: &mut Vec<u8>, body: &[u8], dc: &mut DayCache) -> Option<()> {
    let mut i = 2;
    let id = col(body, &mut i)?;
    let text = col(body, &mut i)?;
    let ts = col(body, &mut i)?;
    let user = col(body, &mut i)?;
    let lc = col(body, &mut i)?;
    if ts.len() != 8 {
        return None;
    }
    out.extend_from_slice(b"{\"id\":");
    out.extend_from_slice(id);
    out.extend_from_slice(b",\"body\":\"");
    fmt::escape(out, text);
    out.extend_from_slice(b"\",\"created_at\":\"");
    fmt::timestamp(out, i64::from_be_bytes(ts.try_into().unwrap()), dc);
    out.extend_from_slice(b"\",\"author\":\"");
    fmt::escape(out, user);
    out.extend_from_slice(b"\",\"like_count\":");
    out.extend_from_slice(lc);
    out.push(b'}');
    Some(())
}

struct U {
    ring: Ring,
    bufs: uring::Bufs,
    lfixed: Vec<bool>,
    accept_rearm: Vec<usize>,
    fixed_slots: u32,
}

struct E {
    ep: i32,
    rbuf: Vec<u8>,
}

impl Worker {
    fn new(id: usize, sh: &'static Shared, listeners: Vec<i32>, cpu: i32) -> Worker {
        let cfg = &sh.cfg;
        let mut pgs = Vec::new();
        for k in 0..cfg.reads + cfg.writes {
            pgs.push(Pg {
                fd: -1,
                write: k >= cfg.reads,
                st: PgSt::Down,
                epoch: 0,
                rx: Vec::with_capacity(128 * 1024),
                rxoff: 0,
                tx: Vec::with_capacity(64 * 1024),
                txing: Vec::with_capacity(64 * 1024),
                txoff: 0,
                sending: false,
                recv: false,
                want_out: false,
                segs: VecDeque::new(),
                load: 0,
                last_rx: 0,
                retry_at: 0,
                scratch: Vec::with_capacity(16 * 1024),
                rows: 0,
                started: false,
            });
        }
        let now = mono_ns();
        Worker {
            id,
            sh,
            conns: Vec::new(),
            free: Vec::new(),
            live: 0,
            bufpool: Vec::new(),
            vpool: Vec::new(),
            segpool: Vec::new(),
            pgs,
            pg_new: Vec::new(),
            readq: Vec::new(),
            writeq: Vec::new(),
            soloq: Vec::new(),
            want_send: Vec::new(),
            want_close: Vec::new(),
            rearm: Vec::new(),
            drain: Vec::new(),
            now_ns: now,
            now_s: unix_s(),
            last_tick: now,
            gz: Gz::new(cfg.gz_fixed),
            name: Vec::new(),
            tmp: Vec::new(),
            dc: DayCache::new(),
            ready: false,
            listeners,
            cpu,
            stats: Stats::default(),
            last_stats: now,
            cpu_ns_at: 0,
            mode: "",
        }
    }

    fn vget(&mut self) -> Vec<u8> {
        self.vpool.pop().unwrap_or_else(|| Vec::with_capacity(4096))
    }

    fn vput(&mut self, mut v: Vec<u8>) {
        if v.capacity() <= 64 * 1024 && self.vpool.len() < 4096 {
            v.clear();
            self.vpool.push(v);
        }
    }

    fn bufs(&mut self, idx: u32) -> &mut CBufs {
        let c = &mut self.conns[idx as usize];
        if c.b.is_none() {
            c.b = Some(self.bufpool.pop().unwrap_or_default());
        }
        c.b.as_mut().unwrap()
    }

    fn maybe_release(&mut self, idx: u32) {
        let c = &mut self.conns[idx as usize];
        if c.fl & (BUSY | SENDING) != 0 {
            return;
        }
        let idle = match &c.b {
            Some(b) => b.inb.is_empty() && b.off >= b.out.len() && b.next.is_empty(),
            None => false,
        };
        if idle {
            let mut b = c.b.take().unwrap();
            b.out.clear();
            b.off = 0;
            if self.bufpool.len() < 4096 && b.out.capacity() <= 64 * 1024 && b.inb.capacity() <= 64 * 1024 {
                self.bufpool.push(b);
            }
        }
    }

    fn new_conn(&mut self, fd: i32, fixed: bool) -> u32 {
        let idx = match self.free.pop() {
            Some(i) => i,
            None => {
                self.conns.push(Conn { fd: -1, gen: 0, fl: 0, b: None });
                (self.conns.len() - 1) as u32
            }
        };
        let c = &mut self.conns[idx as usize];
        c.gen = c.gen.wrapping_add(1) & 0xFF_FFFF;
        c.fd = fd;
        c.fl = OPEN | if fixed { FIXED } else { 0 };
        c.b = None;
        self.live += 1;
        idx
    }

    fn free_conn(&mut self, idx: u32) {
        let c = &mut self.conns[idx as usize];
        c.fl = 0;
        c.fd = -1;
        if let Some(mut b) = c.b.take() {
            if self.bufpool.len() < 4096 && b.out.capacity() <= 64 * 1024 && b.inb.capacity() <= 64 * 1024 {
                b.inb.clear();
                b.out.clear();
                b.next.clear();
                b.off = 0;
                self.bufpool.push(b);
            }
        }
        self.free.push(idx);
        self.live -= 1;
    }

    fn close_conn(&mut self, idx: u32) {
        let c = &mut self.conns[idx as usize];
        if c.fl & OPEN != 0 && c.fl & CLOSING == 0 {
            c.fl |= CLOSING;
            self.want_close.push(idx);
        }
    }

    fn queue_send(&mut self, idx: u32) {
        let c = &mut self.conns[idx as usize];
        if c.fl & INSEND == 0 {
            c.fl |= INSEND;
            self.want_send.push(idx);
        }
    }

    fn deliver(&mut self, idx: u32, gen: u32, v: Vec<u8>, start: usize) {
        let c = &mut self.conns[idx as usize];
        if c.gen != gen || c.fl & OPEN == 0 || c.fl & CLOSING != 0 {
            self.vput(v);
            return;
        }
        c.fl &= !BUSY;
        let sending = c.fl & SENDING != 0;
        let b = self.bufs(idx);
        let spare = if sending {
            b.next.extend_from_slice(&v[start..]);
            v
        } else if b.off >= b.out.len() {
            let old = std::mem::replace(&mut b.out, v);
            b.off = start;
            old
        } else {
            b.out.extend_from_slice(&v[start..]);
            v
        };
        let has_in = !b.inb.is_empty();
        self.vput(spare);
        self.queue_send(idx);
        let c = &mut self.conns[idx as usize];
        if has_in && c.fl & DRAIN == 0 {
            c.fl |= DRAIN;
            self.drain.push(idx);
        }
    }

    fn reply(&mut self, idx: u32, status: u16, body: &[u8]) {
        let gen = self.conns[idx as usize].gen;
        let close = self.conns[idx as usize].fl & CLOSE_AFTER != 0;
        let mut v = self.vget();
        head(&mut v, status, body.len(), false, close);
        v.extend_from_slice(body);
        self.deliver(idx, gen, v, 0);
    }

    fn reply_to(&mut self, idx: u32, gen: u32, status: u16, body: &[u8]) {
        let c = &self.conns[idx as usize];
        if c.gen != gen || c.fl & OPEN == 0 {
            return;
        }
        self.reply(idx, status, body);
    }

    fn finish_json(&mut self, idx: u32, gen: u32, status: u16, mut v: Vec<u8>, gz: bool) {
        let c = &self.conns[idx as usize];
        if c.gen != gen || c.fl & OPEN == 0 || c.fl & CLOSING != 0 {
            self.vput(v);
            return;
        }
        let close = c.fl & CLOSE_AFTER != 0;
        let mut gzipped = false;
        if gz && v.len() - HEADROOM >= 1024 {
            let t0 = if self.sh.cfg.stats { mono_ns() } else { 0 };
            let mut g = self.vget();
            g.clear();
            g.reserve(HEADROOM + 2048);
            g.resize(HEADROOM, 0);
            self.gz.compress(&v[HEADROOM..], &mut g);
            if self.sh.cfg.stats {
                self.stats.gz_calls += 1;
                self.stats.gz_in += (v.len() - HEADROOM) as u64;
                self.stats.gz_out += (g.len() - HEADROOM) as u64;
                self.stats.gz_ns += mono_ns() - t0;
            }
            self.vput(v);
            v = g;
            gzipped = true;
        }
        let mut h = std::mem::take(&mut self.tmp);
        h.clear();
        head(&mut h, status, v.len() - HEADROOM, gzipped, close);
        let start = HEADROOM - h.len();
        v[start..HEADROOM].copy_from_slice(&h);
        self.tmp = h;
        self.deliver(idx, gen, v, start);
    }

    fn client_data(&mut self, idx: u32, data: &[u8]) {
        let fl = self.conns[idx as usize].fl;
        if fl & OPEN == 0 || fl & CLOSING != 0 {
            return;
        }
        let has_inb = self.conns[idx as usize].b.as_ref().map_or(false, |b| !b.inb.is_empty());
        if fl & BUSY != 0 || has_inb {
            let b = self.bufs(idx);
            b.inb.extend_from_slice(data);
            if b.inb.len() > MAX_INBUF {
                self.close_conn(idx);
                return;
            }
            if fl & BUSY == 0 {
                self.drain_inb(idx);
            }
            return;
        }
        let used = self.parse_loop(idx, data);
        let fl = self.conns[idx as usize].fl;
        if used < data.len() && fl & OPEN != 0 && fl & CLOSING == 0 {
            self.bufs(idx).inb.extend_from_slice(&data[used..]);
        }
    }

    fn drain_inb(&mut self, idx: u32) {
        let c = &mut self.conns[idx as usize];
        c.fl &= !DRAIN;
        if c.fl & OPEN == 0 || c.fl & (CLOSING | BUSY) != 0 {
            return;
        }
        let Some(b) = c.b.as_mut() else { return };
        let mut inb = std::mem::take(&mut b.inb);
        let used = self.parse_loop(idx, &inb);
        inb.drain(..used);
        if self.conns[idx as usize].fl & OPEN != 0 {
            self.bufs(idx).inb = inb;
        }
    }

    fn parse_loop(&mut self, idx: u32, data: &[u8]) -> usize {
        let mut off = 0;
        while off < data.len() {
            match http::parse(&data[off..]) {
                Parsed::Done(req, n) => {
                    off += n;
                    self.stats.reqs += 1;
                    self.handle(idx, &req);
                    if self.conns[idx as usize].fl & (BUSY | CLOSING | CLOSE_AFTER) != 0 {
                        break;
                    }
                }
                Parsed::Partial => break,
                Parsed::Bad => {
                    self.conns[idx as usize].fl |= CLOSE_AFTER;
                    self.reply(idx, 400, b"{\"error\":\"bad request\"}");
                    off = data.len();
                    break;
                }
            }
        }
        off
    }

    fn enqueue(&mut self, idx: u32, it: Item, write: bool) {
        self.conns[idx as usize].fl |= BUSY;
        if write {
            self.writeq.push(it);
        } else {
            self.readq.push(it);
        }
    }

    fn auth(&mut self, idx: u32, req: &Req) -> Option<i64> {
        let mut name = std::mem::take(&mut self.name);
        let r = self.sh.cfg.jwt.verify(req.auth, self.now_s, &mut name);
        self.name = name;
        match r {
            Ok(sub) => Some(sub),
            Err(AuthErr::Missing) => {
                self.reply(idx, 401, b"{\"error\":\"missing bearer token\"}");
                None
            }
            Err(AuthErr::Invalid) => {
                self.reply(idx, 401, b"{\"error\":\"invalid or expired token\"}");
                None
            }
            Err(AuthErr::Payload) => {
                self.reply(idx, 401, b"{\"error\":\"invalid token payload\"}");
                None
            }
        }
    }

    fn handle(&mut self, idx: u32, req: &Req) {
        if req.close {
            self.conns[idx as usize].fl |= CLOSE_AFTER;
        }
        let gen = self.conns[idx as usize].gen;
        let path = match req.path.iter().position(|&c| c == b'?') {
            Some(q) => &req.path[..q],
            None => req.path,
        };
        match req.method {
            Method::Get => {
                if path == b"/feed" {
                    return self.enqueue(idx, Item::new(idx, gen, Kind::Feed, req.gzip, 0, 0), false);
                }
                if let Some(rest) = path.strip_prefix(b"/posts/") {
                    if !rest.contains(&b'/') {
                        return match parse_id(rest) {
                            Some(id) => self.enqueue(idx, Item::new(idx, gen, Kind::Post, req.gzip, id, 0), false),
                            None => self.reply(idx, 400, b"{\"error\":\"invalid post id\"}"),
                        };
                    }
                }
                if path == b"/health" {
                    if self.sh.ready.load(Ordering::Relaxed) < self.sh.n {
                        return self.reply(
                            idx,
                            503,
                            b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database connections not ready\"}",
                        );
                    }
                    return self.enqueue(idx, Item::new(idx, gen, Kind::Health, false, 0, 0), false);
                }
            }
            Method::Post => {
                if path == b"/posts" {
                    let Some(user) = self.auth(idx, req) else { return };
                    return self.create(idx, gen, user, req);
                }
                if let Some(rest) = path.strip_prefix(b"/posts/") {
                    if let Some(ids) = rest.strip_suffix(b"/like") {
                        if !ids.contains(&b'/') {
                            let Some(user) = self.auth(idx, req) else { return };
                            return match parse_id(ids) {
                                Some(id) => self.enqueue(idx, Item::new(idx, gen, Kind::Like, false, user, id), true),
                                None => self.reply(idx, 400, b"{\"error\":\"invalid post id\"}"),
                            };
                        }
                    }
                }
            }
            Method::Other => {}
        }
        self.reply(idx, 404, b"{\"error\":\"not found\"}")
    }

    fn create(&mut self, idx: u32, gen: u32, user: i64, req: &Req) {
        if std::str::from_utf8(req.body).is_err() {
            return self.reply(idx, 400, b"{\"error\":\"malformed JSON body\"}");
        }
        let mut found: Option<(&[u8], bool)> = None;
        let mut is_str = false;
        let r = json::fields(req.body, &mut |k, v| {
            if k == b"body" {
                match v {
                    Val::Str { raw, esc } => {
                        found = Some((raw, esc));
                        is_str = true;
                    }
                    _ => {
                        found = None;
                        is_str = false;
                    }
                }
            }
        });
        if r.is_err() {
            return self.reply(idx, 400, b"{\"error\":\"malformed JSON body\"}");
        }
        let Some((raw, esc)) = found.filter(|_| is_str) else {
            return self.reply(idx, 400, b"{\"error\":\"body is required\"}");
        };
        let mut dec = Vec::new();
        let s: &[u8] = if esc {
            if json::decode(raw, &mut dec).is_err() {
                return self.reply(idx, 400, b"{\"error\":\"malformed JSON body\"}");
            }
            &dec
        } else {
            raw
        };
        let Ok(st) = std::str::from_utf8(s) else {
            return self.reply(idx, 400, b"{\"error\":\"malformed JSON body\"}");
        };
        let t = json::js_trim(st);
        if t.is_empty() {
            return self.reply(idx, 400, b"{\"error\":\"body is required\"}");
        }
        if t.len() > 500 && t.chars().count() > 500 {
            return self.reply(idx, 400, b"{\"error\":\"body must be at most 500 characters\"}");
        }
        let mut it = Item::new(idx, gen, Kind::Create, false, user, 0);
        it.data.reserve(self.name.len() + t.len());
        it.data.extend_from_slice(&self.name);
        it.data.extend_from_slice(t.as_bytes());
        it.ulen = self.name.len() as u32;
        self.enqueue(idx, it, true);
    }

    fn requeue(&mut self, mut it: Item) {
        it.tries += 1;
        if it.tries > 3 {
            self.fail_read(it, b"query retries exhausted");
        } else {
            self.readq.push(it);
        }
    }

    fn fail_read(&mut self, it: Item, why: &[u8]) {
        if it.kind == Kind::Health {
            let mut b = Vec::new();
            b.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
            fmt::escape(&mut b, why);
            b.extend_from_slice(b"\"}");
            self.reply_to(it.conn, it.gen, 503, &b);
        } else {
            self.reply_to(it.conn, it.gen, 500, b"{\"error\":\"internal server error\"}");
        }
    }

    fn fail_write(&mut self, it: Item, code: &[u8; 5]) {
        if it.kind == Kind::Like && code == b"23503" {
            self.reply_to(it.conn, it.gen, 404, b"{\"error\":\"post not found\"}");
        } else {
            self.reply_to(it.conn, it.gen, 500, b"{\"error\":\"internal server error\"}");
        }
    }

    fn respond_write(&mut self, it: Item) {
        match it.kind {
            Kind::Create if it.tag == 1 => {
                let mut v = self.vget();
                v.resize(HEADROOM, 0);
                v.extend_from_slice(b"{\"post\":{\"id\":");
                fmt::itoa(&mut v, it.r_id);
                v.extend_from_slice(b",\"body\":\"");
                fmt::escape(&mut v, &it.data[it.ulen as usize..]);
                v.extend_from_slice(b"\",\"created_at\":\"");
                fmt::timestamp(&mut v, it.r_ts, &mut self.dc);
                v.extend_from_slice(b"\",\"author\":\"");
                fmt::escape(&mut v, &it.data[..it.ulen as usize]);
                v.extend_from_slice(b"\",\"like_count\":0}}");
                self.finish_json(it.conn, it.gen, 201, v, it.gz);
            }
            Kind::Like if it.tag != 0 => {
                let mut v = self.vget();
                v.resize(HEADROOM, 0);
                v.extend_from_slice(if it.tag == 1 {
                    b"{\"liked\":true,\"already_liked\":false,\"post_id\":"
                } else {
                    b"{\"liked\":true,\"already_liked\":true,\"post_id\":"
                });
                fmt::itoa(&mut v, it.b);
                v.push(b'}');
                self.finish_json(it.conn, it.gen, if it.tag == 1 { 201 } else { 200 }, v, false);
            }
            _ => self.reply_to(it.conn, it.gen, 500, b"{\"error\":\"internal server error\"}"),
        }
    }

    fn pg_input(&mut self, p: usize) {
        let mut rx = std::mem::take(&mut self.pgs[p].rx);
        let mut off = self.pgs[p].rxoff;
        loop {
            let avail = rx.len() - off;
            if avail < 5 {
                break;
            }
            let len = be32(&rx, off + 1).unwrap() as usize;
            if len < 4 {
                self.pg_dead(p, "protocol error");
                break;
            }
            if avail < 1 + len {
                break;
            }
            let t = rx[off];
            self.pg_msg(p, t, &rx[off + 5..off + 1 + len]);
            off += 1 + len;
            if self.pgs[p].st != PgSt::Up {
                break;
            }
        }
        if off >= rx.len() || self.pgs[p].st != PgSt::Up {
            rx.clear();
            off = 0;
        } else if off > 0 && rx.capacity() - rx.len() < 64 * 1024 {
            rx.drain(..off);
            off = 0;
        }
        let pg = &mut self.pgs[p];
        pg.rx = rx;
        pg.rxoff = off;
    }

    fn pg_msg(&mut self, p: usize, t: u8, body: &[u8]) {
        match t {
            b'D' => self.pg_row(p, body),
            b'C' => self.pg_cmd(p, body),
            b'E' => self.pg_err(p, body),
            b'Z' => self.pg_ready(p),
            _ => {}
        }
    }

    fn pg_row(&mut self, p: usize, body: &[u8]) {
        let pg = &mut self.pgs[p];
        let Some(seg) = pg.segs.front_mut() else { return };
        let Some(it) = seg.items.get_mut(seg.cur) else { return };
        match it.kind {
            Kind::Feed | Kind::Post => {
                if !pg.started {
                    pg.scratch.clear();
                    pg.scratch.resize(HEADROOM, 0);
                    pg.scratch.extend_from_slice(if it.kind == Kind::Feed { b"{\"posts\":[" } else { b"{\"post\":" });
                    pg.started = true;
                    pg.rows = 0;
                }
                if pg.rows > 0 {
                    pg.scratch.push(b',');
                }
                pg.rows += 1;
                let _ = row_json(&mut pg.scratch, body, &mut self.dc);
            }
            Kind::Create => {
                let mut i = 2;
                if let (Some(id), Some(ts)) = (col(body, &mut i), col(body, &mut i)) {
                    if id.len() == 8 && ts.len() == 8 {
                        it.r_id = i64::from_be_bytes(id.try_into().unwrap());
                        it.r_ts = i64::from_be_bytes(ts.try_into().unwrap());
                    }
                }
            }
            _ => {}
        }
    }

    fn pg_cmd(&mut self, p: usize, tag: &[u8]) {
        let pg = &mut self.pgs[p];
        let Some(seg) = pg.segs.front_mut() else { return };
        let cur = seg.cur;
        seg.cur += 1;
        let Some(it) = seg.items.get_mut(cur) else { return };
        let (conn, gen, gz, kind) = (it.conn, it.gen, it.gz, it.kind);
        match kind {
            Kind::Create => it.tag = 1,
            Kind::Like => {
                let tag = tag.strip_suffix(b"\0").unwrap_or(tag);
                it.tag = if tag.ends_with(b" 1") { 1 } else { 2 };
            }
            Kind::Health => {
                let mut b = Vec::new();
                b.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                fmt::itoa(&mut b, self.now_s - self.sh.start_s);
                b.push(b'}');
                self.reply_to(conn, gen, 200, &b);
            }
            Kind::Feed | Kind::Post => {
                let started = pg.started;
                pg.started = false;
                if kind == Kind::Post && !started {
                    self.reply_to(conn, gen, 404, b"{\"error\":\"post not found\"}");
                    return;
                }
                let fresh = self.vpool.pop().unwrap_or_else(|| Vec::with_capacity(16 * 1024));
                let pg = &mut self.pgs[p];
                let mut v = std::mem::replace(&mut pg.scratch, fresh);
                if !started {
                    v.clear();
                    v.resize(HEADROOM, 0);
                    v.extend_from_slice(b"{\"posts\":[");
                }
                v.extend_from_slice(if kind == Kind::Feed { b"]}" } else { b"}" });
                self.finish_json(conn, gen, 200, v, gz);
            }
        }
    }

    fn pg_err(&mut self, p: usize, body: &[u8]) {
        let code = pg::sqlstate(body);
        self.stats.pg_errors += 1;
        if self.stats.pg_errors <= 20 && &code != b"23503" {
            eprintln!("[w{}] postgres error {}: {}", self.id, String::from_utf8_lossy(&code), pg::error_text(body));
        }
        let pg = &mut self.pgs[p];
        pg.started = false;
        if let Some(seg) = pg.segs.front_mut() {
            if seg.err.is_none() {
                seg.err = Some((seg.cur, code));
            }
        }
    }

    fn pg_ready(&mut self, p: usize) {
        let pg = &mut self.pgs[p];
        pg.started = false;
        let Some(mut seg) = pg.segs.pop_front() else { return };
        pg.load -= seg.items.len();
        let mut items = std::mem::take(&mut seg.items);
        if seg.write {
            let n = items.len();
            match seg.err {
                None => {
                    for it in items.drain(..) {
                        self.respond_write(it);
                    }
                }
                Some((ei, code)) => {
                    for (k, it) in items.drain(..).enumerate() {
                        if n == 1 || k == ei {
                            self.fail_write(it, &code);
                        } else {
                            self.soloq.push(it);
                        }
                    }
                }
            }
        } else {
            let ei = seg.err.map(|e| e.0);
            for (k, it) in items.drain(..).enumerate() {
                if k < seg.cur && Some(k) != ei {
                    continue;
                }
                if Some(k) == ei {
                    self.fail_read(it, b"query failed");
                } else {
                    self.requeue(it);
                }
            }
        }
        if self.segpool.len() < 256 {
            self.segpool.push(items);
        }
    }

    fn pg_dead(&mut self, p: usize, why: &str) {
        let pg = &mut self.pgs[p];
        if pg.st != PgSt::Up {
            return;
        }
        eprintln!("[w{}] postgres connection {} lost: {}", self.id, p, why);
        pg.st = PgSt::Draining;
        unsafe { libc::shutdown(pg.fd, libc::SHUT_RDWR) };
        let segs = std::mem::take(&mut pg.segs);
        pg.load = 0;
        pg.started = false;
        pg.tx.clear();
        pg.want_out = false;
        for mut seg in segs {
            for (k, it) in seg.items.drain(..).enumerate() {
                if seg.write {
                    self.reply_to(it.conn, it.gen, 500, b"{\"error\":\"internal server error\"}");
                } else if k >= seg.cur || seg.err.map(|e| e.0) == Some(k) {
                    self.requeue(it);
                }
            }
        }
        self.update_ready();
    }

    fn update_ready(&mut self) {
        let r = self.pgs.iter().any(|p| !p.write && p.st == PgSt::Up) && self.pgs.iter().any(|p| p.write && p.st == PgSt::Up);
        if r != self.ready {
            if r {
                self.sh.ready.fetch_add(1, Ordering::SeqCst);
            } else {
                self.sh.ready.fetch_sub(1, Ordering::SeqCst);
            }
            self.ready = r;
        }
    }

    fn connect_pg(&mut self, p: usize) {
        match pg::connect(&self.sh.cfg.pg) {
            Ok(fd) => {
                let pg = &mut self.pgs[p];
                pg.fd = fd;
                pg.st = PgSt::Up;
                pg.epoch = pg.epoch.wrapping_add(1) & 0xFF_FFFF;
                pg.rx.clear();
                pg.rxoff = 0;
                pg.tx.clear();
                pg.txing.clear();
                pg.txoff = 0;
                pg.sending = false;
                pg.recv = false;
                pg.want_out = false;
                pg.last_rx = self.now_ns;
                self.pg_new.push(p);
            }
            Err(e) => {
                eprintln!("[w{}] postgres connect failed: {}", self.id, e);
                self.pgs[p].retry_at = self.now_ns + 1_000_000_000;
            }
        }
    }

    fn dispatch(&mut self) {
        while !self.readq.is_empty() {
            let best = (0..self.pgs.len())
                .filter(|&p| !self.pgs[p].write && self.pgs[p].st == PgSt::Up)
                .min_by_key(|&p| self.pgs[p].load);
            let Some(p) = best else {
                for it in std::mem::take(&mut self.readq) {
                    self.fail_read(it, b"no database connection");
                }
                break;
            };
            let n = self.readq.len().min(self.sh.cfg.read_batch);
            let mut items = self.segpool.pop().unwrap_or_default();
            if n == self.readq.len() {
                std::mem::swap(&mut items, &mut self.readq);
            } else {
                items.extend(self.readq.drain(..n));
            }
            let pg = &mut self.pgs[p];
            for it in &items {
                match it.kind {
                    Kind::Feed => pg::put_feed(&mut pg.tx),
                    Kind::Post => pg::put_post(&mut pg.tx, it.a),
                    _ => pg::put_health(&mut pg.tx),
                }
            }
            pg::put_sync(&mut pg.tx);
            if pg.segs.is_empty() {
                pg.last_rx = self.now_ns;
            }
            pg.load += n;
            pg.segs.push_back(Seg { items, cur: 0, err: None, write: false });
            self.stats.reads += n as u64;
            self.stats.read_syncs += 1;
        }
        if self.writeq.is_empty() && self.soloq.is_empty() {
            return;
        }
        let any_w = self.pgs.iter().any(|p| p.write && p.st == PgSt::Up);
        if !any_w {
            for it in std::mem::take(&mut self.writeq).into_iter().chain(std::mem::take(&mut self.soloq)) {
                self.reply_to(it.conn, it.gen, 500, b"{\"error\":\"internal server error\"}");
            }
            return;
        }
        if !self.soloq.is_empty() {
            let p = (0..self.pgs.len())
                .filter(|&p| self.pgs[p].write && self.pgs[p].st == PgSt::Up)
                .min_by_key(|&p| self.pgs[p].load)
                .unwrap();
            for it in std::mem::take(&mut self.soloq) {
                let pg = &mut self.pgs[p];
                match it.kind {
                    Kind::Create => pg::put_create(&mut pg.tx, it.a, &it.data[it.ulen as usize..]),
                    _ => pg::put_like(&mut pg.tx, it.a, it.b),
                }
                pg::put_sync(&mut pg.tx);
                pg.load += 1;
                let mut items = Vec::with_capacity(1);
                items.push(it);
                pg.segs.push_back(Seg { items, cur: 0, err: None, write: true });
                self.stats.writes += 1;
                self.stats.write_syncs += 1;
            }
        }
        while !self.writeq.is_empty() {
            let idle = (0..self.pgs.len()).find(|&p| self.pgs[p].write && self.pgs[p].st == PgSt::Up && self.pgs[p].segs.is_empty());
            let Some(p) = idle else { break };
            self.writeq.sort_unstable_by_key(|it| (it.kind, it.a, it.b));
            let n = self.writeq.len().min(self.sh.cfg.write_batch);
            let mut items = self.segpool.pop().unwrap_or_default();
            if n == self.writeq.len() {
                std::mem::swap(&mut items, &mut self.writeq);
            } else {
                items.extend(self.writeq.drain(..n));
            }
            let pg = &mut self.pgs[p];
            for it in &items {
                match it.kind {
                    Kind::Create => pg::put_create(&mut pg.tx, it.a, &it.data[it.ulen as usize..]),
                    _ => pg::put_like(&mut pg.tx, it.a, it.b),
                }
            }
            pg::put_sync(&mut pg.tx);
            pg.load += n;
            pg.segs.push_back(Seg { items, cur: 0, err: None, write: true });
            self.stats.writes += n as u64;
            self.stats.write_syncs += 1;
        }
    }

    fn tick(&mut self) {
        if self.now_ns - self.last_tick < 100_000_000 {
            return;
        }
        self.last_tick = self.now_ns;
        for p in 0..self.pgs.len() {
            let pg = &self.pgs[p];
            if pg.st == PgSt::Up && !pg.write && pg.load > 0 && self.now_ns - pg.last_rx > 10_000_000_000 {
                self.pg_dead(p, "no reply for 10 s");
            }
        }
        for p in 0..self.pgs.len() {
            if self.pgs[p].st == PgSt::Down && self.now_ns >= self.pgs[p].retry_at {
                self.connect_pg(p);
            }
        }
        self.update_ready();
        {
            let c = sys::current_cpu();
            if c >= 0 && c != self.cpu {
                self.cpu = c;
                for &l in &self.listeners {
                    sys::setopt(l, libc::SOL_SOCKET, libc::SO_INCOMING_CPU, c);
                }
            }
        }
        if self.sh.cfg.stats && self.now_ns - self.last_stats >= 10_000_000_000 {
            self.print_stats();
        }
    }

    fn print_stats(&mut self) {
        let s = &self.stats;
        let secs = (self.now_ns - self.last_stats) as f64 / 1e9;
        let cpu = sys::thread_cpu_ns();
        let cpu_us_req = if s.reqs > 0 { (cpu - self.cpu_ns_at) as f64 / 1e3 / s.reqs as f64 } else { 0.0 };
        eprintln!(
            "[w{} {}] req/s={:.0} passes/s={:.0} ev/pass={:.1} reads/sync={:.1} writes/sync={:.1} nobufs={} gz: n={} avg_in={} avg_out={} ns/call={} conns={} user+sys_thread_us/req={:.2} pgerr={}",
            self.id,
            self.mode,
            s.reqs as f64 / secs,
            s.passes as f64 / secs,
            s.events as f64 / s.passes.max(1) as f64,
            s.reads as f64 / s.read_syncs.max(1) as f64,
            s.writes as f64 / s.write_syncs.max(1) as f64,
            s.nobufs,
            s.gz_calls,
            s.gz_in / s.gz_calls.max(1),
            s.gz_out / s.gz_calls.max(1),
            s.gz_ns / s.gz_calls.max(1),
            self.live,
            cpu_us_req,
            s.pg_errors
        );
        self.stats = Stats::default();
        self.last_stats = self.now_ns;
        self.cpu_ns_at = cpu;
    }

    fn end_pass(&mut self) {
        while let Some(i) = self.drain.pop() {
            self.drain_inb(i);
        }
        self.tick();
        self.dispatch();
        self.stats.passes += 1;
    }

    fn u_arm_accept(&mut self, u: &mut U, li: usize) {
        let s = u.ring.sqe();
        s.opcode = uring::OP_ACCEPT;
        s.fd = self.listeners[li];
        s.ioprio = uring::ACCEPT_MULTISHOT;
        if u.lfixed[li] {
            s.file_index = uring::FILE_INDEX_ALLOC;
        } else {
            s.op_flags = libc::SOCK_CLOEXEC as u32;
        }
        s.user_data = ud(K_ACCEPT, 0, li as u32);
    }

    fn u_arm_recv(&mut self, u: &mut U, idx: u32) {
        let c = &mut self.conns[idx as usize];
        let s = u.ring.sqe();
        s.opcode = uring::OP_RECV;
        s.fd = c.fd;
        s.flags = uring::SQE_BUFFER_SELECT | if c.fl & FIXED != 0 { uring::SQE_FIXED_FILE } else { 0 };
        s.ioprio = uring::RECV_MULTISHOT;
        s.buf_index = uring::BGID;
        s.user_data = ud(K_CONN, c.gen, idx);
        c.fl |= RECV;
    }

    fn u_arm_pg_recv(&mut self, u: &mut U, p: usize) {
        let pg = &mut self.pgs[p];
        if pg.st != PgSt::Up || pg.recv {
            return;
        }
        if pg.rx.capacity() - pg.rx.len() < 64 * 1024 {
            pg.rx.reserve(128 * 1024);
        }
        let s = u.ring.sqe();
        s.opcode = uring::OP_RECV;
        s.fd = pg.fd;
        s.addr = unsafe { pg.rx.as_mut_ptr().add(pg.rx.len()) } as u64;
        s.len = (pg.rx.capacity() - pg.rx.len()) as u32;
        s.user_data = ud(K_PGRECV, pg.epoch, p as u32);
        pg.recv = true;
    }

    fn u_pg_send(&mut self, u: &mut U, p: usize) {
        let pg = &mut self.pgs[p];
        if pg.st != PgSt::Up || pg.sending {
            return;
        }
        if pg.txoff >= pg.txing.len() {
            if pg.tx.is_empty() {
                return;
            }
            std::mem::swap(&mut pg.tx, &mut pg.txing);
            pg.tx.clear();
            pg.txoff = 0;
        }
        let s = u.ring.sqe();
        s.opcode = uring::OP_SEND;
        s.fd = pg.fd;
        s.addr = unsafe { pg.txing.as_ptr().add(pg.txoff) } as u64;
        s.len = (pg.txing.len() - pg.txoff) as u32;
        s.op_flags = libc::MSG_NOSIGNAL as u32;
        s.user_data = ud(K_PGSEND, pg.epoch, p as u32);
        pg.sending = true;
    }

    fn u_send(&mut self, u: &mut U, idx: u32) {
        let c = &mut self.conns[idx as usize];
        c.fl &= !INSEND;
        if c.fl & OPEN == 0 || c.fl & (CLOSING | SENDING) != 0 {
            return;
        }
        let Some(b) = c.b.as_mut() else { return };
        if b.off >= b.out.len() {
            if b.next.is_empty() {
                return;
            }
            std::mem::swap(&mut b.out, &mut b.next);
            b.next.clear();
            b.off = 0;
        }
        let s = u.ring.sqe();
        s.opcode = uring::OP_SEND;
        s.fd = c.fd;
        s.flags = if c.fl & FIXED != 0 { uring::SQE_FIXED_FILE } else { 0 };
        s.addr = unsafe { b.out.as_ptr().add(b.off) } as u64;
        s.len = (b.out.len() - b.off) as u32;
        s.op_flags = libc::MSG_NOSIGNAL as u32;
        s.user_data = ud(K_SEND, c.gen, idx);
        c.fl |= SENDING;
    }

    fn u_finish_close(&mut self, u: &mut U, idx: u32) {
        let c = &mut self.conns[idx as usize];
        if c.fl & CLOSING == 0 || c.fl & SENDING != 0 {
            return;
        }
        if c.fl & RECV != 0 {
            if c.fl & CANCELED == 0 {
                c.fl |= CANCELED;
                let target = ud(K_CONN, c.gen, idx);
                let s = u.ring.sqe();
                s.opcode = uring::OP_ASYNC_CANCEL;
                s.fd = -1;
                s.addr = target;
                s.flags = uring::SQE_CQE_SKIP_SUCCESS;
                s.user_data = ud(K_CANCEL, 0, idx);
            }
            return;
        }
        let s = u.ring.sqe();
        s.opcode = uring::OP_CLOSE;
        if c.fl & FIXED != 0 {
            s.fd = 0;
            s.file_index = c.fd as u32 + 1;
        } else {
            s.fd = c.fd;
        }
        s.flags = uring::SQE_CQE_SKIP_SUCCESS;
        s.user_data = ud(K_CLOSE, 0, idx);
        self.free_conn(idx);
    }

    fn u_cqe(&mut self, u: &mut U, c: &Cqe) {
        let kind = c.user_data >> 56;
        let gen = ((c.user_data >> 32) & 0xFF_FFFF) as u32;
        let idx = c.user_data as u32;
        match kind {
            K_CONN => self.u_recv(u, idx, gen, c),
            K_SEND => {
                let cn = &mut self.conns[idx as usize];
                if cn.gen != gen || cn.fl & OPEN == 0 {
                    return;
                }
                cn.fl &= !SENDING;
                if c.res <= 0 {
                    self.close_conn(idx);
                } else if let Some(b) = cn.b.as_mut() {
                    b.off += c.res as usize;
                }
                self.after_send(idx);
                if self.conns[idx as usize].fl & CLOSING != 0 {
                    self.u_finish_close(u, idx);
                }
            }
            K_ACCEPT => {
                let li = idx as usize;
                if c.res >= 0 {
                    let ci = self.new_conn(c.res, u.lfixed[li]);
                    self.u_arm_recv(u, ci);
                } else if c.res == -libc::ENFILE && u.lfixed[li] {
                    eprintln!("[w{}] fixed file table full ({} slots), using plain fds", self.id, u.fixed_slots);
                    u.lfixed[li] = false;
                }
                if c.flags & uring::CQE_F_MORE == 0 {
                    u.accept_rearm.push(li);
                }
            }
            K_PGRECV => {
                let p = idx as usize;
                if self.pgs[p].epoch != gen {
                    return;
                }
                self.pgs[p].recv = false;
                if self.pgs[p].st != PgSt::Up {
                    return;
                }
                if c.res <= 0 {
                    self.pg_dead(p, if c.res == 0 { "eof" } else { "recv error" });
                    return;
                }
                let pg = &mut self.pgs[p];
                unsafe { pg.rx.set_len(pg.rx.len() + c.res as usize) };
                pg.last_rx = self.now_ns;
                self.pg_input(p);
                self.u_arm_pg_recv(u, p);
            }
            K_PGSEND => {
                let p = idx as usize;
                let pg = &mut self.pgs[p];
                if pg.epoch != gen {
                    return;
                }
                pg.sending = false;
                if pg.st != PgSt::Up {
                    return;
                }
                if c.res <= 0 {
                    self.pg_dead(p, "send error");
                    return;
                }
                pg.txoff += c.res as usize;
                if pg.txoff >= pg.txing.len() {
                    pg.txing.clear();
                    pg.txoff = 0;
                }
                self.u_pg_send(u, p);
            }
            _ => {}
        }
    }

    fn u_recv(&mut self, u: &mut U, idx: u32, gen: u32, c: &Cqe) {
        let has_buf = c.flags & uring::CQE_F_BUFFER != 0;
        let bid = (c.flags >> 16) as u16;
        let more = c.flags & uring::CQE_F_MORE != 0;
        let live = (idx as usize) < self.conns.len() && self.conns[idx as usize].gen == gen && self.conns[idx as usize].fl & OPEN != 0;
        if !live {
            if has_buf {
                u.bufs.give_back(&mut u.ring, bid);
            }
            return;
        }
        if has_buf {
            if c.res > 0 {
                let data = u.bufs.data(bid, c.res as usize);
                self.client_data(idx, data);
            }
            u.bufs.give_back(&mut u.ring, bid);
        }
        let cn = &mut self.conns[idx as usize];
        if !more {
            cn.fl &= !RECV;
        }
        if c.res == -libc::ENOBUFS {
            self.stats.nobufs += 1;
            if !more && cn.fl & (CLOSING | REARM) == 0 {
                cn.fl |= REARM;
                self.rearm.push(idx);
            }
        } else if c.res <= 0 {
            self.close_conn(idx);
        } else if !more && cn.fl & (CLOSING | REARM) == 0 {
            cn.fl |= REARM;
            self.rearm.push(idx);
        }
        if self.conns[idx as usize].fl & CLOSING != 0 {
            self.u_finish_close(u, idx);
        }
    }

    fn after_send(&mut self, idx: u32) {
        let c = &mut self.conns[idx as usize];
        if c.fl & CLOSING != 0 {
            return;
        }
        let Some(b) = c.b.as_mut() else { return };
        if b.off < b.out.len() || !b.next.is_empty() {
            self.queue_send(idx);
            return;
        }
        b.out.clear();
        b.off = 0;
        if c.fl & CLOSE_AFTER != 0 && c.fl & BUSY == 0 {
            self.close_conn(idx);
        } else {
            self.maybe_release(idx);
        }
    }

    fn u_flush(&mut self, u: &mut U) {
        for p in std::mem::take(&mut self.pg_new) {
            self.u_arm_pg_recv(u, p);
        }
        for p in 0..self.pgs.len() {
            match self.pgs[p].st {
                PgSt::Up => {
                    if !self.pgs[p].tx.is_empty() {
                        self.u_pg_send(u, p);
                    }
                }
                PgSt::Draining => {
                    if !self.pgs[p].recv && !self.pgs[p].sending {
                        let pg = &mut self.pgs[p];
                        unsafe { libc::close(pg.fd) };
                        pg.fd = -1;
                        pg.st = PgSt::Down;
                        pg.retry_at = self.now_ns + 200_000_000;
                        pg.txing.clear();
                        pg.txoff = 0;
                    }
                }
                PgSt::Down => {}
            }
        }
        let ws = std::mem::take(&mut self.want_send);
        for &i in &ws {
            self.u_send(u, i);
        }
        self.want_send = ws;
        self.want_send.clear();
        let wc = std::mem::take(&mut self.want_close);
        for &i in &wc {
            self.u_finish_close(u, i);
        }
        self.want_close = wc;
        self.want_close.clear();
        let ra = std::mem::take(&mut self.rearm);
        for &i in &ra {
            let c = &mut self.conns[i as usize];
            c.fl &= !REARM;
            if c.fl & OPEN != 0 && c.fl & (CLOSING | RECV) == 0 {
                self.u_arm_recv(u, i);
            }
        }
        self.rearm = ra;
        self.rearm.clear();
        for li in std::mem::take(&mut u.accept_rearm) {
            self.u_arm_accept(u, li);
        }
    }

    fn run_uring(&mut self, mut u: U) {
        self.mode = if u.bufs.ring_mode() { "uring+pbufring" } else { "uring+providebufs" };
        for li in 0..self.listeners.len() {
            self.u_arm_accept(&mut u, li);
        }
        let mut cq: Vec<Cqe> = Vec::with_capacity(4096);
        let mut worked = false;
        let batch = self.sh.cfg.batch_ns;
        let minc = self.sh.cfg.min_complete;
        loop {
            self.u_flush(&mut u);
            if worked && batch > 0 {
                u.ring.submit_wait(minc, batch);
            } else {
                u.ring.submit_wait(1, 100_000_000);
            }
            self.now_ns = mono_ns();
            self.now_s = unix_s();
            let mut total = 0;
            loop {
                cq.clear();
                let n = u.ring.reap(&mut cq, 4096);
                if n == 0 {
                    break;
                }
                total += n;
                for c in &cq {
                    self.u_cqe(&mut u, c);
                }
            }
            self.stats.events += total as u64;
            self.end_pass();
            worked = total > 0;
        }
    }

    fn e_ctl(ep: i32, op: i32, fd: i32, events: u32, data: u64) {
        let mut ev = libc::epoll_event { events, u64: data };
        unsafe { libc::epoll_ctl(ep, op, fd, &mut ev) };
    }

    fn e_send(&mut self, idx: u32) {
        let c = &mut self.conns[idx as usize];
        c.fl &= !INSEND;
        if c.fl & OPEN == 0 || c.fl & (CLOSING | SENDING) != 0 {
            return;
        }
        let fd = c.fd;
        let Some(b) = c.b.as_mut() else { return };
        loop {
            if b.off >= b.out.len() {
                if b.next.is_empty() {
                    break;
                }
                std::mem::swap(&mut b.out, &mut b.next);
                b.next.clear();
                b.off = 0;
            }
            let n = unsafe {
                libc::send(fd, b.out.as_ptr().add(b.off) as *const _, b.out.len() - b.off, libc::MSG_NOSIGNAL)
            };
            if n > 0 {
                b.off += n as usize;
                continue;
            }
            let e = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if n < 0 && (e == libc::EAGAIN || e == libc::EINTR) {
                c.fl |= SENDING;
                return;
            }
            self.close_conn(idx);
            return;
        }
        self.after_send(idx);
    }

    fn e_read(&mut self, e: &mut E, idx: u32, rdhup: bool) {
        loop {
            let c = &self.conns[idx as usize];
            if c.fl & OPEN == 0 || c.fl & CLOSING != 0 {
                return;
            }
            let n = unsafe { libc::recv(c.fd, e.rbuf.as_mut_ptr() as *mut _, e.rbuf.len(), 0) };
            if n > 0 {
                let n = n as usize;
                let buf = std::mem::take(&mut e.rbuf);
                self.client_data(idx, &buf[..n]);
                e.rbuf = buf;
                if n < e.rbuf.len() && !rdhup {
                    return;
                }
                continue;
            }
            if n == 0 {
                self.close_conn(idx);
                return;
            }
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if err == libc::EAGAIN || err == libc::EINTR {
                return;
            }
            self.close_conn(idx);
            return;
        }
    }

    fn e_pg_read(&mut self, p: usize) {
        loop {
            let pg = &mut self.pgs[p];
            if pg.st != PgSt::Up {
                return;
            }
            if pg.rx.capacity() - pg.rx.len() < 64 * 1024 {
                pg.rx.reserve(128 * 1024);
            }
            let spare = pg.rx.capacity() - pg.rx.len();
            let n = unsafe { libc::recv(pg.fd, pg.rx.as_mut_ptr().add(pg.rx.len()) as *mut _, spare, 0) };
            if n > 0 {
                unsafe { pg.rx.set_len(pg.rx.len() + n as usize) };
                pg.last_rx = self.now_ns;
                self.pg_input(p);
                if (n as usize) < spare {
                    return;
                }
                continue;
            }
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if n < 0 && (err == libc::EAGAIN || err == libc::EINTR) {
                return;
            }
            self.pg_dead(p, if n == 0 { "eof" } else { "recv error" });
            return;
        }
    }

    fn e_pg_send(&mut self, p: usize) {
        let pg = &mut self.pgs[p];
        if pg.st != PgSt::Up || pg.want_out {
            return;
        }
        while pg.txoff < pg.tx.len() {
            let n = unsafe {
                libc::send(pg.fd, pg.tx.as_ptr().add(pg.txoff) as *const _, pg.tx.len() - pg.txoff, libc::MSG_NOSIGNAL)
            };
            if n > 0 {
                pg.txoff += n as usize;
                continue;
            }
            let err = std::io::Error::last_os_error().raw_os_error().unwrap_or(0);
            if n < 0 && (err == libc::EAGAIN || err == libc::EINTR) {
                pg.want_out = true;
                if pg.txoff > 0 {
                    pg.tx.drain(..pg.txoff);
                    pg.txoff = 0;
                }
                return;
            }
            self.pg_dead(p, "send error");
            return;
        }
        pg.tx.clear();
        pg.txoff = 0;
    }

    fn e_flush(&mut self, e: &mut E) {
        for p in std::mem::take(&mut self.pg_new) {
            let pg = &self.pgs[p];
            Self::e_ctl(
                e.ep,
                libc::EPOLL_CTL_ADD,
                pg.fd,
                (libc::EPOLLIN | libc::EPOLLOUT | libc::EPOLLRDHUP | libc::EPOLLET) as u32,
                ud(K_PGRECV, pg.epoch, p as u32),
            );
        }
        for p in 0..self.pgs.len() {
            match self.pgs[p].st {
                PgSt::Up => {
                    if !self.pgs[p].tx.is_empty() {
                        self.e_pg_send(p);
                    }
                }
                PgSt::Draining => {
                    let pg = &mut self.pgs[p];
                    unsafe { libc::close(pg.fd) };
                    pg.fd = -1;
                    pg.st = PgSt::Down;
                    pg.retry_at = self.now_ns + 200_000_000;
                    pg.tx.clear();
                    pg.txoff = 0;
                }
                PgSt::Down => {}
            }
        }
        let ws = std::mem::take(&mut self.want_send);
        for &i in &ws {
            self.e_send(i);
        }
        self.want_send = ws;
        self.want_send.clear();
        loop {
            let wc = std::mem::take(&mut self.want_close);
            if wc.is_empty() {
                break;
            }
            for &i in &wc {
                let c = &self.conns[i as usize];
                if c.fl & CLOSING != 0 {
                    unsafe { libc::close(c.fd) };
                    self.free_conn(i);
                }
            }
        }
    }

    fn e_accept(&mut self, e: &mut E, li: usize) {
        for _ in 0..512 {
            let fd = unsafe {
                libc::accept4(self.listeners[li], std::ptr::null_mut(), std::ptr::null_mut(), libc::SOCK_NONBLOCK | libc::SOCK_CLOEXEC)
            };
            if fd < 0 {
                return;
            }
            let idx = self.new_conn(fd, false);
            let gen = self.conns[idx as usize].gen;
            Self::e_ctl(
                e.ep,
                libc::EPOLL_CTL_ADD,
                fd,
                (libc::EPOLLIN | libc::EPOLLOUT | libc::EPOLLRDHUP | libc::EPOLLET) as u32,
                ud(K_CONN, gen, idx),
            );
        }
    }

    fn run_epoll(&mut self, mut e: E) {
        self.mode = "epoll";
        for (li, &l) in self.listeners.iter().enumerate() {
            sys::set_nonblock(l);
            Self::e_ctl(e.ep, libc::EPOLL_CTL_ADD, l, libc::EPOLLIN as u32, ud(K_ACCEPT, 0, li as u32));
        }
        let mut evs: Vec<libc::epoll_event> = vec![libc::epoll_event { events: 0, u64: 0 }; 1024];
        loop {
            self.e_flush(&mut e);
            let n = unsafe { libc::epoll_wait(e.ep, evs.as_mut_ptr(), evs.len() as i32, 100) };
            let t0 = mono_ns();
            self.now_ns = t0;
            self.now_s = unix_s();
            let n = n.max(0) as usize;
            for k in 0..n {
                let ev = evs[k];
                let data = ev.u64;
                let events = ev.events;
                let kind = data >> 56;
                let gen = ((data >> 32) & 0xFF_FFFF) as u32;
                let idx = data as u32;
                match kind {
                    K_ACCEPT => self.e_accept(&mut e, idx as usize),
                    K_CONN => {
                        let c = &self.conns[idx as usize];
                        if c.gen != gen || c.fl & OPEN == 0 {
                            continue;
                        }
                        if events & (libc::EPOLLIN | libc::EPOLLRDHUP | libc::EPOLLHUP | libc::EPOLLERR) as u32 != 0 {
                            self.e_read(&mut e, idx, events & (libc::EPOLLRDHUP | libc::EPOLLHUP | libc::EPOLLERR) as u32 != 0);
                        }
                        if events & libc::EPOLLOUT as u32 != 0 {
                            let c = &mut self.conns[idx as usize];
                            if c.fl & SENDING != 0 {
                                c.fl &= !SENDING;
                                self.queue_send(idx);
                            }
                        }
                    }
                    K_PGRECV => {
                        let p = idx as usize;
                        if self.pgs[p].epoch != gen || self.pgs[p].st != PgSt::Up {
                            continue;
                        }
                        if events & (libc::EPOLLIN | libc::EPOLLRDHUP | libc::EPOLLHUP | libc::EPOLLERR) as u32 != 0 {
                            self.e_pg_read(p);
                        }
                        if events & libc::EPOLLOUT as u32 != 0 && self.pgs[p].want_out {
                            self.pgs[p].want_out = false;
                            self.e_pg_send(p);
                        }
                    }
                    _ => {}
                }
            }
            self.stats.events += n as u64;
            self.end_pass();
            let wait = self.sh.cfg.epoll_batch_ns;
            if wait > 0 && n > 0 {
                let spent = mono_ns() - t0;
                if spent < wait {
                    self.e_flush(&mut e);
                    let left = wait - spent;
                    let ts = libc::timespec { tv_sec: 0, tv_nsec: left as _ };
                    unsafe { libc::nanosleep(&ts, std::ptr::null_mut()) };
                }
            }
        }
    }
}

fn make_uring(cfg: &Config, id: usize) -> Result<U, String> {
    let mut ring = Ring::new(4096, 65536)?;
    let bufs = uring::Bufs::new(&mut ring, cfg.bufs, cfg.buf_size)?;
    let lim = sys::nofile_limit();
    let mut nr = cfg.max_fixed;
    let mut fixed_slots = 0;
    while nr >= 65536 {
        if (nr as u64) <= lim && ring.register_files_sparse(nr) == 0 {
            fixed_slots = nr;
            break;
        }
        nr /= 2;
    }
    if id == 0 {
        eprintln!(
            "io_uring: flags={:#x} buffers={} fixed_files={}",
            ring.setup_flags,
            if bufs.ring_mode() { "pbuf_ring" } else { "provide_buffers" },
            fixed_slots
        );
    }
    Ok(U { ring, bufs, lfixed: vec![fixed_slots > 0; cfg.ports.len()], accept_rearm: Vec::new(), fixed_slots })
}

pub fn run(id: usize, sh: &'static Shared) {
    let cfg = &sh.cfg;
    if cfg.sched_idle {
        sys::set_sched_idle();
    }
    let cpu = sys::current_cpu();
    let mut listeners = Vec::new();
    for &p in &cfg.ports {
        match sys::listener(cfg.ip, p, cfg.defer, cpu) {
            Ok(fd) => listeners.push(fd),
            Err(e) => {
                eprintln!("listen on port {} failed: {}", p, e);
                std::process::exit(1);
            }
        }
    }
    let mut w = Worker::new(id, sh, listeners, cpu);
    for p in 0..w.pgs.len() {
        w.connect_pg(p);
    }
    w.update_ready();
    if !cfg.epoll {
        match make_uring(cfg, id) {
            Ok(u) => return w.run_uring(u),
            Err(e) => eprintln!("[w{}] io_uring unavailable ({}), using epoll", id, e),
        }
    }
    let ep = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
    w.run_epoll(E { ep, rbuf: vec![0u8; 64 * 1024] });
}
