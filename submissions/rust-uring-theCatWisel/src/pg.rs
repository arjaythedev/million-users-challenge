use std::collections::VecDeque;
use std::ffi::c_void;

use crate::crypto::{
    base64_std_decode, base64_std_encode, hmac_sha256_once, pbkdf2_hmac_sha256, sha256_once,
};
use crate::http::{
    finalize_json_reply, ReplyPayload, HDR_HEADROOM, RESP_404_POST_NOT_FOUND, RESP_500_INTERNAL,
};
use crate::json::{
    write_i64, write_i64_ptr, write_json_escaped, write_json_escaped_ptr, write_pg_timestamptz,
    write_pg_timestamptz_ptr, write_u64, DayPrefixCache,
};
use crate::sys::{
    close, connect, errno, freeaddrinfo, getaddrinfo, getrandom, getsockopt, recv, send,
    set_sockopt_int, socket, AddrInfo, IgzipCtx, SockAddrIn, AF_INET, EAGAIN, EINPROGRESS, EINTR,
    IPPROTO_TCP, MSG_NOSIGNAL, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_STREAM, SOL_SOCKET, SO_ERROR,
    SO_KEEPALIVE, TCP_KEEPCNT, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_NODELAY, TCP_USER_TIMEOUT,
};

const SQL_FEED: &[u8] = b"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20\0";
const SQL_GET_POST: &[u8] = b"SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1\0";
const SQL_CREATE_POST: &[u8] =
    b"INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at\0";
const SQL_LIKE_POST: &[u8] =
    b"INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING\0";
const SQL_HEALTH: &[u8] = b"SELECT 1\0";

const FEED_BIND_EXEC: [u8; 25] = [
    b'B', 0, 0, 0, 14, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0,
];

const HEALTH_BIND_EXEC: [u8; 24] = [
    b'B', 0, 0, 0, 13, 0, b'h', 0, 0, 0, 0, 0, 0, 0, b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0,
];

const SYNC_MSG: [u8; 5] = [b'S', 0, 0, 0, 4];

const RESP_404_POST_NOT_FOUND_CLOSE: &[u8] = b"HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: 26\r\n\r\n{\"error\":\"post not found\"}";
const RESP_500_INTERNAL_CLOSE: &[u8] = b"HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";
pub const RESP_503_DB_UNREACHABLE: &[u8] = b"HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: 62\r\n\r\n{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"unreachable\"}";
pub const RESP_503_DB_UNREACHABLE_CLOSE: &[u8] = b"HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: 62\r\n\r\n{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"unreachable\"}";

#[derive(Clone)]
pub struct DbConfig {
    pub addr: SockAddrIn,
    pub user: String,
    pub password: String,
    pub dbname: String,
}

fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            let hi = (b[i + 1] as char).to_digit(16);
            let lo = (b[i + 2] as char).to_digit(16);
            if let (Some(h), Some(l)) = (hi, lo) {
                out.push(((h << 4) | l) as u8);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8(out).unwrap_or_else(|_| s.to_string())
}

pub fn parse_database_url(url: &str) -> Result<DbConfig, String> {
    let rest = url
        .strip_prefix("postgres://")
        .or_else(|| url.strip_prefix("postgresql://"))
        .ok_or_else(|| "DATABASE_URL must start with postgres:// or postgresql://".to_string())?;

    let (without_q, _) = match rest.find('?') {
        Some(q) => (&rest[..q], &rest[q + 1..]),
        None => (rest, ""),
    };

    let (authority, path_db) = match without_q.find('/') {
        Some(sl) => (&without_q[..sl], &without_q[sl + 1..]),
        None => (without_q, "postgres"),
    };

    let (userinfo, hostport) = match authority.rfind('@') {
        Some(at) => (&authority[..at], &authority[at + 1..]),
        None => ("postgres:", authority),
    };

    let (raw_user, raw_pass) = match userinfo.find(':') {
        Some(c) => (&userinfo[..c], &userinfo[c + 1..]),
        None => (userinfo, ""),
    };

    let (host, port_str) = match hostport.rfind(':') {
        Some(c) => (&hostport[..c], &hostport[c + 1..]),
        None => (hostport, "5432"),
    };

    let addr = resolve_ipv4(host, port_str)?;
    Ok(DbConfig {
        addr,
        user: pct_decode(raw_user),
        password: pct_decode(raw_pass),
        dbname: pct_decode(if path_db.is_empty() { "postgres" } else { path_db }),
    })
}

pub fn resolve_ipv4(host: &str, port: &str) -> Result<SockAddrIn, String> {
    let mut node = Vec::with_capacity(host.len() + 1);
    node.extend_from_slice(host.as_bytes());
    node.push(0);

    let mut serv = Vec::with_capacity(port.len() + 1);
    serv.extend_from_slice(port.as_bytes());
    serv.push(0);

    let hints = AddrInfo {
        ai_flags: 0,
        ai_family: AF_INET,
        ai_socktype: SOCK_STREAM,
        ai_protocol: IPPROTO_TCP,
        ai_addrlen: 0,
        ai_addr: std::ptr::null_mut(),
        ai_canonname: std::ptr::null_mut(),
        ai_next: std::ptr::null_mut(),
    };

    let mut res: *mut AddrInfo = std::ptr::null_mut();
    let rc = unsafe { getaddrinfo(node.as_ptr(), serv.as_ptr(), &hints, &mut res) };
    if rc != 0 || res.is_null() {
        return Err(format!("failed to resolve {}:{} (rc={})", host, port, rc));
    }
    let addr = unsafe { *(*res).ai_addr };
    unsafe {
        freeaddrinfo(res);
    }
    Ok(addr)
}

#[derive(Clone)]
pub enum InlineBytes<const N: usize> {
    Inline { len: u16, buf: [u8; N] },
    Heap(Box<[u8]>),
}

impl<const N: usize> InlineBytes<N> {
    #[inline(always)]
    pub fn from_slice(s: &[u8]) -> Self {
        if s.len() <= N {
            let mut buf = [0u8; N];
            buf[..s.len()].copy_from_slice(s);
            Self::Inline {
                len: s.len() as u16,
                buf,
            }
        } else {
            Self::Heap(s.into())
        }
    }

    #[inline(always)]
    pub fn as_slice(&self) -> &[u8] {
        match self {
            Self::Inline { len, buf } => &buf[..*len as usize],
            Self::Heap(b) => b,
        }
    }
}

#[derive(Clone, Copy)]
pub enum ReadOp {
    Feed,
    GetPost { post_id: i64 },
    Health,
}

#[derive(Clone, Copy)]
pub struct ReadReq {
    pub client_fd: i32,
    pub client_gen: u16,
    pub close_after: bool,
    pub accept_gzip: bool,
    pub retries: u8,
    pub enqueued_ms: u32,
    pub enqueued_ns: u64,
    pub op: ReadOp,
}

#[derive(Clone)]
pub enum WriteOp {
    CreatePost {
        user_id: i64,
        username: InlineBytes<64>,
        body: InlineBytes<256>,
    },
    LikePost {
        user_id: i64,
        post_id: i64,
    },
}

#[derive(Clone)]
pub struct WriteReq {
    pub client_fd: i32,
    pub client_gen: u16,
    pub close_after: bool,
    pub accept_gzip: bool,
    pub single_step: bool,
    pub enqueued_ms: u32,
    pub enqueued_ns: u64,
    pub op: WriteOp,
}

#[derive(Clone, Copy)]
pub enum WriteOutcome {
    Pending,
    CreatedPost {
        post_id: i64,
        created_pg_us: i64,
    },
    Liked {
        first_time: bool,
    },
    NotFound404,
    Internal500,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PgPhase {
    Disconnected,
    Connecting,
    Authenticating,
    Preparing,
    Ready,
}

pub struct PgLane {
    pub is_write: bool,
    pub fd: i32,
    pub conn_gen: u16,
    pub phase: PgPhase,
    pub epoll_events: u32,
    pub last_activity_ns: u64,
    pub next_reconnect_ns: u64,
    pub batch_sent_ns: u64,
    pub ewma_rtt_ns: u64,
    pub lifo_counter: u32,

