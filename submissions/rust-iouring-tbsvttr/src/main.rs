mod gzip;
mod http;
mod json;
mod jwt;
mod pg;
mod sys;

use std::collections::VecDeque;
use std::ffi::c_int;
use std::io;
use std::net::{Ipv4Addr, SocketAddrV4, ToSocketAddrs};
use std::sync::Arc;
use std::time::Instant;

use crate::gzip::FastGzip;
use crate::http::{Method, ParseState, parse_http_request, write_http_response};
use crate::json::{BodyError, extract_and_validate_post_body};
use crate::jwt::{JwtError, JwtVerifier};
use crate::pg::{CompletedHttp, DbConfig, PgConn, ReadKind, ReadTask, WriteKind, WriteTask};
use crate::sys::{
    AF_INET, CLOCK_REALTIME, EAGAIN, IPPROTO_TCP, MSG_NOSIGNAL, RLIMIT_NOFILE, SIG_IGN, SIGPIPE,
    SO_REUSEADDR, SO_REUSEPORT, SOCK_STREAM, SOL_SOCKET, bind, clock_gettime, close, getrlimit,
    listen, recv, rlimit, send, set_nonblocking, set_tcp_nodelay, setrlimit, setsockopt, signal,
    sockaddr_in, socket, timespec,
};

const TOTAL_PG_READS: usize = 48;
const TOTAL_PG_WRITES: usize = 16;
const POOL_MAX: usize = 4096;

#[derive(Clone)]
struct ServerConfig {
    db: DbConfig,
    jwt: JwtVerifier,
    bind_addrs: Vec<SocketAddrV4>,
    started: Instant,
    reads_per_worker: usize,
    writes_per_worker: usize,
}

#[derive(Default)]
struct ClientSlot {
    generation: u32,
    tx_pos: u32,
    active: bool,
    closing: bool,
    in_flight_db: bool,
    want_epollout: bool,
    rx_buf: Option<Box<Vec<u8>>>,
    tx_buf: Option<Box<Vec<u8>>>,
}

const _: () = assert!(std::mem::size_of::<ClientSlot>() == 32);

#[allow(dead_code)]
struct Worker {
    cfg: Arc<ServerConfig>,
    epfd: c_int,
    listen_fds: Vec<c_int>,
    read_conns: Vec<PgConn>,
    write_conns: Vec<PgConn>,
    read_queue: VecDeque<ReadTask>,
    write_queue: VecDeque<WriteTask>,
    clients: Vec<ClientSlot>,
    active_clients: usize,
    box_pool: Vec<Box<Vec<u8>>>,
    resp_pool: Vec<Vec<u8>>,
    recv_scratch: Vec<u8>,
    gzip: FastGzip,
    gzip_scratch: Vec<u8>,
    json_scratch: Vec<u8>,
    completed_http: Vec<CompletedHttp>,
}

