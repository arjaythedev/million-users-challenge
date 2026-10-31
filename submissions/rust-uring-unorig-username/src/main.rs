//! Feed API (SPEC.md): one thread per core, each with its own io_uring, its own SO_REUSEPORT listeners on PORT
//! and EXTRA_PORT and its own Postgres connections, spoken to with the small wire client in pg.rs.
mod http;
mod json;
mod pg;

use std::collections::VecDeque;
use std::ffi::c_void;
use std::net::{SocketAddr, ToSocketAddrs};
use std::ptr;
use std::sync::Arc;
use std::sync::atomic::{AtomicU16, Ordering};
use std::time::{Duration, Instant};

use hmac::{Hmac, Mac};
use io_uring::types::{BufRingEntry, CancelBuilder, Fd, Fixed, SubmitArgs, Timespec};
use io_uring::{IoUring, cqueue, opcode, squeue};
use libdeflater::{CompressionLvl, Compressor};
use sha2::Sha256;

use http::{Kind, MAX_REQ, Parse, Req, num, ts};

// Postgres connections in total, split over the threads: sized by the load, not the core count (rule 5: at most 512).
const PG_READS: usize = 96;
const PG_WRITES: usize = 32; // each autocommit INSERT waits for its own WAL flush: writes need connections in flight
const GZIP_MIN: usize = 1024; // only the feed is worth compressing
const NBUF: usize = 4096; // provided receive buffers per thread (a power of two): the most requests one pass reads
const BSZ: usize = 2048;
const PG_IN: usize = 256 << 10; // receive buffer per Postgres connection
const PACK: usize = 64; // reads in flight per connection before the next one is used: fuller packs, fewer Syncs
const WAIT_N: usize = 128; // a wait returns at 128 completions...
const WAIT_NS: u32 = 2_000_000; // ...or after 2 ms, once the thread has work

// user_data: the low 3 bits tag the operation. A send's user_data is its Box<Sop> pointer (tag 0).
const T_SEND: u64 = 0;
const T_ACCEPT: u64 = 1; // | listener << 3
const T_RECV: u64 = 2; // | slot << 8 | generation << 32
const T_PGRECV: u64 = 3; // | pg << 8
const T_PGSEND: u64 = 4; // | pg << 8
const T_NOP: u64 = 5;