    scram_nonce: String,
    scram_client_first_bare: String,

    pub tx_buf: Vec<u8>,
    pub tx_pos: usize,
    pub rx_buf: Vec<u8>,
    pub rx_pos: usize,
    pub uring_rx_armed: bool,
    pub uring_tx_inflight: bool,

    pub read_in_flight: Vec<ReadReq>,
    pub read_cursor: usize,
    pub cur_row_count: usize,
    pub json_scratch: Vec<u8>,
    pub zbuf_scratch: Vec<u8>,
    pub day_cache: DayPrefixCache,

    pub write_in_flight: Vec<(WriteReq, WriteOutcome)>,
    pub write_cursor: usize,
    pub write_batch_aborted_at: Option<usize>,
}

impl PgLane {
    pub fn new(is_write: bool) -> Self {
        Self {
            is_write,
            fd: -1,
            conn_gen: 0,
            phase: PgPhase::Disconnected,
            epoll_events: 0,
            last_activity_ns: 0,
            next_reconnect_ns: 0,
            batch_sent_ns: 0,
            ewma_rtt_ns: 0,
            lifo_counter: 0,
            scram_nonce: String::new(),
            scram_client_first_bare: String::new(),
            tx_buf: Vec::with_capacity(if is_write { 32768 } else { 8192 }),
            tx_pos: 0,
            rx_buf: Vec::with_capacity(if is_write { 16384 } else { 65536 }),
            rx_pos: 0,
            uring_rx_armed: false,
            uring_tx_inflight: false,
            read_in_flight: Vec::with_capacity(if is_write { 0 } else { 64 }),
            read_cursor: 0,
            cur_row_count: 0,
            json_scratch: vec![0u8; 8192],
            zbuf_scratch: vec![0u8; 8192],
            day_cache: DayPrefixCache::new(),
            write_in_flight: Vec::with_capacity(if is_write { 64 } else { 0 }),
            write_cursor: 0,
            write_batch_aborted_at: None,
        }
    }

    #[inline(always)]
    pub fn has_pending_tx(&self) -> bool {
        self.tx_pos < self.tx_buf.len()
    }

    #[inline(always)]
    pub fn is_idle_ready(&self) -> bool {
        self.phase == PgPhase::Ready
            && !self.has_pending_tx()
            && !self.uring_tx_inflight
            && self.read_in_flight.is_empty()
            && self.write_in_flight.is_empty()
    }

    #[inline(always)]
    pub fn is_busy(&self) -> bool {
        !self.read_in_flight.is_empty()
            || !self.write_in_flight.is_empty()
            || self.has_pending_tx()
            || self.uring_tx_inflight
    }

    pub fn start_connect(&mut self, cfg: &DbConfig, now_ns: u64, defer_send_to_uring: bool) -> bool {
        if self.fd >= 0 {
            unsafe {
                close(self.fd);
            }
            self.fd = -1;
        }
        self.conn_gen = self.conn_gen.wrapping_add(1);
        if self.conn_gen == 0 {
            self.conn_gen = 1;
        }
        self.tx_buf.clear();
        self.tx_pos = 0;
        self.rx_buf.clear();
        self.rx_pos = 0;
        self.epoll_events = 0;
        self.uring_rx_armed = false;
        self.uring_tx_inflight = false;

        let fd =
            unsafe { socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP) };
        if fd < 0 {
            self.phase = PgPhase::Disconnected;
            self.next_reconnect_ns = now_ns + 100_000_000;
            return false;
        }

        set_sockopt_int(fd, IPPROTO_TCP, TCP_NODELAY, 1);
        set_sockopt_int(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
        set_sockopt_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, 15);
        set_sockopt_int(fd, IPPROTO_TCP, TCP_KEEPINTVL, 5);
        set_sockopt_int(fd, IPPROTO_TCP, TCP_KEEPCNT, 3);
        set_sockopt_int(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, 10_000);

        let rc = unsafe {
            connect(
                fd,
                &cfg.addr as *const SockAddrIn,
                std::mem::size_of::<SockAddrIn>() as u32,
            )
        };
        if rc < 0 && errno() != EINPROGRESS {
            unsafe {
                close(fd);
            }
            self.phase = PgPhase::Disconnected;
            self.next_reconnect_ns = now_ns + 100_000_000;
            return false;
        }

        self.fd = fd;
        self.last_activity_ns = now_ns;
        if rc == 0 {
            self.phase = PgPhase::Authenticating;
            self.queue_startup_message(cfg);
            if !defer_send_to_uring {
                let _ = self.flush_tx(now_ns);
            }
        } else {
            self.phase = PgPhase::Connecting;
        }
        true
    }