fn main() {
    unsafe {
        signal(SIGPIPE, SIG_IGN);
        let mut rl = rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        if getrlimit(RLIMIT_NOFILE, &mut rl) == 0 {
            rl.rlim_cur = rl.rlim_max;
            let _ = setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

    let host_str = std::env::var("HOST")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "0.0.0.0".to_string());
    let port_primary: u16 = std::env::var("PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(80);
    let port_extra: Option<u16> = std::env::var("EXTRA_PORT")
        .ok()
        .and_then(|s| s.parse().ok());

    let host_ip = resolve_ipv4(&host_str, port_primary);
    let mut bind_addrs = vec![SocketAddrV4::new(host_ip, port_primary)];
    if let Some(ep) = port_extra {
        if ep != port_primary {
            bind_addrs.push(SocketAddrV4::new(host_ip, ep));
        }
    }

    let db_url = std::env::var("DATABASE_URL")
        .unwrap_or_else(|_| "postgres://postgres:postgres@127.0.0.1:5432/feed".to_string());
    let jwt_secret = std::env::var("JWT_SECRET")
        .unwrap_or_else(|_| "million-users-challenge".to_string());

    let core_ids = detect_allowed_cores();
    let num_workers = core_ids.len().max(1);

    let cfg = Arc::new(ServerConfig {
        db: DbConfig::parse(&db_url),
        jwt: JwtVerifier::new(jwt_secret.as_bytes()),
        bind_addrs,
        started: Instant::now(),
        reads_per_worker: TOTAL_PG_READS.div_ceil(num_workers).max(2),
        writes_per_worker: TOTAL_PG_WRITES.div_ceil(num_workers).max(2),
    });

    let mut handles = Vec::new();
    for _idx in 1..num_workers {
        let w_cfg = Arc::clone(&cfg);
        handles.push(std::thread::spawn(move || {
            run_worker(w_cfg);
        }));
    }

    run_worker(cfg);

    for h in handles {
        let _ = h.join();
    }
}

fn resolve_ipv4(host: &str, port: u16) -> Ipv4Addr {
    host.parse::<Ipv4Addr>()
        .or_else(|_| {
            (host, port)
                .to_socket_addrs()
                .ok()
                .and_then(|mut it| {
                    it.find_map(|sa| match sa {
                        std::net::SocketAddr::V4(v4) => Some(*v4.ip()),
                        _ => None,
                    })
                })
                .ok_or(())
        })
        .unwrap_or(Ipv4Addr::UNSPECIFIED)
}

fn detect_allowed_cores() -> Vec<usize> {
    #[cfg(target_os = "linux")]
    unsafe {
        let mut mask = [0u8; 128];
        if crate::sys::sched_getaffinity(0, mask.len(), mask.as_mut_ptr()) == 0 {
            let mut cores = Vec::new();
            for (byte_idx, &b) in mask.iter().enumerate() {
                if b != 0 {
                    for bit in 0..8 {
                        if (b & (1 << bit)) != 0 {
                            cores.push(byte_idx * 8 + bit);
                        }
                    }
                }
            }
            if !cores.is_empty() {
                return cores;
            }
        }
    }
    let n = std::thread::available_parallelism()
        .map(|n| n.get())
        .unwrap_or(1);
    (0..n).collect()
}

fn create_listener(addr: SocketAddrV4) -> io::Result<c_int> {
    let fd = unsafe { socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let one: c_int = 1;
    unsafe {
        let _ = setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            (&one as *const c_int).cast(),
            std::mem::size_of::<c_int>() as u32,
        );
        let _ = setsockopt(
            fd,
            SOL_SOCKET,
            SO_REUSEPORT,
            (&one as *const c_int).cast(),
            std::mem::size_of::<c_int>() as u32,
        );
    }
    set_tcp_nodelay(fd);
    set_nonblocking(fd);

    let sin = sockaddr_in {
        #[cfg(not(target_os = "linux"))]
        sin_len: std::mem::size_of::<sockaddr_in>() as u8,
        sin_family: AF_INET as _,
        sin_port: addr.port().to_be(),
        sin_addr: u32::from_ne_bytes(addr.ip().octets()),
        sin_zero: [0; 8],
    };

    if unsafe {
        bind(
            fd,
            (&sin as *const sockaddr_in).cast(),
            std::mem::size_of::<sockaddr_in>() as u32,
        )
    } < 0
    {
        let err = io::Error::last_os_error();
        unsafe { close(fd) };
        return Err(err);
    }

    if unsafe { listen(fd, 65535) } < 0 {
        let err = io::Error::last_os_error();
        unsafe { close(fd) };
        return Err(err);
    }

    #[cfg(target_os = "linux")]
    unsafe {
        let defer_secs: c_int = 1;
        let zero: c_int = 0;
        let _ = setsockopt(
            fd,
            IPPROTO_TCP,
            crate::sys::TCP_DEFER_ACCEPT,
            (&defer_secs as *const c_int).cast(),
            std::mem::size_of::<c_int>() as u32,
        );
        let _ = setsockopt(
            fd,
            IPPROTO_TCP,
            crate::sys::TCP_QUICKACK,
            (&zero as *const c_int).cast(),
            std::mem::size_of::<c_int>() as u32,
        );
    }

    Ok(fd)
}

#[inline]
fn unix_now_secs() -> i64 {
    let mut ts = timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    unsafe {
        clock_gettime(CLOCK_REALTIME, &mut ts);
    }
    ts.tv_sec
}

#[inline]
fn parse_post_id(bytes: &[u8]) -> Option<i64> {
    if bytes.is_empty() || bytes.len() > 19 {
        return None;
    }
    let mut val = 0i64;
    for &b in bytes {
        if !b.is_ascii_digit() {
            return None;
        }
        val = val.checked_mul(10)?.checked_add((b - b'0') as i64)?;
    }
    if val >= 1 { Some(val) } else { None }
}

#[allow(dead_code)]
const TAG_LISTEN: u64 = 1 << 60;
#[allow(dead_code)]
const TAG_PG_READ: u64 = 2 << 60;
#[allow(dead_code)]
const TAG_PG_WRITE: u64 = 3 << 60;
#[allow(dead_code)]
const TAG_CLIENT: u64 = 4 << 60;
#[allow(dead_code)]
const TAG_MASK: u64 = 0xF << 60;
#[allow(dead_code)]
const VAL_MASK: u64 = !TAG_MASK;

impl Worker {
    fn new(cfg: Arc<ServerConfig>, epfd: c_int) -> Self {
        let mut listen_fds = Vec::with_capacity(cfg.bind_addrs.len());
        for &addr in &cfg.bind_addrs {
            let lfd = create_listener(addr).expect("failed to bind listener");
            listen_fds.push(lfd);
        }

        let mut read_conns = Vec::with_capacity(cfg.reads_per_worker);
        let mut write_conns = Vec::with_capacity(cfg.writes_per_worker);
        let max_conns = cfg.reads_per_worker.max(cfg.writes_per_worker);
        for i in 0..max_conns {
            if i < cfg.reads_per_worker {
                if let Ok(c) = PgConn::connect_and_prepare(&cfg.db, false) {
                    read_conns.push(c);
                }
            }
            if i < cfg.writes_per_worker {
                if let Ok(c) = PgConn::connect_and_prepare(&cfg.db, true) {
                    write_conns.push(c);
                }
            }
        }

        Self {
            cfg,
            epfd,
            listen_fds,
            read_conns,
            write_conns,
            read_queue: VecDeque::with_capacity(4096),
            write_queue: VecDeque::with_capacity(2048),
            clients: Vec::with_capacity(65536),
            active_clients: 0,
            box_pool: Vec::with_capacity(512),
            resp_pool: Vec::with_capacity(2048),
            recv_scratch: vec![0u8; 16384],
            gzip: FastGzip::new(),
            gzip_scratch: Vec::with_capacity(2048),
            json_scratch: Vec::with_capacity(2048),
            completed_http: Vec::with_capacity(256),
        }
    }

    #[inline]
    fn take_box_buf(&mut self) -> Box<Vec<u8>> {
        self.box_pool
            .pop()
            .unwrap_or_else(|| Box::new(Vec::with_capacity(4096)))
    }

    #[inline]
    fn give_box_buf(&mut self, b: Option<Box<Vec<u8>>>) {
        if let Some(mut v) = b {
            if self.box_pool.len() < POOL_MAX && v.capacity() <= 16384 {
                v.clear();
                self.box_pool.push(v);
            }
        }
    }

    #[inline]
    fn give_resp_buf(&mut self, mut v: Vec<u8>) {
        if self.resp_pool.len() < POOL_MAX && v.capacity() <= 16384 {
            v.clear();
            self.resp_pool.push(v);
        }
    }

    #[inline]
    fn update_client_epollout(&mut self, fd: c_int, want_out: bool) {
        let idx = fd as usize;
        if idx >= self.clients.len() || !self.clients[idx].active {
            return;
        }
        if self.clients[idx].want_epollout == want_out {
            return;
        }
        self.clients[idx].want_epollout = want_out;
        #[cfg(target_os = "linux")]
        if self.epfd >= 0 {
            use crate::sys::{EPOLL_CTL_MOD, EPOLLIN, EPOLLOUT, EPOLLRDHUP, epoll_ctl, epoll_event};
            let mut cev = epoll_event {
                events: EPOLLIN | EPOLLRDHUP | if want_out { EPOLLOUT } else { 0 },
                u64_data: TAG_CLIENT | (fd as u64),
            };
            unsafe { epoll_ctl(self.epfd, EPOLL_CTL_MOD, fd, &mut cev) };
        }
    }

    #[allow(unused_variables)]
    #[inline]
    fn sync_pg_epollout(&mut self, is_write: bool, conn_idx: usize) {
        #[cfg(target_os = "linux")]
        if self.epfd >= 0 {
            use crate::sys::{EPOLL_CTL_MOD, EPOLLIN, EPOLLOUT, epoll_ctl, epoll_event};
            let conn = if !is_write {
                &mut self.read_conns[conn_idx]
            } else {
                &mut self.write_conns[conn_idx]
            };
            let want_out = conn.tx_pos < conn.tx_buf.len();
            if conn.want_epollout != want_out {
                conn.want_epollout = want_out;
                let tag = if !is_write { TAG_PG_READ } else { TAG_PG_WRITE };
                let mut pev = epoll_event {
                    events: EPOLLIN | if want_out { EPOLLOUT } else { 0 },
                    u64_data: tag | (conn_idx as u64),
                };
                unsafe { epoll_ctl(self.epfd, EPOLL_CTL_MOD, conn.fd, &mut pev) };
            }
        }
    }

    #[inline]
    fn ensure_client_slot(&mut self, fd: c_int) {
        let idx = fd as usize;
        if idx >= self.clients.len() {
            self.clients.resize_with(idx + 1, ClientSlot::default);
        }
    }

    fn register_client(&mut self, fd: c_int) {
        self.ensure_client_slot(fd);
        let old_rx = self.clients[fd as usize].rx_buf.take();
        let old_tx = self.clients[fd as usize].tx_buf.take();
        let was_active = self.clients[fd as usize].active;
        self.give_box_buf(old_rx);
        self.give_box_buf(old_tx);
        if !was_active {
            self.active_clients += 1;
        }
        let slot = &mut self.clients[fd as usize];
        slot.generation = slot.generation.wrapping_add(1);
        slot.active = true;
        slot.closing = false;
        slot.in_flight_db = false;
        slot.want_epollout = false;
        slot.tx_pos = 0;
    }

    fn close_client(&mut self, fd: c_int) {
        let idx = fd as usize;
        if idx < self.clients.len() && self.clients[idx].active {
            let old_rx = self.clients[idx].rx_buf.take();
            let old_tx = self.clients[idx].tx_buf.take();
            let slot = &mut self.clients[idx];
            slot.active = false;
            slot.closing = false;
            slot.in_flight_db = false;
            slot.want_epollout = false;
            slot.generation = slot.generation.wrapping_add(1);
            slot.tx_pos = 0;
            self.active_clients = self.active_clients.saturating_sub(1);
            self.give_box_buf(old_rx);
            self.give_box_buf(old_tx);
        }
        unsafe { close(fd) };
    }

    fn handle_client_readable(&mut self, fd: c_int) {
        let idx = fd as usize;
        if idx >= self.clients.len() || !self.clients[idx].active {
            return;
        }

        let mut total_read = 0usize;
        let mut eof = false;

        loop {
            let rem_cap = self.recv_scratch.len() - total_read;
            if rem_cap == 0 {
                break;
            }
            let n = unsafe {
                recv(
                    fd,
                    self.recv_scratch.as_mut_ptr().add(total_read).cast(),
                    rem_cap,
                    0,
                )
            };
            if n > 0 {
                let n_usz = n as usize;
                total_read += n_usz;
                if n_usz < rem_cap {
                    break;
                }
            } else if n == 0 {
                eof = true;
                break;
            } else {
                let err = io::Error::last_os_error();
                if err.raw_os_error() != Some(EAGAIN) {
                    eof = true;
                }
                break;
            }
        }

        if total_read == 0 {
            if eof {
                self.close_client(fd);
            }
            return;
        }

        /* Fast path: if no partial rx_buf exists and client is not waiting on DB, parse in-place! */
        let has_partial = self.clients[idx]
            .rx_buf
            .as_ref()
            .is_some_and(|b| !b.is_empty());
        if !has_partial && !self.clients[idx].in_flight_db {
            let raw_ptr = self.recv_scratch.as_ptr();
            let slice = unsafe { std::slice::from_raw_parts(raw_ptr, total_read) };
            let client_gen = self.clients[idx].generation;
            match parse_http_request(slice) {
                ParseState::Complete(req) => {
                    let consumed = req.total_consumed;
                    let close_conn = req.close_conn;
                    self.dispatch_http_request(fd, client_gen, close_conn, req);
                    if self.clients[idx].active && consumed < total_read {
                        let mut rx = self.take_box_buf();
                        rx.extend_from_slice(&slice[consumed..]);
                        self.clients[idx].rx_buf = Some(rx);
                        if !self.clients[idx].in_flight_db {
                            self.process_client_rx(fd);
                        }
                    }
                    return;
                }
                ParseState::Invalid => {
                    let mut out = self
                        .resp_pool
                        .pop()
                        .unwrap_or_else(|| Vec::with_capacity(128));
                    out.clear();
                    write_http_response(&mut out, 400, true, false, b"{\"error\":\"bad request\"}");
                    self.queue_client_response(fd, client_gen, true, out);
                    return;
                }
                ParseState::Incomplete => {
                    if eof {
                        self.close_client(fd);
                        return;
                    }
                    let mut rx = self.take_box_buf();
                    rx.extend_from_slice(slice);
                    self.clients[idx].rx_buf = Some(rx);
                    return;
                }
            }
        }

        let mut rx = self.clients[idx]
            .rx_buf
            .take()
            .unwrap_or_else(|| self.take_box_buf());
        rx.extend_from_slice(&self.recv_scratch[..total_read]);
        self.clients[idx].rx_buf = Some(rx);
        self.process_client_rx(fd);
    }

    fn process_client_rx(&mut self, fd: c_int) {
        let idx = fd as usize;
        if idx >= self.clients.len()
            || !self.clients[idx].active
            || self.clients[idx].in_flight_db
        {
            return;
        }
        let Some(mut rx_buf) = self.clients[idx].rx_buf.take() else {
            return;
        };
        if rx_buf.is_empty() {
            self.give_box_buf(Some(rx_buf));
            return;
        }

        let client_gen = self.clients[idx].generation;

        match parse_http_request(&rx_buf) {
            ParseState::Incomplete => {
                self.clients[idx].rx_buf = Some(rx_buf);
            }
            ParseState::Invalid => {
                self.give_box_buf(Some(rx_buf));
                let mut out = self
                    .resp_pool
                    .pop()
                    .unwrap_or_else(|| Vec::with_capacity(128));
                out.clear();
                write_http_response(&mut out, 400, true, false, b"{\"error\":\"bad request\"}");
                self.queue_client_response(fd, client_gen, true, out);
            }
            ParseState::Complete(req) => {
                let consumed = req.total_consumed;
                let close_conn = req.close_conn;
                self.dispatch_http_request(fd, client_gen, close_conn, req);
                if self.clients[idx].active && consumed < rx_buf.len() {
                    rx_buf.drain(..consumed);
                    self.clients[idx].rx_buf = Some(rx_buf);
                } else {
                    self.give_box_buf(Some(rx_buf));
                }
            }
        }
    }

    #[inline]
    fn enqueue_read(&mut self, task: ReadTask) {
        self.read_queue.push_back(task);
    }

    #[inline]
    fn enqueue_write(&mut self, task: WriteTask) {
        self.write_queue.push_back(task);
    }

    fn dispatch_http_request(
        &mut self,
        fd: c_int,
        client_gen: u32,
        close_conn: bool,
        req: crate::http::ParsedRequest<'_>,
    ) {
        let path = req.path;

        if req.method == Method::Get {
            if path == b"/feed" {
                if self.read_conns.is_empty() {
                    self.send_immediate_json(
                        fd,
                        client_gen,
                        500,
                        close_conn,
                        b"{\"error\":\"internal server error\"}",
                    );
                    return;
                }
                self.clients[fd as usize].in_flight_db = true;
                self.enqueue_read(ReadTask {
                    client_fd: fd,
                    client_gen,
                    close_conn,
                    kind: ReadKind::Feed {
                        accept_gzip: req.accept_gzip,
                    },
                });
                return;
            }

            if path == b"/health" {
                let uptime_s = self.cfg.started.elapsed().as_secs();
                if self.read_conns.is_empty() {
                    self.send_immediate_json(
                        fd,
                        client_gen,
                        503,
                        close_conn,
                        b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}",
                    );
                    return;
                }
                self.clients[fd as usize].in_flight_db = true;
                self.enqueue_read(ReadTask {
                    client_fd: fd,
                    client_gen,
                    close_conn,
                    kind: ReadKind::Health { uptime_s },
                });
                return;
            }

            if let Some(rest) = path.strip_prefix(b"/posts/") {
                if !rest.contains(&b'/') {
                    let Some(post_id) = parse_post_id(rest) else {
                        self.send_immediate_json(
                            fd,
                            client_gen,
                            400,
                            close_conn,
                            b"{\"error\":\"invalid post id\"}",
                        );
                        return;
                    };
                    if self.read_conns.is_empty() {
                        self.send_immediate_json(
                            fd,
                            client_gen,
                            500,
                            close_conn,
                            b"{\"error\":\"internal server error\"}",
                        );
                        return;
                    }
                    self.clients[fd as usize].in_flight_db = true;
                    self.enqueue_read(ReadTask {
                        client_fd: fd,
                        client_gen,
                        close_conn,
                        kind: ReadKind::GetPost { post_id },
                    });
                    return;
                }
            }

            self.send_immediate_json(
                fd,
                client_gen,
                404,
                close_conn,
                b"{\"error\":\"not found\"}",
            );
            return;
        }

        if req.method == Method::Post {
            if path == b"/posts" {
                let now_s = unix_now_secs();
                let claims = match self.cfg.jwt.verify_bearer(req.auth_header, now_s) {
                    Ok(c) => c,
                    Err(JwtError::MissingToken) => {
                        self.send_immediate_json(
                            fd,
                            client_gen,
                            401,
                            close_conn,
                            b"{\"error\":\"missing bearer token\"}",
                        );
                        return;
                    }
                    Err(JwtError::InvalidOrExpired) => {
                        self.send_immediate_json(
                            fd,
                            client_gen,
                            401,
                            close_conn,
                            b"{\"error\":\"invalid or expired token\"}",
                        );
                        return;
                    }
                    Err(JwtError::InvalidPayload) => {
                        self.send_immediate_json(
                            fd,
                            client_gen,
                            401,
                            close_conn,
                            b"{\"error\":\"invalid token payload\"}",
                        );
                        return;
                    }
                };

                let trimmed_body =
                    match extract_and_validate_post_body(req.body, &mut self.json_scratch) {
                        Ok(slice) => slice.to_vec(),
                        Err(BodyError::MalformedJson) => {
                            self.send_immediate_json(
                                fd,
                                client_gen,
                                400,
                                close_conn,
                                b"{\"error\":\"malformed JSON body\"}",
                            );
                            return;
                        }
                        Err(BodyError::MissingOrEmpty) => {
                            self.send_immediate_json(
                                fd,
                                client_gen,
                                400,
                                close_conn,
                                b"{\"error\":\"body is required\"}",
                            );
                            return;
                        }
                        Err(BodyError::TooLong) => {
                            self.send_immediate_json(
                                fd,
                                client_gen,
                                400,
                                close_conn,
                                b"{\"error\":\"body must be at most 500 characters\"}",
                            );
                            return;
                        }
                    };

                if self.write_conns.is_empty() {
                    self.send_immediate_json(
                        fd,
                        client_gen,
                        500,
                        close_conn,
                        b"{\"error\":\"internal server error\"}",
                    );
                    return;
                }

                self.clients[fd as usize].in_flight_db = true;
                self.enqueue_write(WriteTask {
                    client_fd: fd,
                    client_gen,
                    close_conn,
                    kind: WriteKind::CreatePost {
                        user_id: claims.user_id,
                        username_len: claims.username_len as u8,
                        username: claims.username,
                        body: trimmed_body,
                    },
                    out_id_len: 0,
                    out_id_ascii: [0; 24],
                    out_created_micros: 0,
                    out_inserted: false,
                });
                return;
            }

            if let Some(rest) = path.strip_prefix(b"/posts/") {
                if let Some(id_bytes) = rest.strip_suffix(b"/like") {
                    if !id_bytes.is_empty() && !id_bytes.contains(&b'/') {
                        // SPEC: Check auth FIRST, then validate :id!
                        let now_s = unix_now_secs();
                        let claims = match self.cfg.jwt.verify_bearer(req.auth_header, now_s) {
                            Ok(c) => c,
                            Err(JwtError::MissingToken) => {
                                self.send_immediate_json(
                                    fd,
                                    client_gen,
                                    401,
                                    close_conn,
                                    b"{\"error\":\"missing bearer token\"}",
                                );
                                return;
                            }
                            Err(JwtError::InvalidOrExpired) => {
                                self.send_immediate_json(
                                    fd,
                                    client_gen,
                                    401,
                                    close_conn,
                                    b"{\"error\":\"invalid or expired token\"}",
                                );
                                return;
                            }
                            Err(JwtError::InvalidPayload) => {
                                self.send_immediate_json(
                                    fd,
                                    client_gen,
                                    401,
                                    close_conn,
                                    b"{\"error\":\"invalid token payload\"}",
                                );
                                return;
                            }
                        };

                        let Some(post_id) = parse_post_id(id_bytes) else {
                            self.send_immediate_json(
                                fd,
                                client_gen,
                                400,
                                close_conn,
                                b"{\"error\":\"invalid post id\"}",
                            );
                            return;
                        };

                        if self.write_conns.is_empty() {
                            self.send_immediate_json(
                                fd,
                                client_gen,
                                500,
                                close_conn,
                                b"{\"error\":\"internal server error\"}",
                            );
                            return;
                        }

                        self.clients[fd as usize].in_flight_db = true;
                        self.enqueue_write(WriteTask {
                            client_fd: fd,
                            client_gen,
                            close_conn,
                            kind: WriteKind::LikePost {
                                post_id,
                                user_id: claims.user_id,
                            },
                            out_id_len: 0,
                            out_id_ascii: [0; 24],
                            out_created_micros: 0,
                            out_inserted: false,
                        });
                        return;
                    }
                }
            }
        }

        self.send_immediate_json(
            fd,
            client_gen,
            404,
            close_conn,
            b"{\"error\":\"not found\"}",
        );
    }

    fn send_immediate_json(
        &mut self,
        fd: c_int,
        client_gen: u32,
        status: u16,
        close_conn: bool,
        body: &[u8],
    ) {
        let mut resp = self
            .resp_pool
            .pop()
            .unwrap_or_else(|| Vec::with_capacity(256));
        resp.clear();
        write_http_response(&mut resp, status, close_conn, false, body);
        self.queue_client_response(fd, client_gen, close_conn, resp);
    }

    fn queue_client_response(
        &mut self,
        fd: c_int,
        client_gen: u32,
        close_conn: bool,
        resp: Vec<u8>,
    ) {
        let idx = fd as usize;
        if idx >= self.clients.len() {
            self.give_resp_buf(resp);
            return;
        }
        if !self.clients[idx].active || self.clients[idx].generation != client_gen {
            self.give_resp_buf(resp);
            return;
        }
        self.clients[idx].in_flight_db = false;
        self.clients[idx].closing |= close_conn;

        let has_pending_tx = self.clients[idx]
            .tx_buf
            .as_ref()
            .is_some_and(|b| !b.is_empty());

        if !has_pending_tx {
            let n = unsafe { send(fd, resp.as_ptr().cast(), resp.len(), MSG_NOSIGNAL) };
            if n > 0 {
                let sent = n as usize;
                if sent >= resp.len() {
                    self.give_resp_buf(resp);
                    if self.clients[idx].closing {
                        self.close_client(fd);
                    } else if self.clients[idx].rx_buf.is_some() {
                        self.process_client_rx(fd);
                    }
                    return;
                }
                let mut tx_box = self.take_box_buf();
                tx_box.extend_from_slice(&resp[sent..]);
                self.give_resp_buf(resp);
                self.clients[idx].tx_buf = Some(tx_box);
                self.clients[idx].tx_pos = 0;
                self.update_client_epollout(fd, true);
                return;
            }
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(EAGAIN) {
                let mut tx_box = self.take_box_buf();
                tx_box.extend_from_slice(&resp);
                self.give_resp_buf(resp);
                self.clients[idx].tx_buf = Some(tx_box);
                self.clients[idx].tx_pos = 0;
                self.update_client_epollout(fd, true);
                return;
            }
            self.give_resp_buf(resp);
            self.close_client(fd);
        } else {
            if let Some(tx_box) = self.clients[idx].tx_buf.as_mut() {
                tx_box.extend_from_slice(&resp);
            }
            self.give_resp_buf(resp);
            self.flush_client_tx(fd);
        }
    }

    fn flush_client_tx(&mut self, fd: c_int) {
        let idx = fd as usize;
        if idx >= self.clients.len() || !self.clients[idx].active {
            return;
        }
        loop {
            let Some(tx_box) = self.clients[idx].tx_buf.as_ref() else {
                break;
            };
            let pos = self.clients[idx].tx_pos as usize;
            if pos >= tx_box.len() {
                break;
            }
            let rem = &tx_box[pos..];
            let n = unsafe { send(fd, rem.as_ptr().cast(), rem.len(), MSG_NOSIGNAL) };
            if n > 0 {
                self.clients[idx].tx_pos += n as u32;
            } else {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(EAGAIN) {
                    self.update_client_epollout(fd, true);
                    return;
                }
                self.close_client(fd);
                return;
            }
        }
        let finished_tx = self.clients[idx].tx_buf.take();
        self.clients[idx].tx_pos = 0;
        self.give_box_buf(finished_tx);
        self.update_client_epollout(fd, false);

        if self.clients[idx].closing {
            self.close_client(fd);
        } else if self.clients[idx].rx_buf.is_some() {
            self.process_client_rx(fd);
        }
    }

    fn dispatch_db_queues(&mut self) {
        if !self.read_queue.is_empty() {
            for idx in 0..self.read_conns.len() {
                if self.read_queue.is_empty() {
                    break;
                }
                if self.read_conns[idx].start_read_pack(&mut self.read_queue) {
                    self.sync_pg_epollout(false, idx);
                }
            }
        }

        if !self.write_queue.is_empty() {
            for idx in 0..self.write_conns.len() {
                if self.write_queue.is_empty() {
                    break;
                }
                if self.write_conns[idx].start_write_batch(&mut self.write_queue) {
                    self.sync_pg_epollout(true, idx);
                }
            }
        }
    }

    fn handle_pg_event(&mut self, is_write: bool, conn_idx: usize) {
        let res = if !is_write {
            let conn = &mut self.read_conns[conn_idx];
            let _ = conn.try_flush_tx();
            conn.handle_readable(
                &mut self.gzip,
                &mut self.gzip_scratch,
                &mut self.read_queue,
                &mut self.write_queue,
                &mut self.completed_http,
                &mut self.resp_pool,
            )
        } else {
            let conn = &mut self.write_conns[conn_idx];
            let _ = conn.try_flush_tx();
            conn.handle_readable(
                &mut self.gzip,
                &mut self.gzip_scratch,
                &mut self.read_queue,
                &mut self.write_queue,
                &mut self.completed_http,
                &mut self.resp_pool,
            )
        };

        if res.is_err() {
            if !is_write {
                self.read_conns[conn_idx]
                    .fail_in_flight(&mut self.completed_http, &mut self.resp_pool);
            } else {
                self.write_conns[conn_idx]
                    .fail_in_flight(&mut self.completed_http, &mut self.resp_pool);
            }
            if let Ok(fresh) = PgConn::connect_and_prepare(&self.cfg.db, is_write) {
                if !is_write {
                    self.read_conns[conn_idx] = fresh;
                } else {
                    self.write_conns[conn_idx] = fresh;
                }
            }
        } else {
            self.sync_pg_epollout(is_write, conn_idx);
        }

        if !self.completed_http.is_empty() {
            let mut finished = std::mem::take(&mut self.completed_http);
            for item in finished.drain(..) {
                self.queue_client_response(
                    item.client_fd,
                    item.client_gen,
                    item.close_conn,
                    item.response_bytes,
                );
            }
            self.completed_http = finished;
        }
    }
}

#[cfg(target_os = "linux")]
fn run_worker(cfg: Arc<ServerConfig>) {
    use crate::sys::{
        EPOLL_CTL_ADD, EPOLL_CTL_DEL, EPOLLERR, EPOLLHUP, EPOLLIN, EPOLLOUT, EPOLLRDHUP, accept4,
        epoll_create1, epoll_ctl, epoll_event, epoll_wait, nanosleep, unshare_files,
    };

    unshare_files();
    let epfd = unsafe { epoll_create1(0) };
    assert!(epfd >= 0, "epoll_create1 failed");
    let mut worker = Worker::new(cfg, epfd);

    for (idx, &lfd) in worker.listen_fds.iter().enumerate() {
        let mut ev = epoll_event {
            events: EPOLLIN,
            u64_data: TAG_LISTEN | (idx as u64),
        };
        unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &mut ev) };
    }

    for (idx, conn) in worker.read_conns.iter().enumerate() {
        let mut ev = epoll_event {
            events: EPOLLIN,
            u64_data: TAG_PG_READ | (idx as u64),
        };
        unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, conn.fd, &mut ev) };
    }

    for (idx, conn) in worker.write_conns.iter().enumerate() {
        let mut ev = epoll_event {
            events: EPOLLIN,
            u64_data: TAG_PG_WRITE | (idx as u64),
        };
        unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, conn.fd, &mut ev) };
    }

    let mut events = vec![epoll_event::default(); 2048];

    loop {
        worker.dispatch_db_queues();

        let timeout_ms = if !worker.read_queue.is_empty() || !worker.write_queue.is_empty() {
            1
        } else {
            100
        };

        let n = unsafe { epoll_wait(epfd, events.as_mut_ptr(), events.len() as c_int, timeout_ms) };
        if n <= 0 {
            continue;
        }

        for i in 0..(n as usize) {
            let ev = events[i];
            let tag = ev.u64_data & TAG_MASK;
            let val = (ev.u64_data & VAL_MASK) as usize;

            match tag {
                TAG_LISTEN => {
                    let lfd = worker.listen_fds[val];
                    loop {
                        let cfd = unsafe {
                            accept4(
                                lfd,
                                std::ptr::null_mut(),
                                std::ptr::null_mut(),
                                crate::sys::O_NONBLOCK,
                            )
                        };
                        if cfd < 0 {
                            break;
                        }
                        set_tcp_nodelay(cfd);
                        worker.register_client(cfd);
                        let mut cev = epoll_event {
                            events: EPOLLIN | EPOLLRDHUP,
                            u64_data: TAG_CLIENT | (cfd as u64),
                        };
                        unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &mut cev) };
                    }
                }
                TAG_PG_READ => {
                    let old_fd = worker.read_conns[val].fd;
                    worker.handle_pg_event(false, val);
                    let new_fd = worker.read_conns[val].fd;
                    if new_fd != old_fd {
                        let mut pev = epoll_event {
                            events: EPOLLIN,
                            u64_data: TAG_PG_READ | (val as u64),
                        };
                        unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &mut pev) };
                    }
                }
                TAG_PG_WRITE => {
                    let old_fd = worker.write_conns[val].fd;
                    worker.handle_pg_event(true, val);
                    let new_fd = worker.write_conns[val].fd;
                    if new_fd != old_fd {
                        let mut pev = epoll_event {
                            events: EPOLLIN,
                            u64_data: TAG_PG_WRITE | (val as u64),
                        };
                        unsafe { epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &mut pev) };
                    }
                }
                TAG_CLIENT => {
                    let cfd = val as c_int;
                    if (ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0
                        && (ev.events & EPOLLIN) == 0
                    {
                        unsafe { epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, std::ptr::null_mut()) };
                        worker.close_client(cfd);
                        continue;
                    }
                    if (ev.events & EPOLLIN) != 0 {
                        worker.handle_client_readable(cfd);
                    }
                    if (ev.events & EPOLLOUT) != 0 {
                        worker.flush_client_tx(cfd);
                    }
                }
                _ => {}
            }
        }

        worker.dispatch_db_queues();

        if n < 32 {
            let pause = timespec {
                tv_sec: 0,
                tv_nsec: 2_000_000,
            };
            unsafe {
                let _ = nanosleep(&pause, std::ptr::null_mut());
            }
        }
    }
}