macro_rules! post_sql {
    ($tail:literal) => {
        concat!(
            "SELECT p.id, p.body, p.created_at, u.username, ",
            "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count ",
            "FROM posts p JOIN users u ON u.id = p.user_id ",
            $tail
        )
    };
}
// (name, SQL, parameter type OIDs: 20 = int8, 25 = text), prepared once per connection
const STMTS: [(&str, &str, &[u32]); 5] = [
    ("o", "SELECT 1", &[]),
    ("f", post_sql!("ORDER BY p.created_at DESC, p.id DESC LIMIT 20"), &[]),
    ("p", post_sql!("WHERE p.id = $1"), &[20]),
    ("i", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", &[20, 25]),
    ("l", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", &[20, 20]),
];

struct Cfg {
    url: pg::Url,
    key: Hmac<Sha256>,
    addrs: Vec<SocketAddr>,
    started: Instant,
    reads: usize, // Postgres connections per thread
    writes: usize,
}

#[derive(Default, Clone, Copy, PartialEq)]
enum St {
    #[default]
    Idle, // waiting for a request
    Db,   // its query is in flight
    Send, // a response is being sent
}

/// 16 bytes per connection; input that has to wait (a partial or pipelined request) gets a boxed Vec.
#[derive(Default)]
struct Conn {
    g: u32,
    open: bool,
    st: St,
    closing: bool,  // close once the response in flight is sent
    continued: bool, // "100 Continue" already sent for the request being read
    inb: Option<Box<Vec<u8>>>,
}

struct Sop {
    who: u64,
    d: Vec<u8>,
    off: usize,
}

enum PK {
    Health,
    Feed,
    Post,
    Create(Box<(String, String)>), // body text, author
    Like(i64),
}

struct Pending {
    who: u64,
    k: PK,
    gzip: bool,
    close: bool,
    last: bool, // a read whose Sync ends its pack
}

struct Pg {
    fd: i32, // -1 once closed
    alive: bool,
    out: Vec<u8>,  // requests queued since the last send
    open: bool,    // out holds reads not yet closed by a Sync
    sbuf: Vec<u8>, // the send in flight
    soff: usize,
    sending: bool,
    inb: Vec<u8>, // replies; [rpos..] is not yet answered
    rpos: usize,
    receiving: bool,
    q: VecDeque<Pending>,
    retry: Instant,
}

impl Pg {
    fn new(fd: i32) -> Pg {
        Pg {
            fd,
            alive: true,
            out: Vec::with_capacity(16384),
            open: false,
            sbuf: Vec::with_capacity(16384),
            soff: 0,
            sending: false,
            inb: Vec::with_capacity(PG_IN),
            rpos: 0,
            receiving: false,
            q: VecDeque::with_capacity(256),
            retry: Instant::now(),
        }
    }
}

struct Srv {
    ring: IoUring,
    conns: Vec<Conn>,
    br: *mut BufRingEntry,
    tail: u16,
    bufs: Vec<u8>,
    lfds: Vec<i32>,
    pgs: Vec<Pg>,
    cfg: Arc<Cfg>,
    key: Hmac<Sha256>,
    body: Vec<u8>,
    zb: Vec<u8>,
    z: Compressor,
    pool: Vec<Vec<u8>>, // response buffers, reused
    rearm: Vec<u64>,
    rearm_accept: Vec<usize>,
    reconnect: Instant, // no reconnect attempt before this (set after a failed one)
}

fn die(what: &str, e: impl std::fmt::Display) -> ! {
    eprintln!("{what}: {e}");
    std::process::exit(1)
}

fn main() {
    let env = |k: &str| std::env::var(k).ok().filter(|v| !v.is_empty());
    let host = env("HOST").unwrap_or_else(|| "127.0.0.1".into());
    let mut addrs = Vec::new();
    for port in [env("PORT").or(Some("3000".into())), env("EXTRA_PORT")].into_iter().flatten() {
        let port = port.parse::<u16>().unwrap_or_else(|e| die("bad port", e));
        addrs.push((host.as_str(), port).to_socket_addrs().unwrap_or_else(|e| die("bad HOST", e)).next().unwrap());
    }
    let secret = env("JWT_SECRET").unwrap_or_else(|| die("config", "JWT_SECRET must be set"));
    let url = env("DATABASE_URL").unwrap_or_else(|| die("config", "DATABASE_URL must be set"));
    let n = std::thread::available_parallelism().map_or(1, |n| n.get());
    let cfg = Arc::new(Cfg {
        reads: PG_READS.div_ceil(n),
        writes: PG_WRITES.div_ceil(n),
        url: pg::parse_url(&url).unwrap_or_else(|| die("config", "bad DATABASE_URL")),
        key: Hmac::new_from_slice(secret.as_bytes()).unwrap(),
        addrs,
        started: Instant::now(),
    });
    unsafe {
        let mut rl: libc::rlimit = std::mem::zeroed();
        libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl);
        rl.rlim_cur = rl.rlim_max;
        libc::setrlimit(libc::RLIMIT_NOFILE, &rl);
    }
    eprintln!("feed: {n} threads, {} + {} Postgres connections (reads + writes), listening on {:?}", n * cfg.reads, n * cfg.writes, cfg.addrs);
    let threads: Vec<_> = (0..n)
        .map(|_| {
            let cfg = cfg.clone();
            std::thread::spawn(move || Srv::new(cfg).run())
        })
        .collect();
    for t in threads {
        let _ = t.join();
    }
}

fn listener(a: &SocketAddr) -> i32 {
    unsafe {
        let fam = if a.is_ipv4() { libc::AF_INET } else { libc::AF_INET6 };
        let fd = libc::socket(fam, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0);
        let one: libc::c_int = 1;
        for (lvl, opt) in [(libc::SOL_SOCKET, libc::SO_REUSEADDR), (libc::SOL_SOCKET, libc::SO_REUSEPORT), (libc::IPPROTO_TCP, libc::TCP_NODELAY)] {
            libc::setsockopt(fd, lvl, opt, &one as *const _ as *const c_void, 4); // accepted sockets inherit TCP_NODELAY
        }
        let rc = match a {
            SocketAddr::V4(v) => {
                let mut s: libc::sockaddr_in = std::mem::zeroed();
                s.sin_family = libc::AF_INET as u16;
                s.sin_port = v.port().to_be();
                s.sin_addr.s_addr = u32::from(*v.ip()).to_be();
                libc::bind(fd, &s as *const _ as *const libc::sockaddr, std::mem::size_of_val(&s) as u32)
            }
            SocketAddr::V6(v) => {
                let mut s: libc::sockaddr_in6 = std::mem::zeroed();
                s.sin6_family = libc::AF_INET6 as u16;
                s.sin6_port = v.port().to_be();
                s.sin6_addr.s6_addr = v.ip().octets();
                libc::bind(fd, &s as *const _ as *const libc::sockaddr, std::mem::size_of_val(&s) as u32)
            }
        };
        if fd < 0 || rc != 0 || libc::listen(fd, 65535) != 0 {
            die("bind/listen", std::io::Error::last_os_error());
        }
        fd
    }
}

fn fail(b: &mut Vec<u8>, msg: &str) {
    b.extend_from_slice(b"{\"error\":\"");
    b.extend_from_slice(msg.as_bytes());
    b.extend_from_slice(b"\"}");
}

const NO_DB: &[u8] = b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}";