    pub fn complete_connect<F>(
        &mut self,
        cfg: &DbConfig,
        now_ns: u64,
        defer_send_to_uring: bool,
        retry_reads_out: &mut VecDeque<ReadReq>,
        on_reply: &mut F,
    ) where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        let mut err: i32 = 0;
        let mut len = std::mem::size_of::<i32>() as u32;
        let rc = unsafe {
            getsockopt(
                self.fd,
                SOL_SOCKET,
                SO_ERROR,
                &mut err as *mut i32 as *mut c_void,
                &mut len,
            )
        };
        if rc < 0 || err != 0 {
            self.fail_and_reset(now_ns, retry_reads_out, on_reply);
            return;
        }
        self.phase = PgPhase::Authenticating;
        self.last_activity_ns = now_ns;
        self.queue_startup_message(cfg);
        if !defer_send_to_uring && self.flush_tx(now_ns).is_err() {
            self.fail_and_reset(now_ns, retry_reads_out, on_reply);
        }
    }

    fn queue_startup_message(&mut self, cfg: &DbConfig) {
        let start = self.tx_buf.len();
        self.tx_buf.extend_from_slice(&[0, 0, 0, 0]);
        self.tx_buf.extend_from_slice(&196608u32.to_be_bytes());
        let kv: [(&[u8], &[u8]); 10] = [
            (b"user", cfg.user.as_bytes()),
            (b"database", cfg.dbname.as_bytes()),
            (b"application_name", b"feed"),
            (b"TimeZone", b"UTC"),
            (b"client_encoding", b"UTF8"),
            (b"synchronous_commit", b"on"),
            (b"plan_cache_mode", b"force_generic_plan"),
            (b"max_parallel_workers_per_gather", b"0"),
            (b"jit", b"off"),
            (b"client_connection_check_interval", b"0"),
        ];
        for (k, v) in kv {
            self.tx_buf.extend_from_slice(k);
            self.tx_buf.push(0);
            self.tx_buf.extend_from_slice(v);
            self.tx_buf.push(0);
        }
        self.tx_buf.push(0);
        let len = (self.tx_buf.len() - start) as u32;
        self.tx_buf[start..start + 4].copy_from_slice(&len.to_be_bytes());
    }

    fn queue_prepare_all(&mut self) {
        append_parse(&mut self.tx_buf, b"\0", SQL_FEED, &[]);
        append_parse(&mut self.tx_buf, b"g\0", SQL_GET_POST, &[20]);
        append_parse(&mut self.tx_buf, b"c\0", SQL_CREATE_POST, &[20, 25]);
        append_parse(&mut self.tx_buf, b"l\0", SQL_LIKE_POST, &[20, 20]);
        append_parse(&mut self.tx_buf, b"h\0", SQL_HEALTH, &[]);
        self.tx_buf.extend_from_slice(&SYNC_MSG);
    }

    pub fn flush_tx(&mut self, now_ns: u64) -> Result<(), ()> {
        while self.tx_pos < self.tx_buf.len() {
            let rem = &self.tx_buf[self.tx_pos..];
            let n = unsafe {
                send(
                    self.fd,
                    rem.as_ptr() as *const c_void,
                    rem.len(),
                    MSG_NOSIGNAL,
                )
            };
            if n > 0 {
                self.tx_pos += n as usize;
                self.last_activity_ns = now_ns;
            } else if n < 0 {
                let e = errno();
                if e == EINTR {
                    continue;
                }
                if e == EAGAIN {
                    break;
                }
                return Err(());
            } else {
                return Err(());
            }
        }
        if self.tx_pos == self.tx_buf.len() {
            self.tx_buf.clear();
            self.tx_pos = 0;
        }
        Ok(())
    }

    pub fn send_read_batch(
        &mut self,
        queue: &mut VecDeque<ReadReq>,
        max_take: usize,
        now_ns: u64,
        now_ms: u32,
        lifo_age_ms: u32,
        defer_send_to_uring: bool,
    ) -> Result<usize, ()> {
        let take = queue.len().min(max_take);
        if take == 0 {
            return Ok(0);
        }
        self.read_in_flight.clear();
        self.read_cursor = 0;
        self.cur_row_count = 0;
        self.batch_sent_ns = now_ns;

        let use_lifo = if lifo_age_ms > 0 {
            if let Some(front) = queue.front() {
                now_ms.wrapping_sub(front.enqueued_ms) >= lifo_age_ms
            } else {
                false
            }
        } else {
            false
        };

        for _ in 0..take {
            let req = if use_lifo {
                let take_oldest = (self.lifo_counter & 7) == 0;
                self.lifo_counter = self.lifo_counter.wrapping_add(1);
                if take_oldest {
                    queue.pop_front().unwrap()
                } else {
                    queue.pop_back().unwrap()
                }
            } else {
                queue.pop_front().unwrap()
            };
            match req.op {
                ReadOp::Feed => self.tx_buf.extend_from_slice(&FEED_BIND_EXEC),
                ReadOp::GetPost { post_id } => append_get_post_bind_exec(&mut self.tx_buf, post_id),
                ReadOp::Health => self.tx_buf.extend_from_slice(&HEALTH_BIND_EXEC),
            }
            self.read_in_flight.push(req);
        }
        if let Some(&first) = self.read_in_flight.first() {
            self.prepare_read_scratch(first);
        }
        self.tx_buf.extend_from_slice(&SYNC_MSG);
        if !defer_send_to_uring {
            self.flush_tx(now_ns)?;
        }
        Ok(take)
    }

    #[inline(always)]
    fn prepare_read_scratch(&mut self, req: ReadReq) {
        self.cur_row_count = 0;
        self.day_cache = DayPrefixCache::new();
        if self.json_scratch.len() >= HDR_HEADROOM {
            self.json_scratch.truncate(HDR_HEADROOM);
        } else {
            self.json_scratch.resize(HDR_HEADROOM, 0);
        }
        match req.op {
            ReadOp::Feed => self.json_scratch.extend_from_slice(b"{\"posts\":["),
            ReadOp::GetPost { .. } => self.json_scratch.extend_from_slice(b"{\"post\":"),
            ReadOp::Health => {}
        }
    }

    pub fn send_write_batch(
        &mut self,
        queue: &mut VecDeque<WriteReq>,
        max_take: usize,
        now_ns: u64,
        defer_send_to_uring: bool,
    ) -> Result<usize, ()> {
        if queue.is_empty() {
            return Ok(0);
        }
        self.write_in_flight.clear();
        self.write_cursor = 0;
        self.write_batch_aborted_at = None;
        self.batch_sent_ns = now_ns;

        let first_single = queue.front().map(|r| r.single_step).unwrap_or(false);
        let limit = if first_single { 1 } else { max_take };
        let mut count = 0usize;

        while count < limit {
            let Some(front) = queue.front() else {
                break;
            };
            if count > 0 && front.single_step {
                break;
            }
            let req = queue.pop_front().unwrap();
            match &req.op {
                WriteOp::CreatePost { user_id, body, .. } => {
                    append_create_post_bind_exec(&mut self.tx_buf, *user_id, body.as_slice());
                }
                WriteOp::LikePost { user_id, post_id } => {
                    append_like_post_bind_exec(&mut self.tx_buf, *user_id, *post_id);
                }
            }
            self.write_in_flight.push((req, WriteOutcome::Pending));
            count += 1;
        }
        self.tx_buf.extend_from_slice(&SYNC_MSG);
        if !defer_send_to_uring {
            self.flush_tx(now_ns)?;
        }
        Ok(count)
    }

    #[inline(always)]
    pub fn advance_uring_tx(&mut self, sent: usize, now_ns: u64) -> (*const u8, usize) {
        self.tx_pos += sent;
        self.last_activity_ns = now_ns;
        if self.tx_pos < self.tx_buf.len() {
            let rem = &self.tx_buf[self.tx_pos..];
            (rem.as_ptr(), rem.len())
        } else {
            self.tx_buf.clear();
            self.tx_pos = 0;
            self.uring_tx_inflight = false;
            (std::ptr::null(), 0)
        }
    }

    #[inline(always)]
    pub fn pending_tx_slice(&self) -> (*const u8, usize) {
        if self.tx_pos < self.tx_buf.len() {
            let rem = &self.tx_buf[self.tx_pos..];
            (rem.as_ptr(), rem.len())
        } else {
            (std::ptr::null(), 0)
        }
    }

    #[inline(always)]
    fn record_rtt(&mut self, now_ns: u64) {
        if self.batch_sent_ns > 0 && now_ns > self.batch_sent_ns {
            let rtt = now_ns - self.batch_sent_ns;
            if self.ewma_rtt_ns == 0 {
                self.ewma_rtt_ns = rtt;
            } else {
                self.ewma_rtt_ns = self.ewma_rtt_ns - (self.ewma_rtt_ns >> 3) + (rtt >> 3);
            }
        }
    }

    pub fn handle_io<F>(
        &mut self,
        can_read: bool,
        can_write: bool,
        cfg: &DbConfig,
        now_ns: u64,
        uptime_s: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        retry_reads_out: &mut VecDeque<ReadReq>,
        retry_writes_out: &mut VecDeque<WriteReq>,
        on_reply: &mut F,
    ) where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        if self.phase == PgPhase::Connecting {
            let mut err: i32 = 0;
            let mut len = std::mem::size_of::<i32>() as u32;
            let rc = unsafe {
                getsockopt(
                    self.fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &mut err as *mut i32 as *mut c_void,
                    &mut len,
                )
            };
            if rc < 0 || err != 0 {
                self.fail_and_reset(now_ns, retry_reads_out, on_reply);
                return;
            }
            self.phase = PgPhase::Authenticating;
            self.queue_startup_message(cfg);
            if self.flush_tx(now_ns).is_err() {
                self.fail_and_reset(now_ns, retry_reads_out, on_reply);
                return;
            }
        } else if can_write && self.has_pending_tx() && self.flush_tx(now_ns).is_err() {
            self.fail_and_reset(now_ns, retry_reads_out, on_reply);
            return;
        }

        if !can_read && self.rx_pos == self.rx_buf.len() {
            return;
        }

        loop {
            let avail_space = self.rx_buf.capacity().saturating_sub(self.rx_buf.len());
            if avail_space < 16384 {
                if self.rx_pos > 0 {
                    let unread = self.rx_buf.len() - self.rx_pos;
                    self.rx_buf.copy_within(self.rx_pos.., 0);
                    self.rx_buf.truncate(unread);
                    self.rx_pos = 0;
                }
                if self.rx_buf.capacity() - self.rx_buf.len() < 16384 {
                    self.rx_buf.reserve(32768);
                }
            }

            let cur_len = self.rx_buf.len();
            let cap = self.rx_buf.capacity();
            let n = unsafe {
                recv(
                    self.fd,
                    self.rx_buf.as_mut_ptr().add(cur_len) as *mut c_void,
                    cap - cur_len,
                    0,
                )
            };
            if n > 0 {
                unsafe {
                    self.rx_buf.set_len(cur_len + n as usize);
                }
                self.last_activity_ns = now_ns;
            } else if n == 0 {
                self.fail_and_reset(now_ns, retry_reads_out, on_reply);
                return;
            } else {
                let e = errno();
                if e == EINTR {
                    continue;
                }
                if e == EAGAIN {
                    break;
                }
                self.fail_and_reset(now_ns, retry_reads_out, on_reply);
                return;
            }
        }

        let now_ms = now_ns / 1_000_000;
        if self
            .process_frames(
                cfg,
                now_ns,
                now_ms,
                uptime_s,
                gz_min_bytes,
                igzip,
                retry_writes_out,
                on_reply,
            )
            .is_err()
        {
            self.fail_and_reset(now_ns, retry_reads_out, on_reply);
        }
    }

    #[inline(always)]
    fn dispatch_one_frame<F>(
        &mut self,
        tag: u8,
        payload: &[u8],
        cfg: &DbConfig,
        now_ns: u64,
        now_ms: u64,
        uptime_s: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        defer_send_to_uring: bool,
        retry_writes_out: &mut VecDeque<WriteReq>,
        on_reply: &mut F,
    ) -> Result<(), ()>
    where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        match self.phase {
            PgPhase::Authenticating => match tag {
                b'R' => {
                    if payload.len() < 4 {
                        return Err(());
                    }
                    let code = u32::from_be_bytes([payload[0], payload[1], payload[2], payload[3]]);
                    self.handle_auth_msg(code, &payload[4..], cfg, now_ns, defer_send_to_uring)
                }
                b'Z' => {
                    self.phase = PgPhase::Preparing;
                    self.queue_prepare_all();
                    if !defer_send_to_uring {
                        self.flush_tx(now_ns)?;
                    }
                    Ok(())
                }
                b'E' => Err(()),
                _ => Ok(()),
            },
            PgPhase::Preparing => match tag {
                b'Z' => {
                    self.phase = PgPhase::Ready;
                    Ok(())
                }
                b'E' => Err(()),
                _ => Ok(()),
            },
            PgPhase::Ready => {
                if !self.is_write {
                    self.handle_read_payload(
                        tag,
                        payload,
                        now_ns,
                        now_ms,
                        uptime_s,
                        gz_min_bytes,
                        igzip,
                        on_reply,
                    )
                } else {
                    self.handle_write_payload(
                        tag,
                        payload,
                        now_ns,
                        now_ms,
                        gz_min_bytes,
                        igzip,
                        retry_writes_out,
                        on_reply,
                    )
                }
            }
            _ => Err(()),
        }
    }

    pub fn ingest_uring_rx_slice<F>(
        &mut self,
        pkt: &[u8],
        cfg: &DbConfig,
        now_ns: u64,
        uptime_s: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        retry_reads_out: &mut VecDeque<ReadReq>,
        retry_writes_out: &mut VecDeque<WriteReq>,
        on_reply: &mut F,
    ) where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        self.last_activity_ns = now_ns;
        let now_ms = now_ns / 1_000_000;
        if self
            .ingest_uring_rx_inner(
                pkt,
                cfg,
                now_ns,
                now_ms,
                uptime_s,
                gz_min_bytes,
                igzip,
                retry_writes_out,
                on_reply,
            )
            .is_err()
        {
            self.fail_and_reset(now_ns, retry_reads_out, on_reply);
        }
    }

    fn ingest_uring_rx_inner<F>(
        &mut self,
        mut pkt: &[u8],
        cfg: &DbConfig,
        now_ns: u64,
        now_ms: u64,
        uptime_s: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        retry_writes_out: &mut VecDeque<WriteReq>,
        on_reply: &mut F,
    ) -> Result<(), ()>
    where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        if self.rx_pos == self.rx_buf.len() && !self.rx_buf.is_empty() {
            self.rx_buf.clear();
            self.rx_pos = 0;
        }

        if self.rx_pos < self.rx_buf.len() {
            let have_hdr = self.rx_buf.len() - self.rx_pos;
            if have_hdr < 5 {
                let take_h = (5 - have_hdr).min(pkt.len());
                self.rx_buf.extend_from_slice(&pkt[..take_h]);
                pkt = &pkt[take_h..];
                if self.rx_buf.len() - self.rx_pos < 5 {
                    return Ok(());
                }
            }
            let p0 = self.rx_pos;
            let tag = self.rx_buf[p0];
            let mlen = u32::from_be_bytes([
                self.rx_buf[p0 + 1],
                self.rx_buf[p0 + 2],
                self.rx_buf[p0 + 3],
                self.rx_buf[p0 + 4],
            ]) as usize;
            if mlen < 4 {
                return Err(());
            }
            let needed_total = 1 + mlen;
            let cur_have = self.rx_buf.len() - p0;
            if cur_have + pkt.len() < needed_total {
                self.rx_buf.extend_from_slice(pkt);
                return Ok(());
            }
            let need_more = needed_total.saturating_sub(cur_have);
            if need_more > 0 {
                self.rx_buf.extend_from_slice(&pkt[..need_more]);
                pkt = &pkt[need_more..];
            }
            let mut held = std::mem::take(&mut self.rx_buf);
            let res = self.dispatch_one_frame(
                tag,
                &held[p0 + 5..p0 + needed_total],
                cfg,
                now_ns,
                now_ms,
                uptime_s,
                gz_min_bytes,
                igzip,
                true,
                retry_writes_out,
                on_reply,
            );
            let next_pos = p0 + needed_total;
            if next_pos < held.len() {
                held.copy_within(next_pos.., 0);
                held.truncate(held.len() - next_pos);
            } else {
                held.clear();
            }
            self.rx_buf = held;
            self.rx_pos = 0;
            res?;

            if !self.rx_buf.is_empty() {
                self.rx_buf.extend_from_slice(pkt);
                return self.drain_buffered_frames(
                    cfg,
                    now_ns,
                    now_ms,
                    uptime_s,
                    gz_min_bytes,
                    igzip,
                    true,
                    retry_writes_out,
                    on_reply,
                );
            }
        }

        let mut p = 0usize;
        while p + 5 <= pkt.len() {
            let tag = pkt[p];
            let mlen =
                u32::from_be_bytes([pkt[p + 1], pkt[p + 2], pkt[p + 3], pkt[p + 4]]) as usize;
            if mlen < 4 {
                return Err(());
            }
            let frame_end = p + 1 + mlen;
            if frame_end > pkt.len() {
                break;
            }
            self.dispatch_one_frame(
                tag,
                &pkt[p + 5..frame_end],
                cfg,
                now_ns,
                now_ms,
                uptime_s,
                gz_min_bytes,
                igzip,
                true,
                retry_writes_out,
                on_reply,
            )?;
            p = frame_end;
        }
        if p < pkt.len() {
            self.rx_buf.extend_from_slice(&pkt[p..]);
            self.rx_pos = 0;
        }
        Ok(())
    }

    fn drain_buffered_frames<F>(
        &mut self,
        cfg: &DbConfig,
        now_ns: u64,
        now_ms: u64,
        uptime_s: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        defer_send_to_uring: bool,
        retry_writes_out: &mut VecDeque<WriteReq>,
        on_reply: &mut F,
    ) -> Result<(), ()>
    where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        let mut held = std::mem::take(&mut self.rx_buf);
        let mut pos = self.rx_pos;
        let mut res = Ok(());
        while pos + 5 <= held.len() {
            let tag = held[pos];
            let mlen =
                u32::from_be_bytes([held[pos + 1], held[pos + 2], held[pos + 3], held[pos + 4]])
                    as usize;
            if mlen < 4 {
                res = Err(());
                break;
            }
            let frame_end = pos + 1 + mlen;
            if frame_end > held.len() {
                break;
            }
            res = self.dispatch_one_frame(
                tag,
                &held[pos + 5..frame_end],
                cfg,
                now_ns,
                now_ms,
                uptime_s,
                gz_min_bytes,
                igzip,
                defer_send_to_uring,
                retry_writes_out,
                on_reply,
            );
            pos = frame_end;
            if res.is_err() {
                break;
            }
        }
        if pos == held.len() {
            held.clear();
            pos = 0;
        }
        self.rx_buf = held;
        self.rx_pos = pos;
        res
    }

    fn process_frames<F>(
        &mut self,
        cfg: &DbConfig,
        now_ns: u64,
        now_ms: u64,
        uptime_s: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        retry_writes_out: &mut VecDeque<WriteReq>,
        on_reply: &mut F,
    ) -> Result<(), ()>
    where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        self.drain_buffered_frames(
            cfg,
            now_ns,
            now_ms,
            uptime_s,
            gz_min_bytes,
            igzip,
            false,
            retry_writes_out,
            on_reply,
        )
    }

    fn handle_auth_msg(
        &mut self,
        code: u32,
        body: &[u8],
        cfg: &DbConfig,
        now_ns: u64,
        defer_send_to_uring: bool,
    ) -> Result<(), ()> {
        match code {
            0 | 12 => Ok(()),
            3 => {
                let len = (4 + cfg.password.len() + 1) as u32;
                self.tx_buf.push(b'p');
                self.tx_buf.extend_from_slice(&len.to_be_bytes());
                self.tx_buf.extend_from_slice(cfg.password.as_bytes());
                self.tx_buf.push(0);
                if defer_send_to_uring {
                    Ok(())
                } else {
                    self.flush_tx(now_ns)
                }
            }
            10 => {
                let mut rnd = [0u8; 18];
                unsafe {
                    getrandom(rnd.as_mut_ptr() as *mut c_void, 18, 0);
                }
                let nonce = base64_std_encode(&rnd);
                let client_first_bare = format!("n=,r={}", nonce);
                let gs2_msg = format!("n,,{}", client_first_bare);
                self.scram_nonce = nonce;
                self.scram_client_first_bare = client_first_bare;

                let mech = b"SCRAM-SHA-256\0";
                let len = (4 + mech.len() + 4 + gs2_msg.len()) as u32;
                self.tx_buf.push(b'p');
                self.tx_buf.extend_from_slice(&len.to_be_bytes());
                self.tx_buf.extend_from_slice(mech);
                self.tx_buf
                    .extend_from_slice(&(gs2_msg.len() as u32).to_be_bytes());
                self.tx_buf.extend_from_slice(gs2_msg.as_bytes());
                if defer_send_to_uring {
                    Ok(())
                } else {
                    self.flush_tx(now_ns)
                }
            }
            11 => {
                let server_first = std::str::from_utf8(body).map_err(|_| ())?;
                let mut s_nonce = "";
                let mut s_salt_b64 = "";
                let mut iter: u32 = 0;
                for part in server_first.split(',') {
                    if let Some(r) = part.strip_prefix("r=") {
                        s_nonce = r;
                    } else if let Some(s) = part.strip_prefix("s=") {
                        s_salt_b64 = s;
                    } else if let Some(i) = part.strip_prefix("i=") {
                        iter = i.parse::<u32>().map_err(|_| ())?;
                    }
                }
                if !s_nonce.starts_with(&self.scram_nonce) || iter == 0 {
                    return Err(());
                }
                let salt = base64_std_decode(s_salt_b64.as_bytes()).ok_or(())?;
                let salted_pw = pbkdf2_hmac_sha256(cfg.password.as_bytes(), &salt, iter);
                let client_key = hmac_sha256_once(&salted_pw, b"Client Key");
                let stored_key = sha256_once(&client_key);

                let client_final_no_proof = format!("c=biws,r={}", s_nonce);
                let auth_msg = format!(
                    "{},{},{}",
                    self.scram_client_first_bare, server_first, client_final_no_proof
                );
                let client_sig = hmac_sha256_once(&stored_key, auth_msg.as_bytes());
                let mut proof = [0u8; 32];
                for k in 0..32 {
                    proof[k] = client_key[k] ^ client_sig[k];
                }
                let client_final =
                    format!("{},p={}", client_final_no_proof, base64_std_encode(&proof));
                let len = (4 + client_final.len()) as u32;
                self.tx_buf.push(b'p');
                self.tx_buf.extend_from_slice(&len.to_be_bytes());
                self.tx_buf.extend_from_slice(client_final.as_bytes());
                if defer_send_to_uring {
                    Ok(())
                } else {
                    self.flush_tx(now_ns)
                }
            }
            _ => Err(()),
        }
    }

    fn handle_read_payload<F>(
        &mut self,
        tag: u8,
        payload: &[u8],
        now_ns: u64,
        now_ms: u64,
        uptime_s: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        on_reply: &mut F,
    ) -> Result<(), ()>
    where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        match tag {
            b'2' | b'n' | b'N' | b'S' => Ok(()),
            b'D' => {
                if self.read_cursor >= self.read_in_flight.len() {
                    return Ok(());
                }
                let op = self.read_in_flight[self.read_cursor].op;
                match op {
                    ReadOp::Feed | ReadOp::GetPost { .. } => {
                        let is_feed = matches!(op, ReadOp::Feed);
                        if is_feed && self.cur_row_count > 0 {
                            self.json_scratch.push(b',');
                        }
                        if !is_feed && self.cur_row_count > 0 {
                            return Ok(());
                        }
                        format_post_datarow(
                            payload,
                            &mut self.json_scratch,
                            &mut self.day_cache,
                        )?;
                        self.cur_row_count += 1;
                    }
                    ReadOp::Health => {
                        self.cur_row_count += 1;
                    }
                }
                Ok(())
            }
            b'C' => {
                if self.read_cursor >= self.read_in_flight.len() {
                    return Ok(());
                }
                let req = self.read_in_flight[self.read_cursor];
                match req.op {
                    ReadOp::Feed => {
                        self.json_scratch.extend_from_slice(b"]}");
                        let rep = finalize_json_reply(
                            &mut self.json_scratch,
                            &mut self.zbuf_scratch,
                            200,
                            req.accept_gzip,
                            req.close_after,
                            gz_min_bytes,
                            igzip,
                            now_ms,
                        );
                        on_reply(
                            req.client_fd,
                            req.client_gen,
                            req.close_after,
                            req.enqueued_ms,
                            rep,
                        );
                    }
                    ReadOp::GetPost { .. } => {
                        if self.cur_row_count == 0 {
                            let s = if req.close_after {
                                RESP_404_POST_NOT_FOUND_CLOSE
                            } else {
                                RESP_404_POST_NOT_FOUND
                            };
                            on_reply(
                                req.client_fd,
                                req.client_gen,
                                req.close_after,
                                req.enqueued_ms,
                                ReplyPayload::Static(s),
                            );
                        } else {
                            self.json_scratch.push(b'}');
                            let rep = finalize_json_reply(
                                &mut self.json_scratch,
                                &mut self.zbuf_scratch,
                                200,
                                req.accept_gzip,
                                req.close_after,
                                gz_min_bytes,
                                igzip,
                                now_ms,
                            );
                            on_reply(
                                req.client_fd,
                                req.client_gen,
                                req.close_after,
                                req.enqueued_ms,
                                rep,
                            );
                        }
                    }
                    ReadOp::Health => {
                        if self.json_scratch.len() >= HDR_HEADROOM {
                            self.json_scratch.truncate(HDR_HEADROOM);
                        } else {
                            self.json_scratch.resize(HDR_HEADROOM, 0);
                        }
                        self.json_scratch
                            .extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                        write_u64(&mut self.json_scratch, uptime_s);
                        self.json_scratch.push(b'}');
                        let rep = finalize_json_reply(
                            &mut self.json_scratch,
                            &mut self.zbuf_scratch,
                            200,
                            req.accept_gzip,
                            req.close_after,
                            gz_min_bytes,
                            igzip,
                            now_ms,
                        );
                        on_reply(
                            req.client_fd,
                            req.client_gen,
                            req.close_after,
                            req.enqueued_ms,
                            rep,
                        );
                    }
                }

                self.read_cursor += 1;
                if self.read_cursor < self.read_in_flight.len() {
                    let next = self.read_in_flight[self.read_cursor];
                    self.prepare_read_scratch(next);
                }
                Ok(())
            }
            b'E' => {
                if self.read_cursor < self.read_in_flight.len() {
                    let req = self.read_in_flight[self.read_cursor];
                    match req.op {
                        ReadOp::Health => {
                            let s = if req.close_after {
                                RESP_503_DB_UNREACHABLE_CLOSE
                            } else {
                                RESP_503_DB_UNREACHABLE
                            };
                            on_reply(
                                req.client_fd,
                                req.client_gen,
                                req.close_after,
                                req.enqueued_ms,
                                ReplyPayload::Static(s),
                            );
                        }
                        _ => {
                            let s = if req.close_after {
                                RESP_500_INTERNAL_CLOSE
                            } else {
                                RESP_500_INTERNAL
                            };
                            on_reply(
                                req.client_fd,
                                req.client_gen,
                                req.close_after,
                                req.enqueued_ms,
                                ReplyPayload::Static(s),
                            );
                        }
                    }
                    for &skipped in &self.read_in_flight[self.read_cursor + 1..] {
                        let s = match (skipped.op, skipped.close_after) {
                            (ReadOp::Health, true) => RESP_503_DB_UNREACHABLE_CLOSE,
                            (ReadOp::Health, false) => RESP_503_DB_UNREACHABLE,
                            (_, true) => RESP_500_INTERNAL_CLOSE,
                            (_, false) => RESP_500_INTERNAL,
                        };
                        on_reply(
                            skipped.client_fd,
                            skipped.client_gen,
                            skipped.close_after,
                            skipped.enqueued_ms,
                            ReplyPayload::Static(s),
                        );
                    }
                    self.read_cursor = self.read_in_flight.len();
                }
                Ok(())
            }
            b'Z' => {
                self.record_rtt(now_ns);
                self.read_in_flight.clear();
                self.read_cursor = 0;
                Ok(())
            }
            _ => Ok(()),
        }
    }

    fn handle_write_payload<F>(
        &mut self,
        tag: u8,
        payload: &[u8],
        now_ns: u64,
        now_ms: u64,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        retry_writes_out: &mut VecDeque<WriteReq>,
        on_reply: &mut F,
    ) -> Result<(), ()>
    where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        match tag {
            b'2' | b'n' | b'N' | b'S' => Ok(()),
            b'D' => {
                if self.write_cursor < self.write_in_flight.len() {
                    let (post_id, pg_us) = parse_create_post_datarow(payload)?;
                    self.write_in_flight[self.write_cursor].1 = WriteOutcome::CreatedPost {
                        post_id,
                        created_pg_us: pg_us,
                    };
                }
                Ok(())
            }
            b'C' => {
                if self.write_cursor < self.write_in_flight.len() {
                    let is_like = matches!(
                        self.write_in_flight[self.write_cursor].0.op,
                        WriteOp::LikePost { .. }
                    );
                    if is_like {
                        let first_time = payload.starts_with(b"INSERT 0 1");
                        self.write_in_flight[self.write_cursor].1 =
                            WriteOutcome::Liked { first_time };
                    }
                    self.write_cursor += 1;
                }
                Ok(())
            }
            b'E' => {
                if self.write_cursor < self.write_in_flight.len()
                    && self.write_batch_aborted_at.is_none()
                {
                    let sqlstate = extract_sqlstate(payload);
                    let is_like = matches!(
                        self.write_in_flight[self.write_cursor].0.op,
                        WriteOp::LikePost { .. }
                    );
                    let outcome = if is_like && &sqlstate == b"23503" {
                        WriteOutcome::NotFound404
                    } else {
                        WriteOutcome::Internal500
                    };
                    self.write_in_flight[self.write_cursor].1 = outcome;
                    self.write_batch_aborted_at = Some(self.write_cursor);
                }
                Ok(())
            }
            b'Z' => {
                // Rule 8: Write responses are emitted strictly after ReadyForQuery confirms WAL commit.
                self.record_rtt(now_ns);
                let batch = std::mem::take(&mut self.write_in_flight);
                let aborted_at = self.write_batch_aborted_at.take();
                self.write_cursor = 0;

                if let Some(err_idx) = aborted_at {
                    let mut siblings_rev = Vec::with_capacity(batch.len().saturating_sub(1));
                    for (idx, (mut req, outcome)) in batch.into_iter().enumerate() {
                        if idx == err_idx {
                            let s = match outcome {
                                WriteOutcome::NotFound404 => {
                                    if req.close_after {
                                        RESP_404_POST_NOT_FOUND_CLOSE
                                    } else {
                                        RESP_404_POST_NOT_FOUND
                                    }
                                }
                                _ => {
                                    if req.close_after {
                                        RESP_500_INTERNAL_CLOSE
                                    } else {
                                        RESP_500_INTERNAL
                                    }
                                }
                            };
                            on_reply(
                                req.client_fd,
                                req.client_gen,
                                req.close_after,
                                req.enqueued_ms,
                                ReplyPayload::Static(s),
                            );
                        } else {
                            req.single_step = true;
                            req.enqueued_ns = 0;
                            siblings_rev.push(req);
                        }
                    }
                    for req in siblings_rev.into_iter().rev() {
                        retry_writes_out.push_front(req);
                    }
                } else {
                    for (req, outcome) in batch {
                        self.emit_committed_write(
                            &req,
                            outcome,
                            gz_min_bytes,
                            igzip,
                            now_ms,
                            on_reply,
                        );
                    }
                }
                Ok(())
            }
            _ => Ok(()),
        }
    }

    fn emit_committed_write<F>(
        &mut self,
        req: &WriteReq,
        outcome: WriteOutcome,
        gz_min_bytes: usize,
        igzip: *mut IgzipCtx,
        now_ms: u64,
        on_reply: &mut F,
    ) where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        match (&req.op, outcome) {
            (
                WriteOp::CreatePost { username, body, .. },
                WriteOutcome::CreatedPost {
                    post_id,
                    created_pg_us,
                },
            ) => {
                if self.json_scratch.len() >= HDR_HEADROOM {
                    self.json_scratch.truncate(HDR_HEADROOM);
                } else {
                    self.json_scratch.resize(HDR_HEADROOM, 0);
                }
                self.json_scratch.extend_from_slice(b"{\"post\":{\"id\":");
                write_i64(&mut self.json_scratch, post_id);
                self.json_scratch.extend_from_slice(b",\"body\":\"");
                write_json_escaped(&mut self.json_scratch, body.as_slice());
                self.json_scratch.extend_from_slice(b"\",\"created_at\":\"");
                self.day_cache = DayPrefixCache::new();
                write_pg_timestamptz(&mut self.json_scratch, created_pg_us, &mut self.day_cache);
                self.json_scratch.extend_from_slice(b"\",\"author\":\"");
                write_json_escaped(&mut self.json_scratch, username.as_slice());
                self.json_scratch.extend_from_slice(b"\",\"like_count\":0}}");
                let rep = finalize_json_reply(
                    &mut self.json_scratch,
                    &mut self.zbuf_scratch,
                    201,
                    req.accept_gzip,
                    req.close_after,
                    gz_min_bytes,
                    igzip,
                    now_ms,
                );
                on_reply(
                    req.client_fd,
                    req.client_gen,
                    req.close_after,
                    req.enqueued_ms,
                    rep,
                );
            }
            (WriteOp::LikePost { post_id, .. }, WriteOutcome::Liked { first_time }) => {
                if self.json_scratch.len() >= HDR_HEADROOM {
                    self.json_scratch.truncate(HDR_HEADROOM);
                } else {
                    self.json_scratch.resize(HDR_HEADROOM, 0);
                }
                if first_time {
                    self.json_scratch
                        .extend_from_slice(b"{\"liked\":true,\"already_liked\":false,\"post_id\":");
                } else {
                    self.json_scratch
                        .extend_from_slice(b"{\"liked\":true,\"already_liked\":true,\"post_id\":");
                }
                write_u64(&mut self.json_scratch, *post_id as u64);
                self.json_scratch.push(b'}');
                let rep = finalize_json_reply(
                    &mut self.json_scratch,
                    &mut self.zbuf_scratch,
                    if first_time { 201 } else { 200 },
                    req.accept_gzip,
                    req.close_after,
                    gz_min_bytes,
                    igzip,
                    now_ms,
                );
                on_reply(
                    req.client_fd,
                    req.client_gen,
                    req.close_after,
                    req.enqueued_ms,
                    rep,
                );
            }
            _ => {
                let s = if req.close_after {
                    RESP_500_INTERNAL_CLOSE
                } else {
                    RESP_500_INTERNAL
                };
                on_reply(
                    req.client_fd,
                    req.client_gen,
                    req.close_after,
                    req.enqueued_ms,
                    ReplyPayload::Static(s),
                );
            }
        }
    }

    pub fn fail_and_reset<F>(
        &mut self,
        now_ns: u64,
        retry_reads_out: &mut VecDeque<ReadReq>,
        on_reply: &mut F,
    ) where
        F: FnMut(i32, u16, bool, u32, ReplyPayload<'_>),
    {
        if self.fd >= 0 {
            unsafe {
                close(self.fd);
            }
            self.fd = -1;
        }
        self.conn_gen = self.conn_gen.wrapping_add(1);
        if self.conn_gen == 0 {
            self.conn_gen = 1;
        }
        self.phase = PgPhase::Disconnected;
        self.epoll_events = 0;
        self.uring_rx_armed = false;
        self.uring_tx_inflight = false;
        self.tx_buf.clear();
        self.tx_pos = 0;
        self.rx_buf.clear();
        self.rx_pos = 0;
        self.next_reconnect_ns = now_ns + 50_000_000;

        if !self.is_write {
            for mut req in self.read_in_flight.drain(..) {
                if req.retries == 0 {
                    req.retries = 1;
                    req.enqueued_ns = 0;
                    retry_reads_out.push_back(req);
                } else {
                    let s = match req.op {
                        ReadOp::Health => {
                            if req.close_after {
                                RESP_503_DB_UNREACHABLE_CLOSE
                            } else {
                                RESP_503_DB_UNREACHABLE
                            }
                        }
                        _ => {
                            if req.close_after {
                                RESP_500_INTERNAL_CLOSE
                            } else {
                                RESP_500_INTERNAL
                            }
                        }
                    };
                    on_reply(
                        req.client_fd,
                        req.client_gen,
                        req.close_after,
                        req.enqueued_ms,
                        ReplyPayload::Static(s),
                    );
                }
            }
            self.read_cursor = 0;
        } else {
            for (req, _) in self.write_in_flight.drain(..) {
                let s = if req.close_after {
                    RESP_500_INTERNAL_CLOSE
                } else {
                    RESP_500_INTERNAL
                };
                on_reply(
                    req.client_fd,
                    req.client_gen,
                    req.close_after,
                    req.enqueued_ms,
                    ReplyPayload::Static(s),
                );
            }
            self.write_cursor = 0;
            self.write_batch_aborted_at = None;
        }
    }
}