#[cfg(not(target_os = "linux"))]
fn run_worker(cfg: Arc<ServerConfig>) {
    use crate::sys::{POLLERR, POLLHUP, POLLIN, POLLOUT, accept, poll, pollfd};

    let mut worker = Worker::new(cfg, -1);
    let mut fds: Vec<pollfd> = Vec::with_capacity(1024);

    loop {
        worker.dispatch_db_queues();

        fds.clear();
        for &lfd in &worker.listen_fds {
            fds.push(pollfd {
                fd: lfd,
                events: POLLIN,
                revents: 0,
            });
        }
        for conn in &worker.read_conns {
            let mut ev = POLLIN;
            if conn.tx_pos < conn.tx_buf.len() {
                ev |= POLLOUT;
            }
            fds.push(pollfd {
                fd: conn.fd,
                events: ev,
                revents: 0,
            });
        }
        for conn in &worker.write_conns {
            let mut ev = POLLIN;
            if conn.tx_pos < conn.tx_buf.len() {
                ev |= POLLOUT;
            }
            fds.push(pollfd {
                fd: conn.fd,
                events: ev,
                revents: 0,
            });
        }
        let client_poll_start = fds.len();
        for (cfd, slot) in worker.clients.iter().enumerate() {
            if slot.active {
                let mut ev = POLLIN;
                if slot.tx_buf.as_ref().is_some_and(|b| (slot.tx_pos as usize) < b.len()) {
                    ev |= POLLOUT;
                }
                fds.push(pollfd {
                    fd: cfd as c_int,
                    events: ev,
                    revents: 0,
                });
            }
        }

        let rc = unsafe { poll(fds.as_mut_ptr(), fds.len() as u32, 2) };
        if rc <= 0 {
            continue;
        }

        let n_listen = worker.listen_fds.len();
        let n_read_pg = worker.read_conns.len();
        let n_write_pg = worker.write_conns.len();

        for i in 0..n_listen {
            if (fds[i].revents & POLLIN) != 0 {
                let lfd = fds[i].fd;
                loop {
                    let cfd = unsafe { accept(lfd, std::ptr::null_mut(), std::ptr::null_mut()) };
                    if cfd < 0 {
                        break;
                    }
                    set_nonblocking(cfd);
                    set_tcp_nodelay(cfd);
                    worker.register_client(cfd);
                }
            }
        }

        for i in 0..n_read_pg {
            let p = fds[n_listen + i];
            if p.revents != 0 {
                worker.handle_pg_event(false, i);
            }
        }

        for i in 0..n_write_pg {
            let p = fds[n_listen + n_read_pg + i];
            if p.revents != 0 {
                worker.handle_pg_event(true, i);
            }
        }

        for p in &fds[client_poll_start..] {
            if p.revents == 0 {
                continue;
            }
            if (p.revents & (POLLERR | POLLHUP)) != 0 && (p.revents & POLLIN) == 0 {
                worker.close_client(p.fd);
                continue;
            }
            if (p.revents & POLLIN) != 0 {
                worker.handle_client_readable(p.fd);
            }
            if (p.revents & POLLOUT) != 0 {
                worker.flush_client_tx(p.fd);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::json::{append_post_object_raw, format_pg_timestamptz_utc};

    #[test]
    fn test_pg_timestamptz_and_gzip_and_http() {
        // 2025-01-01T00:00:34.152Z relative to 2000-01-01T00:00:00Z
        // Days from 2000-01-01 to 2025-01-01:
        // 25 years * 365 + 7 leap days (2000, 2004, 2008, 2012, 2016, 2020, 2024) = 9132 days
        let secs = 9132i64 * 86400 + 34;
        let micros = secs * 1_000_000 + 152_000;
        let ts = format_pg_timestamptz_utc(micros);
        assert_eq!(std::str::from_utf8(&ts).unwrap(), "2025-01-01T00:00:34.152Z");

        let mut post_json = Vec::from(&b"{\"post\":"[..]);
        append_post_object_raw(
            &mut post_json,
            b"1",
            b"Fast tomorrow small bug big droplet. Weird cache really project rarely everything?",
            micros,
            b"happy_nebula_10241",
            b"11",
        );
        post_json.push(b'}');
        let golden = std::fs::read("../../test/golden/post-1.json").unwrap();
        assert_eq!(
            std::str::from_utf8(&post_json).unwrap(),
            std::str::from_utf8(&golden).unwrap().trim()
        );

        // Zero-header HTTP request parsing
        let raw = b"GET /feed HTTP/1.1\r\nAccept-Encoding: gzip\r\n\r\n";
        match parse_http_request(raw) {
            ParseState::Complete(req) => {
                assert_eq!(req.method, Method::Get);
                assert_eq!(req.path, b"/feed");
                assert!(req.accept_gzip);
            }
            _ => panic!("expected complete request"),
        }

        // Gzip compression non-empty and valid header/trailer
        let mut gz = FastGzip::new();
        let mut gz_out = Vec::new();
        gz.compress_into(&post_json, &mut gz_out);
        assert!(gz_out.len() > 18);
        assert_eq!(&gz_out[..3], &[0x1f, 0x8b, 0x08]);
        let isize_le = u32::from_le_bytes(gz_out[gz_out.len() - 4..].try_into().unwrap());
        assert_eq!(isize_le as usize, post_json.len());

        // Body validation
        let mut scratch = Vec::new();
        let trimmed = extract_and_validate_post_body(br#" { "body" : "  hello world \n " } "#, &mut scratch).unwrap();
        assert_eq!(trimmed, b"hello world");
    }
}