/// One feed or post row: id int8, body text, created_at timestamptz, username text, like_count int4.
fn post_row(b: &mut Vec<u8>, m: &[u8]) -> bool {
    let Some([id, body, at, user, likes]) = pg::cols::<5>(m) else { return false };
    let (Some(id), Some(at), Some(likes)) = (pg::int(id), pg::int(at), pg::int(likes)) else { return false };
    b.extend_from_slice(b"{\"id\":");
    num(b, id);
    b.extend_from_slice(b",\"body\":\"");
    json::esc(b, body);
    b.extend_from_slice(b"\",\"created_at\":\"");
    ts(b, at);
    b.extend_from_slice(b"\",\"author\":\"");
    json::esc(b, user);
    b.extend_from_slice(b"\",\"like_count\":");
    num(b, likes);
    b.push(b'}');
    true
}

impl Srv {
    fn new(cfg: Arc<Cfg>) -> Srv {
        let ring = IoUring::builder()
            .setup_single_issuer()
            .setup_defer_taskrun() // completions run inside our own wait, never as interrupts of the loop
            .setup_submit_all()
            .setup_cqsize(32768)
            .build(4096)
            .unwrap_or_else(|e| die("io_uring setup (needs Linux 6.1+ with io_uring enabled)", e));
        let mut rl: libc::rlimit = unsafe { std::mem::zeroed() };
        unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut rl) };
        // Accepted sockets live in this ring's registered file table (no fd table entry, no per-op fd lookup).
        // One table per thread, capped at 1M entries (and RLIMIT_NOFILE); add rings if one thread ever needs more.
        let tsize = rl.rlim_cur.min(1 << 20) as u32;
        ring.submitter().register_files_sparse(tsize).unwrap_or_else(|e| die("register files", e));
        let br = unsafe {
            libc::mmap(ptr::null_mut(), NBUF * 16, libc::PROT_READ | libc::PROT_WRITE, libc::MAP_PRIVATE | libc::MAP_ANONYMOUS, -1, 0)
        };
        if br == libc::MAP_FAILED {
            die("mmap", std::io::Error::last_os_error());
        }
        unsafe { ring.submitter().register_buf_ring_with_flags(br as u64, NBUF as u16, 0, 0) }
            .unwrap_or_else(|e| die("register buffer ring (needs Linux 5.19+)", e));
        let pgs = (0..cfg.reads + cfg.writes)
            .map(|_| Pg::new(pg::connect(&cfg.url, &STMTS).unwrap_or_else(|e| die("postgres", e))))
            .collect();
        let mut s = Srv {
            ring,
            conns: Vec::new(),
            br: br as *mut BufRingEntry,
            tail: 0,
            bufs: vec![0; NBUF * BSZ],
            lfds: cfg.addrs.iter().map(listener).collect(),
            pgs,
            key: cfg.key.clone(),
            cfg,
            body: Vec::with_capacity(16384),
            zb: Vec::with_capacity(16384),
            z: Compressor::new(CompressionLvl::new(1).unwrap()),
            pool: Vec::new(),
            rearm: Vec::new(),
            rearm_accept: Vec::new(),
            reconnect: Instant::now(),
        };
        for i in 0..NBUF {
            s.buf_add(i as u16);
        }
        for i in 0..s.lfds.len() {
            s.arm_accept(i);
        }
        for i in 0..s.pgs.len() {
            s.pg_arm(i);
        }
        s
    }

    fn buf_add(&mut self, bid: u16) {
        unsafe {
            let e = &mut *self.br.add((self.tail & (NBUF as u16 - 1)) as usize);
            e.set_addr(self.bufs.as_ptr().add(bid as usize * BSZ) as u64);
            e.set_len(BSZ as u32);
            e.set_bid(bid);
            self.tail = self.tail.wrapping_add(1);
            (*(BufRingEntry::tail(self.br) as *const AtomicU16)).store(self.tail, Ordering::Release);
        }
    }

    fn push(&mut self, e: &[squeue::Entry]) {
        while unsafe { self.ring.submission().push_multiple(e) }.is_err() {
            let _ = self.ring.submit();
        }
    }

    fn conn(&mut self, who: u64) -> Option<&mut Conn> {
        let c = self.conns.get_mut((who >> 8 & 0xff_ffff) as usize)?;
        (c.open && c.g == (who >> 32) as u32).then_some(c)
    }

    fn arm_accept(&mut self, i: usize) {
        let e = opcode::AcceptMulti::new(Fd(self.lfds[i])).allocate_file_index(true).build();
        self.push(&[e.user_data(T_ACCEPT | (i as u64) << 3)]);
    }

    fn arm_recv(&mut self, who: u64) {
        let slot = (who >> 8 & 0xff_ffff) as u32;
        self.push(&[opcode::RecvMulti::new(Fixed(slot), 0).build().user_data(who)]);
    }

    fn close(&mut self, who: u64) {
        let slot = (who >> 8 & 0xff_ffff) as u32;
        let c = &mut self.conns[slot as usize];
        *c = Conn { g: c.g.wrapping_add(1), ..Conn::default() };
        self.push(&[
            opcode::AsyncCancel2::new(CancelBuilder::fd(Fixed(slot)).all()).build().flags(squeue::Flags::IO_HARDLINK).user_data(T_NOP),
            opcode::Close::new(Fixed(slot)).build().user_data(T_NOP),
        ]);
    }

    fn send(&mut self, who: u64, d: Vec<u8>) {
        if let Some(c) = self.conn(who) {
            c.st = St::Send;
        }
        let o = Box::new(Sop { who, d, off: 0 });
        self.send_op(o);
    }

    fn send_op(&mut self, o: Box<Sop>) {
        let slot = (o.who >> 8 & 0xff_ffff) as u32;
        let e = opcode::Send::new(Fixed(slot), o.d[o.off..].as_ptr(), (o.d.len() - o.off) as u32).flags(libc::MSG_NOSIGNAL);
        self.push(&[e.build().user_data(Box::into_raw(o) as u64)]);
    }

    fn send_done(&mut self, mut o: Box<Sop>, res: i32) {
        let who = o.who;
        let alive = self.conn(who).is_some();
        if alive && res > 0 {
            o.off += res as usize;
            if o.off < o.d.len() {
                return self.send_op(o); // short send: the rest
            }
        }
        if self.pool.len() < 4096 {
            let mut d = std::mem::take(&mut o.d);
            d.clear();
            self.pool.push(d);
        }
        if !alive {
            return;
        }
        if res <= 0 {
            return self.close(who);
        }
        let c = self.conn(who).unwrap();
        c.st = St::Idle;
        if c.closing {
            return self.close(who);
        }
        self.process_inb(who);
    }

    /// Frames self.body as a response (gzipped when asked and worth it) and sends it.
    fn respond(&mut self, who: u64, status: u16, gzip: bool, close: bool) {
        let Some(c) = self.conn(who) else { return };
        c.closing |= close;
        let mut out = self.pool.pop().unwrap_or_else(|| Vec::with_capacity(8192));
        out.extend_from_slice(match status {
            200 => b"HTTP/1.1 200 OK\r\n".as_slice(),
            201 => b"HTTP/1.1 201 Created\r\n",
            400 => b"HTTP/1.1 400 Bad Request\r\n",
            401 => b"HTTP/1.1 401 Unauthorized\r\n",
            404 => b"HTTP/1.1 404 Not Found\r\n",
            503 => b"HTTP/1.1 503 Service Unavailable\r\n",
            _ => b"HTTP/1.1 500 Internal Server Error\r\n",
        });
        if close {
            out.extend_from_slice(b"Connection: close\r\n");
        }
        out.extend_from_slice(b"Content-Type: application/json\r\n");
        let body = if gzip && self.body.len() >= GZIP_MIN {
            out.extend_from_slice(b"Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
            self.zb.resize(self.z.gzip_compress_bound(self.body.len()), 0);
            let n = self.z.gzip_compress(&self.body, &mut self.zb).expect("gzip bound");
            &self.zb[..n]
        } else {
            &self.body[..]
        };
        out.extend_from_slice(b"Content-Length: ");
        num(&mut out, body.len() as i64);
        out.extend_from_slice(b"\r\n\r\n");
        out.extend_from_slice(body);
        self.send(who, out);
    }

    fn reply(&mut self, who: u64, status: u16, msg: &str, gzip: bool, close: bool) {
        self.body.clear();
        fail(&mut self.body, msg);
        self.respond(who, status, gzip, close);
    }

    /// Serves at most one request from the start of buf. Returns the bytes used, None once the connection closed.
    fn process(&mut self, who: u64, buf: &[u8]) -> Option<usize> {
        match http::parse(buf, &self.key) {
            Parse::Done(r, used) => {
                self.conn(who)?.continued = false;
                self.dispatch(who, r);
                Some(used)
            }
            Parse::More(cont) => {
                if buf.len() > MAX_REQ {
                    self.close(who);
                    return None;
                }
                let c = self.conn(who)?;
                if cont && !c.continued {
                    c.continued = true;
                    let mut v = self.pool.pop().unwrap_or_default();
                    v.extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
                    self.send(who, v);
                }
                Some(0)
            }
            Parse::Bad => {
                self.close(who);
                None
            }
        }
    }

    fn process_inb(&mut self, who: u64) {
        let Some(c) = self.conn(who) else { return };
        let Some(mut v) = c.inb.take() else { return };
        let Some(used) = self.process(who, &v[..]) else { return };
        v.drain(..used);
        if !v.is_empty() {
            if let Some(c) = self.conn(who) {
                c.inb = Some(v);
            }
        }
    }

    fn on_data(&mut self, who: u64, p: &[u8]) {
        let c = self.conn(who).unwrap();
        if c.st == St::Idle && c.inb.is_none() {
            let Some(used) = self.process(who, p) else { return };
            if used < p.len() {
                if let Some(c) = self.conn(who) {
                    c.inb = Some(Box::new(p[used..].to_vec()));
                }
            }
            return;
        }
        let inb = c.inb.get_or_insert_with(Default::default);
        inb.extend_from_slice(p);
        if inb.len() > 4 * MAX_REQ {
            return self.close(who); // keeps sending without reading the replies
        }
        if c.st == St::Idle {
            self.process_inb(who);
        }
    }

    /// Reads: the first live connection with fewer than PACK requests in flight, so the queries of a batch share a
    /// few sends and Postgres wake-ups (fewer syscalls on both sides). Writes, and reads once all are that full: the
    /// least loaded, so commits (one WAL flush each) run side by side. Writes have their own connections.
    fn pick(&self, write: bool) -> Option<usize> {
        let n = self.pgs.len();
        let r = n - self.cfg.writes;
        let range = if write { r..n } else { 0..r };
        if !write {
            if let Some(i) = range.clone().find(|&i| self.pgs[i].alive && self.pgs[i].q.len() < PACK) {
                return Some(i);
            }
        }
        range.filter(|&i| self.pgs[i].alive).min_by_key(|&i| self.pgs[i].q.len())
    }

    fn dispatch(&mut self, who: u64, r: Req) {
        let Req { kind, gzip, close } = r;
        if let Kind::Fail(st, msg) = kind {
            return self.reply(who, st, msg, gzip, close);
        }
        let write = matches!(kind, Kind::Create(..) | Kind::Like(..));
        let Some(i) = self.pick(write) else {
            if matches!(kind, Kind::Health) {
                self.body.clear();
                self.body.extend_from_slice(NO_DB);
                return self.respond(who, 503, gzip, close);
            }
            return self.reply(who, 500, "internal server error", gzip, close);
        };
        let o = &mut self.pgs[i].out;
        let k = match kind {
            Kind::Health => {
                pg::query(o, b"o", &[]);
                PK::Health
            }
            Kind::Feed => {
                pg::query(o, b"f", &[]);
                PK::Feed
            }
            Kind::Post(id) => {
                pg::query(o, b"p", &[(&id.to_be_bytes(), true)]);
                PK::Post
            }
            Kind::Create(uid, name, text) => {
                pg::query(o, b"i", &[(&uid.to_be_bytes(), true), (text.as_bytes(), false)]);
                PK::Create(Box::new((text, name)))
            }
            Kind::Like(uid, pid) => {
                pg::query(o, b"l", &[(&uid.to_be_bytes(), true), (&pid.to_be_bytes(), true)]);
                PK::Like(pid)
            }
            Kind::Fail(..) => unreachable!(),
        };
        // A write gets its own Sync: its own transaction, answered after its commit. The reads queued on a
        // connection in one pass share the Sync pg_flush adds: one Postgres flush and fewer packets for the pack.
        let pg = &mut self.pgs[i];
        if write {
            pg.out.extend_from_slice(pg::SYNC);
        } else {
            pg.open = true;
        }
        pg.q.push_back(Pending { who, k, gzip, close, last: false });
        if let Some(c) = self.conn(who) {
            c.st = St::Db;
        }
    }

    /// Answers one request from its reply messages (everything up to and including its ReadyForQuery).
    fn finish(&mut self, p: Pending, msgs: &[u8]) {
        if self.conn(p.who).is_none() {
            return;
        }
        let b = &mut self.body;
        b.clear();
        let (mut rows, mut tag, mut err, mut bad) = (0, &[][..], None, false);
        match p.k {
            PK::Feed => b.extend_from_slice(b"{\"posts\":["),
            PK::Post => b.extend_from_slice(b"{\"post\":"),
            _ => {}
        }
        for (t, m) in (pg::Msgs { b: msgs, p: 0 }) {
            match t {
                b'D' => {
                    match &p.k {
                        PK::Feed => {
                            if rows > 0 {
                                b.push(b',');
                            }
                            bad |= !post_row(b, m);
                        }
                        PK::Post if rows == 0 => bad |= !post_row(b, m),
                        PK::Create(x) => match pg::cols::<2>(m).and_then(|[id, at]| Some((pg::int(id)?, pg::int(at)?))) {
                            Some((id, at)) => {
                                b.extend_from_slice(b"{\"post\":{\"id\":");
                                num(b, id);
                                b.extend_from_slice(b",\"body\":\"");
                                json::esc(b, x.0.as_bytes());
                                b.extend_from_slice(b"\",\"created_at\":\"");
                                ts(b, at);
                                b.extend_from_slice(b"\",\"author\":\"");
                                json::esc(b, x.1.as_bytes());
                                b.extend_from_slice(b"\",\"like_count\":0}}");
                            }
                            None => bad = true,
                        },
                        _ => {}
                    }
                    rows += 1;
                }
                b'C' => tag = m,
                b'E' => err = Some(pg::err_fields(m)),
                _ => {}
            }
        }
        let status = if let Some((msg, code)) = err {
            b.clear();
            match p.k {
                PK::Like(_) if code == b"23503" => {
                    fail(b, "post not found");
                    404
                }
                PK::Health => {
                    b.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
                    json::esc(b, msg);
                    b.extend_from_slice(b"\"}");
                    503
                }
                _ => {
                    eprintln!("query failed: {}", String::from_utf8_lossy(msg));
                    fail(b, "internal server error");
                    500
                }
            }
        } else if bad {
            b.clear();
            fail(b, "internal server error");
            500
        } else {
            match p.k {
                PK::Feed => {
                    b.extend_from_slice(b"]}");
                    200
                }
                PK::Post if rows == 0 => {
                    b.clear();
                    fail(b, "post not found");
                    404
                }
                PK::Post => {
                    b.push(b'}');
                    200
                }
                PK::Create(_) if rows == 1 => 201,
                PK::Create(_) => {
                    b.clear();
                    fail(b, "internal server error");
                    500
                }
                PK::Like(pid) => {
                    // CommandComplete "INSERT 0 <rows>"
                    let new = tag.strip_suffix(b"\0").unwrap_or(tag).ends_with(b" 1");
                    b.extend_from_slice(b"{\"liked\":true,\"already_liked\":");
                    b.extend_from_slice(if new { b"false" } else { b"true" });
                    b.extend_from_slice(b",\"post_id\":");
                    num(b, pid);
                    b.push(b'}');
                    if new { 201 } else { 200 }
                }
                PK::Health => {
                    b.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                    num(b, self.cfg.started.elapsed().as_secs() as i64);
                    b.push(b'}');
                    200
                }
            }
        };
        self.respond(p.who, status, p.gzip, p.close);
    }

    fn pg_arm(&mut self, i: usize) {
        let pg = &mut self.pgs[i];
        if pg.inb.capacity() - pg.inb.len() < 64 * 1024 {
            pg.inb.drain(..pg.rpos);
            pg.rpos = 0;
            pg.inb.reserve(64 * 1024); // no receive is in flight, so the buffer may move
        }
        let n = pg.inb.len();
        let spare = (pg.inb.capacity() - n) as u32;
        let e = opcode::Recv::new(Fd(pg.fd), unsafe { pg.inb.as_mut_ptr().add(n) }, spare).build();
        pg.receiving = true;
        self.push(&[e.user_data(T_PGRECV | (i as u64) << 8)]);
    }

    fn pg_recv(&mut self, i: usize, res: i32) {
        self.pgs[i].receiving = false;
        if !self.pgs[i].alive {
            return;
        }
        if res <= 0 {
            return self.pg_dead(i, if res == 0 { "closed by the server".into() } else { std::io::Error::from_raw_os_error(-res).to_string() });
        }
        let mut inb = std::mem::take(&mut self.pgs[i].inb);
        unsafe { inb.set_len(inb.len() + res as usize) };
        let mut it = pg::Msgs { b: &inb, p: self.pgs[i].rpos };
        let mut start = it.p;
        let mut dead = None;
        // A write ends at its ReadyForQuery (after the commit). A read ends at its CommandComplete or error, and its
        // pack's ReadyForQuery follows the pack's last read. An error makes Postgres skip the pack's later reads.
        let write = i >= self.pgs.len() - self.cfg.writes;
        while let Some((t, m)) = it.next() {
            if t == b'E' && pg::fatal(m) {
                // e.g. the backend was terminated: the queue may hold requests not even sent, so fail them all
                dead = Some(String::from_utf8_lossy(pg::err_fields(m).0).into_owned());
                break;
            }
            let done = if write { t == b'Z' } else { t == b'C' || t == b'E' };
            if !done {
                if t == b'Z' {
                    start = it.p; // a read pack's end
                }
                continue;
            }
            let Some(p) = self.pgs[i].q.pop_front() else {
                dead = Some("unexpected reply".into());
                break;
            };
            let mut skip = t == b'E' && !p.last;
            self.finish(p, &inb[start..it.p]);
            start = it.p;
            // shortcut: the skipped reads answer 500 instead of being re-sent; reads only fail when Postgres is in trouble.
            while skip {
                let Some(p) = self.pgs[i].q.pop_front() else { break };
                skip = !p.last;
                self.reply(p.who, 500, "internal server error", p.gzip, p.close);
            }
        }
        let pg = &mut self.pgs[i];
        if start == inb.len() {
            inb.clear();
            start = 0;
        }
        pg.inb = inb;
        pg.rpos = start;
        if let Some(why) = dead {
            return self.pg_dead(i, why);
        }
        self.pg_arm(i);
    }

    fn pg_flush(&mut self) {
        for i in 0..self.pgs.len() {
            let pg = &mut self.pgs[i];
            if !pg.alive || pg.sending || pg.out.is_empty() {
                continue;
            }
            if pg.open {
                pg.out.extend_from_slice(pg::SYNC);
                if let Some(p) = pg.q.back_mut() {
                    p.last = true;
                }
                pg.open = false;
            }
            std::mem::swap(&mut pg.out, &mut pg.sbuf);
            pg.soff = 0;
            pg.sending = true;
            let e = opcode::Send::new(Fd(pg.fd), pg.sbuf.as_ptr(), pg.sbuf.len() as u32).flags(libc::MSG_NOSIGNAL).build();
            self.push(&[e.user_data(T_PGSEND | (i as u64) << 8)]);
        }
    }

    fn pg_sent(&mut self, i: usize, res: i32) {
        let pg = &mut self.pgs[i];
        pg.sending = false;
        if !pg.alive {
            return;
        }
        if res <= 0 {
            return self.pg_dead(i, std::io::Error::from_raw_os_error(-res).to_string());
        }
        pg.soff += res as usize;
        if pg.soff < pg.sbuf.len() {
            pg.sending = true;
            let e = opcode::Send::new(Fd(pg.fd), unsafe { pg.sbuf.as_ptr().add(pg.soff) }, (pg.sbuf.len() - pg.soff) as u32).flags(libc::MSG_NOSIGNAL).build();
            return self.push(&[e.user_data(T_PGSEND | (i as u64) << 8)]);
        }
        pg.sbuf.clear();
    }

    /// Fails the connection's requests in flight (a write is never retried: its commit is uncertain) and lets
    /// maintain() reconnect it.
    fn pg_dead(&mut self, i: usize, why: String) {
        eprintln!("postgres connection lost: {why}");
        let pg = &mut self.pgs[i];
        pg.alive = false;
        unsafe { libc::shutdown(pg.fd, libc::SHUT_RDWR) }; // ends the receive and send in flight
        pg.out.clear();
        pg.open = false;
        pg.inb.clear();
        pg.rpos = 0;
        pg.retry = Instant::now() + Duration::from_secs(1);
        let q = std::mem::take(&mut pg.q);
        for p in q {
            if matches!(p.k, PK::Health) {
                self.body.clear();
                self.body.extend_from_slice(NO_DB);
                self.respond(p.who, 503, p.gzip, p.close);
            } else {
                self.reply(p.who, 500, "internal server error", p.gzip, p.close);
            }
        }
    }

    /// Closes dead Postgres connections once their operations are back, and reconnects them one per pass. The
    /// reconnect blocks the event loop (1 s connect and 10 s read timeouts), so after a failed one the thread waits a
    /// second before the next: while Postgres is unreachable, the thread still serves its connections between tries.
    fn maintain(&mut self) {
        let now = Instant::now();
        let mut tried = now < self.reconnect;
        for i in 0..self.pgs.len() {
            let pg = &mut self.pgs[i];
            if pg.alive {
                continue;
            }
            if pg.fd >= 0 && !pg.sending && !pg.receiving {
                unsafe { libc::close(pg.fd) };
                pg.fd = -1;
                // The kernel is done with the send buffer now: drop its bytes, or the next flush would replay
                // statements already answered with 500 (and maybe committed) on the new connection.
                pg.sbuf.clear();
                pg.soff = 0;
            }
            if pg.fd >= 0 || now < pg.retry || tried {
                continue;
            }
            tried = true;
            match pg::connect(&self.cfg.url, &STMTS) {
                Ok(fd) => {
                    pg.fd = fd;
                    pg.alive = true;
                    self.pg_arm(i);
                }
                Err(e) => {
                    eprintln!("postgres reconnect: {e}");
                    pg.retry = Instant::now() + Duration::from_secs(1);
                    self.reconnect = pg.retry;
                }
            }
        }
    }

    fn on_cqe(&mut self, ud: u64, res: i32, f: u32) {
        match ud & 7 {
            T_SEND => self.send_done(unsafe { Box::from_raw(ud as *mut Sop) }, res),
            T_ACCEPT => {
                if res >= 0 {
                    let slot = res as usize;
                    if slot >= self.conns.len() {
                        self.conns.resize_with(slot + 1, Conn::default);
                    }
                    let c = &mut self.conns[slot];
                    c.open = true;
                    let who = T_RECV | (slot as u64) << 8 | (c.g as u64) << 32;
                    self.arm_recv(who);
                } else if res != -libc::ECANCELED {
                    eprintln!("accept: {}", std::io::Error::from_raw_os_error(-res));
                }
                if !cqueue::more(f) {
                    self.rearm_accept.push((ud >> 3) as usize);
                }
            }
            T_RECV => {
                let alive = self.conn(ud).is_some();
                if let Some(bid) = cqueue::buffer_select(f) {
                    if alive && res > 0 {
                        // the kernel does not touch this buffer again until buf_add returns it
                        let p = unsafe { std::slice::from_raw_parts(self.bufs.as_ptr().add(bid as usize * BSZ), res as usize) };
                        self.on_data(ud, p);
                    }
                    self.buf_add(bid);
                }
                if cqueue::more(f) {
                    return;
                }
                let Some(c) = self.conn(ud) else { return };
                if res > 0 || res == -libc::ENOBUFS {
                    self.rearm.push(ud); // re-armed after this batch, once buffers are back
                } else if c.st != St::Idle {
                    c.closing = true; // EOF: finish the reply in flight first
                } else {
                    self.close(ud);
                }
            }
            T_PGRECV => self.pg_recv((ud >> 8) as usize, res),
            T_PGSEND => self.pg_sent((ud >> 8) as usize, res),
            _ => {}
        }
    }

    fn run(&mut self) -> ! {
        let mut cqes = Vec::with_capacity(4096);
        let mut seen = 0;
        loop {
            for who in std::mem::take(&mut self.rearm) {
                if self.conn(who).is_some() {
                    self.arm_recv(who);
                }
            }
            for i in std::mem::take(&mut self.rearm_accept) {
                self.arm_accept(i);
            }
            self.pg_flush();
            self.maintain();
            // With work in the last pass, wait for 128 completions or 2 ms: the sockets that become ready meanwhile
            // are served by one wake-up, and their queries share a Postgres round trip. Idle, wait up to 1 s.
            let ts = if seen > 0 { Timespec::new().nsec(WAIT_NS) } else { Timespec::new().sec(1) };
            let want = if seen > 0 { WAIT_N } else { 1 };
            if let Err(e) = self.ring.submitter().submit_with_args(want, &SubmitArgs::new().timespec(&ts)) {
                if !matches!(e.raw_os_error(), Some(libc::ETIME | libc::EINTR | libc::EBUSY | libc::EAGAIN | libc::ENOMEM | libc::EBADR)) {
                    die("io_uring_enter", e);
                }
            }
            cqes.extend(self.ring.completion().map(|e| (e.user_data(), e.result(), e.flags())));
            seen = cqes.len();
            for &(ud, res, f) in &cqes {
                self.on_cqe(ud, res, f);
            }
            cqes.clear();
        }
    }
}