fn append_parse(buf: &mut Vec<u8>, name_nul: &[u8], sql_nul: &[u8], oids: &[u32]) {
    let len = (4 + name_nul.len() + sql_nul.len() + 2 + oids.len() * 4) as u32;
    buf.push(b'P');
    buf.extend_from_slice(&len.to_be_bytes());
    buf.extend_from_slice(name_nul);
    buf.extend_from_slice(sql_nul);
    buf.extend_from_slice(&(oids.len() as u16).to_be_bytes());
    for &oid in oids {
        buf.extend_from_slice(&oid.to_be_bytes());
    }
}

#[inline(always)]
fn append_get_post_bind_exec(buf: &mut Vec<u8>, post_id: i64) {
    let mut msg = [
        b'B', 0, 0, 0, 29, 0, b'g', 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
        0, 1, b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0,
    ];
    msg[18..26].copy_from_slice(&post_id.to_be_bytes());
    buf.extend_from_slice(&msg);
}

#[inline(always)]
fn append_like_post_bind_exec(buf: &mut Vec<u8>, user_id: i64, post_id: i64) {
    let mut msg = [
        b'B', 0, 0, 0, 41, 0, b'l', 0, 0, 2, 0, 1, 0, 1, 0, 2, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0,
    ];
    msg[20..28].copy_from_slice(&user_id.to_be_bytes());
    msg[32..40].copy_from_slice(&post_id.to_be_bytes());
    buf.extend_from_slice(&msg);
}

#[inline(always)]
fn append_create_post_bind_exec(buf: &mut Vec<u8>, user_id: i64, body: &[u8]) {
    let bind_len = (4 + 1 + 2 + 2 + 4 + 2 + 4 + 8 + 4 + body.len() + 2 + 2) as u32;
    buf.reserve(1 + bind_len as usize + 10);
    buf.push(b'B');
    buf.extend_from_slice(&bind_len.to_be_bytes());
    buf.push(0);
    buf.extend_from_slice(b"c\0");
    buf.extend_from_slice(&2u16.to_be_bytes());
    buf.extend_from_slice(&1u16.to_be_bytes());
    buf.extend_from_slice(&0u16.to_be_bytes());
    buf.extend_from_slice(&2u16.to_be_bytes());
    buf.extend_from_slice(&8u32.to_be_bytes());
    buf.extend_from_slice(&user_id.to_be_bytes());
    buf.extend_from_slice(&(body.len() as u32).to_be_bytes());
    buf.extend_from_slice(body);
    buf.extend_from_slice(&1u16.to_be_bytes());
    buf.extend_from_slice(&1u16.to_be_bytes());
    buf.extend_from_slice(&[b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0]);
}

#[inline(always)]
fn read_col<'a>(row: &'a [u8], pos: &mut usize) -> Result<&'a [u8], ()> {
    if *pos + 4 > row.len() {
        return Err(());
    }
    let len = i32::from_be_bytes([row[*pos], row[*pos + 1], row[*pos + 2], row[*pos + 3]]);
    *pos += 4;
    if len < 0 {
        return Err(());
    }
    let ulen = len as usize;
    if *pos + ulen > row.len() {
        return Err(());
    }
    let s = &row[*pos..*pos + ulen];
    *pos += ulen;
    Ok(s)
}

#[inline(always)]
fn format_post_datarow(
    row: &[u8],
    out: &mut Vec<u8>,
    day_cache: &mut DayPrefixCache,
) -> Result<(), ()> {
    if row.len() < 2 {
        return Err(());
    }
    let ncols = u16::from_be_bytes([row[0], row[1]]);
    if ncols != 5 {
        return Err(());
    }
    let mut pos = 2usize;
    let id_bin = read_col(row, &mut pos)?;
    let body_txt = read_col(row, &mut pos)?;
    let created_bin = read_col(row, &mut pos)?;
    let author_txt = read_col(row, &mut pos)?;
    let likes_bin = read_col(row, &mut pos)?;
    if id_bin.len() != 8 || created_bin.len() != 8 || likes_bin.len() != 4 {
        return Err(());
    }
    let post_id = i64::from_be_bytes(id_bin.try_into().unwrap());
    let pg_us = i64::from_be_bytes(created_bin.try_into().unwrap());
    let likes = i32::from_be_bytes(likes_bin.try_into().unwrap());

    let cur_len = out.len();
    let max_add = 128 + body_txt.len() * 6 + author_txt.len() * 6;
    if out.capacity() - cur_len < max_add {
        out.reserve(max_add);
    }
    unsafe {
        let base = out.as_mut_ptr();
        let mut p = base.add(cur_len);
        std::ptr::copy_nonoverlapping(b"{\"id\":".as_ptr(), p, 6);
        p = write_i64_ptr(p.add(6), post_id);
        std::ptr::copy_nonoverlapping(b",\"body\":\"".as_ptr(), p, 9);
        p = write_json_escaped_ptr(p.add(9), body_txt);
        std::ptr::copy_nonoverlapping(b"\",\"created_at\":\"".as_ptr(), p, 16);
        p = write_pg_timestamptz_ptr(p.add(16), pg_us, day_cache);
        std::ptr::copy_nonoverlapping(b"\",\"author\":\"".as_ptr(), p, 12);
        p = write_json_escaped_ptr(p.add(12), author_txt);
        std::ptr::copy_nonoverlapping(b"\",\"like_count\":".as_ptr(), p, 15);
        p = write_i64_ptr(p.add(15), likes as i64);
        *p = b'}';
        p = p.add(1);
        let new_len = (p as usize) - (base as usize);
        out.set_len(new_len);
    }
    Ok(())
}

#[inline(always)]
fn parse_create_post_datarow(row: &[u8]) -> Result<(i64, i64), ()> {
    if row.len() < 2 {
        return Err(());
    }
    let ncols = u16::from_be_bytes([row[0], row[1]]);
    if ncols != 2 {
        return Err(());
    }
    let mut pos = 2usize;
    let id_bin = read_col(row, &mut pos)?;
    let created_bin = read_col(row, &mut pos)?;
    if id_bin.len() != 8 || created_bin.len() != 8 {
        return Err(());
    }
    let post_id = i64::from_be_bytes(id_bin.try_into().unwrap());
    let pg_us = i64::from_be_bytes(created_bin.try_into().unwrap());
    Ok((post_id, pg_us))
}

fn extract_sqlstate(err_payload: &[u8]) -> [u8; 5] {
    let mut out = [0u8; 5];
    let mut pos = 0usize;
    while pos < err_payload.len() {
        let code = err_payload[pos];
        if code == 0 {
            break;
        }
        pos += 1;
        let Some(nul_rel) = err_payload[pos..].iter().position(|&b| b == 0) else {
            break;
        };
        let val = &err_payload[pos..pos + nul_rel];
        if code == b'C' && val.len() == 5 {
            out.copy_from_slice(val);
            return out;
        }
        pos += nul_rel + 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_binary_post_datarow_and_create_post_parsing() {
        let mut row = Vec::new();
        row.extend_from_slice(&5u16.to_be_bytes());
        row.extend_from_slice(&8i32.to_be_bytes());
        row.extend_from_slice(&42i64.to_be_bytes());
        let body = b"hello \"world\"";
        row.extend_from_slice(&(body.len() as i32).to_be_bytes());
        row.extend_from_slice(body);
        row.extend_from_slice(&8i32.to_be_bytes());
        row.extend_from_slice(&820_540_800_123_000i64.to_be_bytes());
        let author = b"alice";
        row.extend_from_slice(&(author.len() as i32).to_be_bytes());
        row.extend_from_slice(author);
        row.extend_from_slice(&4i32.to_be_bytes());
        row.extend_from_slice(&7i32.to_be_bytes());

        let mut out = Vec::new();
        let mut cache = DayPrefixCache::new();
        format_post_datarow(&row, &mut out, &mut cache).unwrap();
        assert_eq!(
            std::str::from_utf8(&out).unwrap(),
            "{\"id\":42,\"body\":\"hello \\\"world\\\"\",\"created_at\":\"2026-01-01T00:00:00.123Z\",\"author\":\"alice\",\"like_count\":7}"
        );

        let mut create_row = Vec::new();
        create_row.extend_from_slice(&2u16.to_be_bytes());
        create_row.extend_from_slice(&8i32.to_be_bytes());
        create_row.extend_from_slice(&9001i64.to_be_bytes());
        create_row.extend_from_slice(&8i32.to_be_bytes());
        create_row.extend_from_slice(&820_454_400_500_000i64.to_be_bytes());
        assert_eq!(
            parse_create_post_datarow(&create_row),
            Ok((9001, 820_454_400_500_000))
        );
    }

    #[test]
    fn test_zero_copy_uring_rx_arbitrary_chunk_splits() {
        let mut wire = Vec::new();
        for id in [101i64, 202i64] {
            wire.extend_from_slice(&[b'2', 0, 0, 0, 4]);
            let mut row = Vec::new();
            row.extend_from_slice(&5u16.to_be_bytes());
            row.extend_from_slice(&8i32.to_be_bytes());
            row.extend_from_slice(&id.to_be_bytes());
            let body = b"split-frame check";
            row.extend_from_slice(&(body.len() as i32).to_be_bytes());
            row.extend_from_slice(body);
            row.extend_from_slice(&8i32.to_be_bytes());
            row.extend_from_slice(&820_540_800_999_000i64.to_be_bytes());
            let author = b"bob";
            row.extend_from_slice(&(author.len() as i32).to_be_bytes());
            row.extend_from_slice(author);
            row.extend_from_slice(&4i32.to_be_bytes());
            row.extend_from_slice(&3i32.to_be_bytes());

            wire.push(b'D');
            wire.extend_from_slice(&((4 + row.len()) as u32).to_be_bytes());
            wire.extend_from_slice(&row);
            wire.extend_from_slice(b"C\x00\x00\x00\x0dSELECT 1\x00");
        }
        wire.extend_from_slice(b"Z\x00\x00\x00\x05I");

        let dummy_cfg = DbConfig {
            addr: SockAddrIn {
                sin_family: 2,
                sin_port: 0,
                sin_addr: 0,
                sin_zero: [0; 8],
            },
            user: "feed".to_string(),
            password: "feed".to_string(),
            dbname: "feed".to_string(),
        };

        for step in [1usize, 3, 5, 17, 64, wire.len()] {
            let mut lane = PgLane::new(false);
            lane.phase = PgPhase::Ready;
            let mut q = VecDeque::new();
            q.push_back(ReadReq {
                client_fd: 10,
                client_gen: 1,
                close_after: false,
                accept_gzip: false,
                retries: 0,
                enqueued_ms: 1,
                enqueued_ns: 1,
                op: ReadOp::GetPost { post_id: 101 },
            });
            q.push_back(ReadReq {
                client_fd: 11,
                client_gen: 2,
                close_after: false,
                accept_gzip: false,
                retries: 0,
                enqueued_ms: 1,
                enqueued_ns: 1,
                op: ReadOp::GetPost { post_id: 202 },
            });
            lane.send_read_batch(&mut q, 48, 1000, 1, 0, true).unwrap();
            lane.tx_buf.clear();
            lane.tx_pos = 0;

            let mut replies: Vec<String> = Vec::new();
            let mut rr = VecDeque::new();
            let mut rw = VecDeque::new();
            for chunk in wire.chunks(step) {
                lane.ingest_uring_rx_slice(
                    chunk,
                    &dummy_cfg,
                    2000,
                    5,
                    1024,
                    std::ptr::null_mut(),
                    &mut rr,
                    &mut rw,
                    &mut |_fd, _gen, _ca, _ms, rep| match rep {
                        ReplyPayload::Static(s) => {
                            replies.push(std::str::from_utf8(s).unwrap().to_string());
                        }
                        ReplyPayload::OwnedSlice { buf, start, end } => {
                            replies.push(std::str::from_utf8(&buf[start..end]).unwrap().to_string());
                        }
                    },
                );
            }
            assert_eq!(replies.len(), 2, "step={}", step);
            assert!(replies[0].contains("\"id\":101"), "step={}", step);
            assert!(replies[1].contains("\"id\":202"), "step={}", step);
            assert!(lane.is_idle_ready(), "step={}", step);
        }
    }
}
