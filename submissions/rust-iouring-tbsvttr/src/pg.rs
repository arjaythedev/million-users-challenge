use std::collections::VecDeque;
use std::ffi::c_int;
use std::io;
use std::net::{Ipv4Addr, SocketAddrV4, ToSocketAddrs};

use crate::gzip::FastGzip;
use crate::http::write_http_response;
use crate::json::{append_post_object_raw, push_i64, push_u64};
use crate::jwt::{hmac_sha256_once, sha256_once};
use crate::sys::{
    AF_INET, EAGAIN, IPPROTO_TCP, MSG_NOSIGNAL, SOCK_STREAM, bind, close, connect, recv, send,
    set_nonblocking, set_tcp_nodelay, sockaddr_in, socket,
};

const OID_INT8: u32 = 20;
const OID_TEXT: u32 = 25;

pub const MAX_READ_PACK: usize = 64;
pub const MAX_WRITE_BATCH: usize = 64;

#[derive(Clone)]
pub struct DbConfig {
    pub addr: SocketAddrV4,
    pub user: String,
    pub pass: String,
    pub dbname: String,
}

impl DbConfig {
    pub fn parse(url: &str) -> Self {
        let rest = url
            .strip_prefix("postgres://")
            .or_else(|| url.strip_prefix("postgresql://"))
            .unwrap_or(url);
        let (authority, path_part) = rest.split_once('/').unwrap_or((rest, "feed"));
        let dbname = path_part.split('?').next().unwrap_or("feed").to_string();

        let (user_pass, host_port) = authority
            .rsplit_once('@')
            .unwrap_or(("postgres:postgres", authority));
        let (user, pass) = user_pass
            .split_once(':')
            .unwrap_or((user_pass, ""));

        let (host, port_str) = host_port
            .rsplit_once(':')
            .unwrap_or((host_port, "5432"));
        let port: u16 = port_str.parse().unwrap_or(5432);

        let ip = host
            .parse::<Ipv4Addr>()
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
            .unwrap_or(Ipv4Addr::LOCALHOST);

        Self {
            addr: SocketAddrV4::new(ip, port),
            user: user.to_string(),
            pass: pass.to_string(),
            dbname,
        }
    }
}

#[derive(Clone)]
pub enum ReadKind {
    Feed { accept_gzip: bool },
    GetPost { post_id: i64 },
    Health { uptime_s: u64 },
}

#[derive(Clone)]
pub struct ReadTask {
    pub client_fd: c_int,
    pub client_gen: u32,
    pub close_conn: bool,
    pub kind: ReadKind,
}

#[derive(Clone)]
pub enum WriteKind {
    CreatePost {
        user_id: i64,
        username_len: u8,
        username: [u8; 128],
        body: Vec<u8>,
    },
    LikePost {
        post_id: i64,
        user_id: i64,
    },
}

#[derive(Clone)]
pub struct WriteTask {
    pub client_fd: c_int,
    pub client_gen: u32,
    pub close_conn: bool,
    pub kind: WriteKind,
    pub out_id_len: u8,
    pub out_id_ascii: [u8; 24],
    pub out_created_micros: i64,
    pub out_inserted: bool,
}

pub struct CompletedHttp {
    pub client_fd: c_int,
    pub client_gen: u32,
    pub close_conn: bool,
    pub response_bytes: Vec<u8>,
}

#[allow(dead_code)]
pub struct PgConn {
    pub fd: c_int,
    pub is_write: bool,
    pub want_epollout: bool,
    pub tx_buf: Vec<u8>,
    pub tx_pos: usize,
    pub rx_buf: Vec<u8>,
    pub rx_pos: usize,
    pub active_reads: VecDeque<ReadTask>,
    pub active_writes: Vec<WriteTask>,
    pub write_cursor: usize,
    pub write_completed_count: usize,
    pub pack_had_error: bool,
    pub error_sqlstate: [u8; 5],
    pub current_json: Vec<u8>,
    pub row_count_in_cmd: usize,
}

impl PgConn {
    pub fn connect_and_prepare(cfg: &DbConfig, is_write: bool) -> io::Result<Self> {
        let fd = unsafe { socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        set_tcp_nodelay(fd);

        let sin = sockaddr_in {
            #[cfg(not(target_os = "linux"))]
            sin_len: std::mem::size_of::<sockaddr_in>() as u8,
            sin_family: AF_INET as _,
            sin_port: cfg.addr.port().to_be(),
            sin_addr: u32::from_ne_bytes(cfg.addr.ip().octets()),
            sin_zero: [0; 8],
        };
        let _ = bind;
        let rc = unsafe {
            connect(
                fd,
                (&sin as *const sockaddr_in).cast(),
                std::mem::size_of::<sockaddr_in>() as u32,
            )
        };
        if rc < 0 {
            let err = io::Error::last_os_error();
            unsafe { close(fd) };
            return Err(err);
        }

        let mut conn = Self {
            fd,
            is_write,
            want_epollout: false,
            tx_buf: Vec::with_capacity(65536),
            tx_pos: 0,
            rx_buf: Vec::with_capacity(65536),
            rx_pos: 0,
            active_reads: VecDeque::with_capacity(MAX_READ_PACK),
            active_writes: Vec::with_capacity(MAX_WRITE_BATCH),
            write_cursor: 0,
            write_completed_count: 0,
            pack_had_error: false,
            error_sqlstate: [0; 5],
            current_json: Vec::with_capacity(8192),
            row_count_in_cmd: 0,
        };

        conn.perform_startup_and_auth(cfg)?;
        conn.prepare_all_statements()?;
        set_nonblocking(fd);
        Ok(conn)
    }

    fn perform_startup_and_auth(&mut self, cfg: &DbConfig) -> io::Result<()> {
        let mut body = Vec::with_capacity(256);
        body.extend_from_slice(&196608u32.to_be_bytes()); // v3.0
        push_cstr(&mut body, "user");
        push_cstr(&mut body, &cfg.user);
        push_cstr(&mut body, "database");
        push_cstr(&mut body, &cfg.dbname);
        push_cstr(&mut body, "TimeZone");
        push_cstr(&mut body, "UTC");
        push_cstr(&mut body, "jit");
        push_cstr(&mut body, "off");
        push_cstr(&mut body, "synchronous_commit");
        push_cstr(&mut body, "on");
        push_cstr(&mut body, "enable_memoize");
        push_cstr(&mut body, "off");
        push_cstr(&mut body, "plan_cache_mode");
        push_cstr(&mut body, "force_generic_plan");
        body.push(0);

        let mut pkt = Vec::with_capacity(body.len() + 4);
        pkt.extend_from_slice(&((body.len() + 4) as u32).to_be_bytes());
        pkt.extend_from_slice(&body);
        self.write_all_blocking(&pkt)?;

        let mut scram_client_first_bare = String::new();
        let mut scram_nonce = String::new();

        loop {
            let (tag, payload) = self.read_msg_blocking()?;
            match tag {
                b'R' => {
                    if payload.len() < 4 {
                        return Err(io::Error::new(io::ErrorKind::InvalidData, "short auth msg"));
                    }
                    let auth_code =
                        u32::from_be_bytes([payload[0], payload[1], payload[2], payload[3]]);
                    match auth_code {
                        0 => {} // AuthenticationOk
                        3 => {
                            // CleartextPassword
                            let mut p = Vec::new();
                            p.push(b'p');
                            let len = (4 + cfg.pass.len() + 1) as u32;
                            p.extend_from_slice(&len.to_be_bytes());
                            p.extend_from_slice(cfg.pass.as_bytes());
                            p.push(0);
                            self.write_all_blocking(&p)?;
                        }
                        10 => {
                            // SASL (SCRAM-SHA-256)
                            scram_nonce = "rOprNGfwEbeRWgbNEkqO0123".to_string();
                            scram_client_first_bare = format!("n=,r={scram_nonce}");
                            let client_first_msg = format!("n,,{scram_client_first_bare}");
                            let mech = b"SCRAM-SHA-256\0";
                            let mut p = Vec::new();
                            p.push(b'p');
                            let len =
                                (4 + mech.len() + 4 + client_first_msg.len()) as u32;
                            p.extend_from_slice(&len.to_be_bytes());
                            p.extend_from_slice(mech);
                            p.extend_from_slice(&(client_first_msg.len() as u32).to_be_bytes());
                            p.extend_from_slice(client_first_msg.as_bytes());
                            self.write_all_blocking(&p)?;
                        }
                        11 => {
                            // SASLContinue
                            let server_first = std::str::from_utf8(&payload[4..])
                                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "utf8"))?;
                            let client_final = build_scram_client_final(
                                &cfg.pass,
                                &scram_client_first_bare,
                                &scram_nonce,
                                server_first,
                            )?;
                            let mut p = Vec::new();
                            p.push(b'p');
                            let len = (4 + client_final.len()) as u32;
                            p.extend_from_slice(&len.to_be_bytes());
                            p.extend_from_slice(client_final.as_bytes());
                            self.write_all_blocking(&p)?;
                        }
                        12 => {} // SASLFinal
                        _ => {
                            return Err(io::Error::new(
                                io::ErrorKind::Other,
                                "unsupported PG auth method",
                            ));
                        }
                    }
                }
                b'Z' => break,
                b'E' => {
                    return Err(io::Error::new(
                        io::ErrorKind::PermissionDenied,
                        "PG startup error",
                    ));
                }
                _ => {}
            }
        }
        Ok(())
    }

    fn prepare_all_statements(&mut self) -> io::Result<()> {
        let mut buf = Vec::with_capacity(1024);
        append_parse(
            &mut buf,
            "s0",
            "SELECT 1",
            &[],
        );
        append_parse(
            &mut buf,
            "s1",
            "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
            &[],
        );
        append_parse(
            &mut buf,
            "s2",
            "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1",
            &[OID_INT8],
        );
        append_parse(
            &mut buf,
            "s3",
            "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
            &[OID_INT8, OID_TEXT],
        );
        append_parse(
            &mut buf,
            "s4",
            "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
            &[OID_INT8, OID_INT8],
        );
        append_sync(&mut buf);
        self.write_all_blocking(&buf)?;

        loop {
            let (tag, _) = self.read_msg_blocking()?;
            if tag == b'Z' {
                break;
            }
            if tag == b'E' {
                return Err(io::Error::new(
                    io::ErrorKind::Other,
                    "failed to prepare statements",
                ));
            }
        }
        Ok(())
    }

    fn write_all_blocking(&self, mut data: &[u8]) -> io::Result<()> {
        while !data.is_empty() {
            let n = unsafe { send(self.fd, data.as_ptr().cast(), data.len(), MSG_NOSIGNAL) };
            if n <= 0 {
                return Err(io::Error::last_os_error());
            }
            data = &data[n as usize..];
        }
        Ok(())
    }

    fn read_msg_blocking(&mut self) -> io::Result<(u8, Vec<u8>)> {
        while self.rx_buf.len() - self.rx_pos < 5 {
            self.fill_rx_blocking()?;
        }
        let tag = self.rx_buf[self.rx_pos];
        let len = u32::from_be_bytes([
            self.rx_buf[self.rx_pos + 1],
            self.rx_buf[self.rx_pos + 2],
            self.rx_buf[self.rx_pos + 3],
            self.rx_buf[self.rx_pos + 4],
        ]) as usize;
        let total = 1 + len;
        while self.rx_buf.len() - self.rx_pos < total {
            self.fill_rx_blocking()?;
        }
        let payload = self.rx_buf[self.rx_pos + 5..self.rx_pos + total].to_vec();
        self.rx_pos += total;
        if self.rx_pos == self.rx_buf.len() {
            self.rx_buf.clear();
            self.rx_pos = 0;
        }
        Ok((tag, payload))
    }

    fn fill_rx_blocking(&mut self) -> io::Result<()> {
        let mut tmp = [0u8; 4096];
        let n = unsafe { recv(self.fd, tmp.as_mut_ptr().cast(), tmp.len(), 0) };
        if n <= 0 {
            return Err(io::Error::last_os_error());
        }
        self.rx_buf.extend_from_slice(&tmp[..n as usize]);
        Ok(())
    }

    #[inline]
    pub fn is_idle(&self) -> bool {
        self.fd >= 0
            && self.active_reads.is_empty()
            && self.active_writes.is_empty()
            && self.tx_pos >= self.tx_buf.len()
    }

    pub fn fail_in_flight(
        &mut self,
        completed: &mut Vec<CompletedHttp>,
        resp_pool: &mut Vec<Vec<u8>>,
    ) {
        while let Some(failed) = self.active_reads.pop_front() {
            let mut resp = resp_pool
                .pop()
                .unwrap_or_else(|| Vec::with_capacity(256));
            resp.clear();
            if matches!(failed.kind, ReadKind::Health { .. }) {
                write_http_response(
                    &mut resp,
                    503,
                    failed.close_conn,
                    false,
                    b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"query failed\"}",
                );
            } else {
                write_http_response(
                    &mut resp,
                    500,
                    failed.close_conn,
                    false,
                    b"{\"error\":\"internal server error\"}",
                );
            }
            completed.push(CompletedHttp {
                client_fd: failed.client_fd,
                client_gen: failed.client_gen,
                close_conn: failed.close_conn,
                response_bytes: resp,
            });
        }
        for failed in self.active_writes.drain(..) {
            let mut resp = resp_pool
                .pop()
                .unwrap_or_else(|| Vec::with_capacity(256));
            resp.clear();
            write_http_response(
                &mut resp,
                500,
                failed.close_conn,
                false,
                b"{\"error\":\"internal server error\"}",
            );
            completed.push(CompletedHttp {
                client_fd: failed.client_fd,
                client_gen: failed.client_gen,
                close_conn: failed.close_conn,
                response_bytes: resp,
            });
        }
        self.write_cursor = 0;
        self.write_completed_count = 0;
        self.pack_had_error = false;
    }

    pub fn start_read_pack(&mut self, queue: &mut VecDeque<ReadTask>) -> bool {
        if !self.is_idle() || queue.is_empty() {
            return false;
        }
        self.tx_buf.clear();
        self.tx_pos = 0;
        self.pack_had_error = false;
        self.row_count_in_cmd = 0;
        self.current_json.clear();

        let count = queue.len().min(MAX_READ_PACK);
        for _ in 0..count {
            let task = queue.pop_front().unwrap();
            match &task.kind {
                ReadKind::Feed { .. } => {
                    append_bind_feed(&mut self.tx_buf);
                    append_execute(&mut self.tx_buf);
                }
                ReadKind::GetPost { post_id } => {
                    append_bind_get_post(&mut self.tx_buf, *post_id);
                    append_execute(&mut self.tx_buf);
                }
                ReadKind::Health { .. } => {
                    append_bind_health(&mut self.tx_buf);
                    append_execute(&mut self.tx_buf);
                }
            }
            self.active_reads.push_back(task);
        }
        append_sync(&mut self.tx_buf);
        let _ = self.try_flush_tx();
        true
    }

    pub fn start_write_batch(&mut self, queue: &mut VecDeque<WriteTask>) -> bool {
        if !self.is_idle() || queue.is_empty() {
            return false;
        }
        self.tx_buf.clear();
        self.tx_pos = 0;
        self.write_cursor = 0;
        self.write_completed_count = 0;
        self.pack_had_error = false;
        self.error_sqlstate = [0; 5];

        let count = queue.len().min(MAX_WRITE_BATCH);
        for _ in 0..count {
            let task = queue.pop_front().unwrap();
            match &task.kind {
                WriteKind::CreatePost {
                    user_id,
                    body,
                    ..
                } => {
                    append_bind_insert_post(&mut self.tx_buf, *user_id, body);
                    append_execute(&mut self.tx_buf);
                }
                WriteKind::LikePost { post_id, user_id } => {
                    append_bind_like_post(&mut self.tx_buf, *user_id, *post_id);
                    append_execute(&mut self.tx_buf);
                }
            }
            self.active_writes.push(task);
        }
        append_sync(&mut self.tx_buf);
        let _ = self.try_flush_tx();
        true
    }

    pub fn try_flush_tx(&mut self) -> io::Result<bool> {
        while self.tx_pos < self.tx_buf.len() {
            let rem = &self.tx_buf[self.tx_pos..];
            let n = unsafe { send(self.fd, rem.as_ptr().cast(), rem.len(), MSG_NOSIGNAL) };
            if n > 0 {
                self.tx_pos += n as usize;
            } else {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(EAGAIN) {
                    return Ok(false);
                }
                return Err(err);
            }
        }
        self.tx_buf.clear();
        self.tx_pos = 0;
        Ok(true)
    }

    pub fn handle_readable(
        &mut self,
        gzip: &mut FastGzip,
        gzip_scratch: &mut Vec<u8>,
        requeue_reads: &mut VecDeque<ReadTask>,
        requeue_writes: &mut VecDeque<WriteTask>,
        completed: &mut Vec<CompletedHttp>,
        resp_pool: &mut Vec<Vec<u8>>,
    ) -> io::Result<()> {
        let mut tmp = [0u8; 32768];
        loop {
            let n = unsafe { recv(self.fd, tmp.as_mut_ptr().cast(), tmp.len(), 0) };
            if n > 0 {
                self.rx_buf.extend_from_slice(&tmp[..n as usize]);
                if (n as usize) < tmp.len() {
                    break;
                }
            } else if n == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "PG closed socket",
                ));
            } else {
                let err = io::Error::last_os_error();
                if err.raw_os_error() == Some(EAGAIN) {
                    break;
                }
                return Err(err);
            }
        }

        while self.rx_buf.len() - self.rx_pos >= 5 {
            let tag = self.rx_buf[self.rx_pos];
            let len = u32::from_be_bytes([
                self.rx_buf[self.rx_pos + 1],
                self.rx_buf[self.rx_pos + 2],
                self.rx_buf[self.rx_pos + 3],
                self.rx_buf[self.rx_pos + 4],
            ]) as usize;
            let total = 1 + len;
            if self.rx_buf.len() - self.rx_pos < total {
                break;
            }
            let p_start = self.rx_pos + 5;
            let p_end = self.rx_pos + total;
            self.rx_pos = p_end;

            if !self.is_write {
                self.process_read_msg(
                    tag,
                    p_start,
                    p_end,
                    gzip,
                    gzip_scratch,
                    requeue_reads,
                    completed,
                    resp_pool,
                );
            } else {
                self.process_write_msg(tag, p_start, p_end, requeue_writes, completed, resp_pool);
            }
        }

        if self.rx_pos > 0 {
            if self.rx_pos == self.rx_buf.len() {
                self.rx_buf.clear();
                self.rx_pos = 0;
            } else if self.rx_pos >= 32768 {
                self.rx_buf.drain(..self.rx_pos);
                self.rx_pos = 0;
            }
        }
        Ok(())
    }

    fn process_read_msg(
        &mut self,
        tag: u8,
        p_start: usize,
        p_end: usize,
        gzip: &mut FastGzip,
        gzip_scratch: &mut Vec<u8>,
        requeue_reads: &mut VecDeque<ReadTask>,
        completed: &mut Vec<CompletedHttp>,
        resp_pool: &mut Vec<Vec<u8>>,
    ) {
        match tag {
            b'2' => {
                // BindComplete
                self.row_count_in_cmd = 0;
                self.current_json.clear();
                if let Some(front) = self.active_reads.front() {
                    if matches!(front.kind, ReadKind::Feed { .. }) {
                        self.current_json.extend_from_slice(b"{\"posts\":[");
                    } else if matches!(front.kind, ReadKind::GetPost { .. }) {
                        self.current_json.extend_from_slice(b"{\"post\":");
                    }
                }
            }
            b'D' => {
                // DataRow
                let Some(front) = self.active_reads.front() else {
                    return;
                };
                match front.kind {
                    ReadKind::Feed { .. } => {
                        if self.row_count_in_cmd > 0 {
                            self.current_json.push(b',');
                        }
                        if let Some((id_s, body_s, created_us, author_s, likes_s)) =
                            parse_post_datarow(&self.rx_buf[p_start..p_end])
                        {
                            append_post_object_raw(
                                &mut self.current_json,
                                id_s,
                                body_s,
                                created_us,
                                author_s,
                                likes_s,
                            );
                            self.row_count_in_cmd += 1;
                        }
                    }
                    ReadKind::GetPost { .. } => {
                        if let Some((id_s, body_s, created_us, author_s, likes_s)) =
                            parse_post_datarow(&self.rx_buf[p_start..p_end])
                        {
                            append_post_object_raw(
                                &mut self.current_json,
                                id_s,
                                body_s,
                                created_us,
                                author_s,
                                likes_s,
                            );
                            self.current_json.push(b'}');
                            self.row_count_in_cmd += 1;
                        }
                    }
                    ReadKind::Health { .. } => {
                        self.row_count_in_cmd += 1;
                    }
                }
            }
            b'C' => {
                // CommandComplete -> finish current read immediately!
                let Some(task) = self.active_reads.pop_front() else {
                    return;
                };
                let mut resp = resp_pool
                    .pop()
                    .unwrap_or_else(|| Vec::with_capacity(4096));
                resp.clear();
                match task.kind {
                    ReadKind::Feed { accept_gzip } => {
                        self.current_json.extend_from_slice(b"]}");
                        if accept_gzip && self.current_json.len() > 1024 {
                            gzip_scratch.clear();
                            gzip.compress_into(&self.current_json, gzip_scratch);
                            write_http_response(
                                &mut resp,
                                200,
                                task.close_conn,
                                true,
                                gzip_scratch,
                            );
                        } else {
                            write_http_response(
                                &mut resp,
                                200,
                                task.close_conn,
                                false,
                                &self.current_json,
                            );
                        }
                    }
                    ReadKind::GetPost { .. } => {
                        if self.row_count_in_cmd > 0 {
                            write_http_response(
                                &mut resp,
                                200,
                                task.close_conn,
                                false,
                                &self.current_json,
                            );
                        } else {
                            write_http_response(
                                &mut resp,
                                404,
                                task.close_conn,
                                false,
                                b"{\"error\":\"post not found\"}",
                            );
                        }
                    }
                    ReadKind::Health { uptime_s } => {
                        self.current_json.clear();
                        self.current_json
                            .extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
                        push_u64(&mut self.current_json, uptime_s);
                        self.current_json.push(b'}');
                        write_http_response(
                            &mut resp,
                            200,
                            task.close_conn,
                            false,
                            &self.current_json,
                        );
                    }
                }
                completed.push(CompletedHttp {
                    client_fd: task.client_fd,
                    client_gen: task.client_gen,
                    close_conn: task.close_conn,
                    response_bytes: resp,
                });
            }
            b'E' => {
                self.pack_had_error = true;
                if let Some(failed) = self.active_reads.pop_front() {
                    let mut resp = resp_pool
                        .pop()
                        .unwrap_or_else(|| Vec::with_capacity(256));
                    resp.clear();
                    if matches!(failed.kind, ReadKind::Health { .. }) {
                        write_http_response(
                            &mut resp,
                            503,
                            failed.close_conn,
                            false,
                            b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"query failed\"}",
                        );
                    } else {
                        write_http_response(
                            &mut resp,
                            500,
                            failed.close_conn,
                            false,
                            b"{\"error\":\"internal server error\"}",
                        );
                    }
                    completed.push(CompletedHttp {
                        client_fd: failed.client_fd,
                        client_gen: failed.client_gen,
                        close_conn: failed.close_conn,
                        response_bytes: resp,
                    });
                }
            }
            b'Z' => {
                // ReadyForQuery: if an error occurred mid-pack, re-queue skipped reads
                while let Some(skipped) = self.active_reads.pop_front() {
                    requeue_reads.push_front(skipped);
                }
                self.pack_had_error = false;
            }
            _ => {}
        }
    }

    fn process_write_msg(
        &mut self,
        tag: u8,
        p_start: usize,
        p_end: usize,
        requeue_writes: &mut VecDeque<WriteTask>,
        completed: &mut Vec<CompletedHttp>,
        resp_pool: &mut Vec<Vec<u8>>,
    ) {
        match tag {
            b'D' => {
                if self.write_cursor >= self.active_writes.len() {
                    return;
                }
                let payload = &self.rx_buf[p_start..p_end];
                let task = &mut self.active_writes[self.write_cursor];
                match task.kind {
                    WriteKind::CreatePost { .. } => {
                        if let Some((id_s, created_us)) = parse_insert_post_datarow(payload) {
                            let n = id_s.len().min(24);
                            task.out_id_len = n as u8;
                            task.out_id_ascii[..n].copy_from_slice(&id_s[..n]);
                            task.out_created_micros = created_us;
                        }
                    }
                    WriteKind::LikePost { .. } => {}
                }
            }
            b'C' => {
                if self.write_cursor < self.active_writes.len() {
                    let payload = &self.rx_buf[p_start..p_end];
                    let task = &mut self.active_writes[self.write_cursor];
                    if matches!(task.kind, WriteKind::LikePost { .. }) {
                        task.out_inserted = payload.ends_with(b" 1\0");
                    }
                }
                self.write_cursor += 1;
                self.write_completed_count += 1;
            }
            b'E' => {
                self.pack_had_error = true;
                self.error_sqlstate = extract_sqlstate(&self.rx_buf[p_start..p_end]);
            }
            b'Z' => {
                if !self.pack_had_error {
                    /* Entire group-commit write batch committed durably! */
                    for task in self.active_writes.drain(..) {
                        self.current_json.clear();
                        let status = match &task.kind {
                            WriteKind::CreatePost {
                                username_len,
                                username,
                                body,
                                ..
                            } => {
                                self.current_json.extend_from_slice(b"{\"post\":");
                                append_post_object_raw(
                                    &mut self.current_json,
                                    &task.out_id_ascii[..task.out_id_len as usize],
                                    body,
                                    task.out_created_micros,
                                    &username[..*username_len as usize],
                                    b"0",
                                );
                                self.current_json.push(b'}');
                                201
                            }
                            WriteKind::LikePost { post_id, .. } => {
                                if task.out_inserted {
                                    self.current_json.extend_from_slice(
                                        b"{\"liked\":true,\"already_liked\":false,\"post_id\":",
                                    );
                                    push_i64(&mut self.current_json, *post_id);
                                    self.current_json.push(b'}');
                                    201
                                } else {
                                    self.current_json.extend_from_slice(
                                        b"{\"liked\":true,\"already_liked\":true,\"post_id\":",
                                    );
                                    push_i64(&mut self.current_json, *post_id);
                                    self.current_json.push(b'}');
                                    200
                                }
                            }
                        };
                        let mut resp = resp_pool
                            .pop()
                            .unwrap_or_else(|| Vec::with_capacity(1024));
                        resp.clear();
                        write_http_response(
                            &mut resp,
                            status,
                            task.close_conn,
                            false,
                            &self.current_json,
                        );
                        completed.push(CompletedHttp {
                            client_fd: task.client_fd,
                            client_gen: task.client_gen,
                            close_conn: task.close_conn,
                            response_bytes: resp,
                        });
                    }
                } else {
                    /* The item at write_completed_count failed; re-queue all other items in batch! */
                    let bad_idx = self.write_completed_count;
                    let sqlstate = self.error_sqlstate;
                    let mut retry_items = Vec::new();
                    for (idx, mut task) in self.active_writes.drain(..).enumerate() {
                        if idx == bad_idx {
                            let mut resp = resp_pool
                                .pop()
                                .unwrap_or_else(|| Vec::with_capacity(256));
                            resp.clear();
                            if &sqlstate == b"23503"
                                && matches!(task.kind, WriteKind::LikePost { .. })
                            {
                                write_http_response(
                                    &mut resp,
                                    404,
                                    task.close_conn,
                                    false,
                                    b"{\"error\":\"post not found\"}",
                                );
                            } else {
                                write_http_response(
                                    &mut resp,
                                    500,
                                    task.close_conn,
                                    false,
                                    b"{\"error\":\"internal server error\"}",
                                );
                            }
                            completed.push(CompletedHttp {
                                client_fd: task.client_fd,
                                client_gen: task.client_gen,
                                close_conn: task.close_conn,
                                response_bytes: resp,
                            });
                        } else {
                            task.out_id_len = 0;
                            task.out_inserted = false;
                            retry_items.push(task);
                        }
                    }
                    for item in retry_items.into_iter().rev() {
                        requeue_writes.push_front(item);
                    }
                }
                self.write_cursor = 0;
                self.write_completed_count = 0;
                self.pack_had_error = false;
            }
            _ => {}
        }
    }
}

impl Drop for PgConn {
    fn drop(&mut self) {
        if self.fd >= 0 {
            unsafe { close(self.fd) };
            self.fd = -1;
        }
    }
}

/* =========================================================================
 * Wire Protocol Builders & DataRow Parsers
 * ========================================================================= */

#[inline]
fn push_cstr(buf: &mut Vec<u8>, s: &str) {
    buf.extend_from_slice(s.as_bytes());
    buf.push(0);
}

fn append_parse(buf: &mut Vec<u8>, name: &str, sql: &str, param_oids: &[u32]) {
    buf.push(b'P');
    let len = 4 + name.len() + 1 + sql.len() + 1 + 2 + param_oids.len() * 4;
    buf.extend_from_slice(&(len as u32).to_be_bytes());
    push_cstr(buf, name);
    push_cstr(buf, sql);
    buf.extend_from_slice(&(param_oids.len() as u16).to_be_bytes());
    for &oid in param_oids {
        buf.extend_from_slice(&oid.to_be_bytes());
    }
}

#[inline]
fn append_sync(buf: &mut Vec<u8>) {
    buf.extend_from_slice(&[b'S', 0, 0, 0, 4]);
}

#[inline]
fn append_execute(buf: &mut Vec<u8>) {
    // 'E', len=9, portal="", max_rows=0
    buf.extend_from_slice(&[b'E', 0, 0, 0, 9, 0, 0, 0, 0, 0]);
}

#[inline]
fn append_bind_health(buf: &mut Vec<u8>) {
    // 'B', portal="", stmt="s0", 0 param formats, 0 params, 0 result formats
    buf.extend_from_slice(&[
        b'B', 0, 0, 0, 14, 0, b's', b'0', 0, 0, 0, 0, 0, 0, 0,
    ]);
}

#[inline]
fn append_bind_feed(buf: &mut Vec<u8>) {
    // 'B', portal="", stmt="s1", 0 param formats, 0 params, 5 result formats: [0, 1, 1, 1, 0]
    buf.extend_from_slice(&[
        b'B', 0, 0, 0, 24, 0, b's', b'1', 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0,
    ]);
}

#[inline]
fn append_bind_get_post(buf: &mut Vec<u8>, post_id: i64) {
    // 'B', portal="", stmt="s2", 1 param format [1=binary], 1 param (8 bytes), 5 result formats [0, 1, 1, 1, 0]
    buf.extend_from_slice(&[
        b'B', 0, 0, 0, 38, 0, b's', b'2', 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 8,
    ]);
    buf.extend_from_slice(&post_id.to_be_bytes());
    buf.extend_from_slice(&[0, 5, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0]);
}

#[inline]
fn append_bind_insert_post(buf: &mut Vec<u8>, user_id: i64, body: &[u8]) {
    // 'B', portal="", stmt="s3", 2 param formats [1, 1], 2 params, 2 result formats [0, 1]
    let total_len = 4 + 1 + 3 + 2 + 4 + 2 + (4 + 8) + (4 + body.len()) + 2 + 4;
    buf.push(b'B');
    buf.extend_from_slice(&(total_len as u32).to_be_bytes());
    buf.extend_from_slice(&[0, b's', b'3', 0, 0, 2, 0, 1, 0, 1, 0, 2]);
    buf.extend_from_slice(&8u32.to_be_bytes());
    buf.extend_from_slice(&user_id.to_be_bytes());
    buf.extend_from_slice(&(body.len() as u32).to_be_bytes());
    buf.extend_from_slice(body);
    buf.extend_from_slice(&[0, 2, 0, 0, 0, 1]);
}

#[inline]
fn append_bind_like_post(buf: &mut Vec<u8>, user_id: i64, post_id: i64) {
    // 'B', portal="", stmt="s4", 2 param formats [1, 1], 2 params (8B + 8B), 0 result formats
    buf.extend_from_slice(&[
        b'B', 0, 0, 0, 42, 0, b's', b'4', 0, 0, 2, 0, 1, 0, 1, 0, 2, 0, 0, 0, 8,
    ]);
    buf.extend_from_slice(&user_id.to_be_bytes());
    buf.extend_from_slice(&8u32.to_be_bytes());
    buf.extend_from_slice(&post_id.to_be_bytes());
    buf.extend_from_slice(&[0, 0]);
}

#[inline]
fn read_col<'a>(payload: &'a [u8], pos: &mut usize) -> Option<&'a [u8]> {
    if *pos + 4 > payload.len() {
        return None;
    }
    let len = i32::from_be_bytes([
        payload[*pos],
        payload[*pos + 1],
        payload[*pos + 2],
        payload[*pos + 3],
    ]);
    *pos += 4;
    if len < 0 {
        return Some(&[]);
    }
    let ulen = len as usize;
    if *pos + ulen > payload.len() {
        return None;
    }
    let slice = &payload[*pos..*pos + ulen];
    *pos += ulen;
    Some(slice)
}

#[inline]
fn parse_post_datarow(payload: &[u8]) -> Option<(&[u8], &[u8], i64, &[u8], &[u8])> {
    if payload.len() < 2 {
        return None;
    }
    let ncols = u16::from_be_bytes([payload[0], payload[1]]);
    if ncols != 5 {
        return None;
    }
    let mut pos = 2usize;
    let id_s = read_col(payload, &mut pos)?;
    let body_s = read_col(payload, &mut pos)?;
    let created_col = read_col(payload, &mut pos)?;
    if created_col.len() != 8 {
        return None;
    }
    let created_us = i64::from_be_bytes(created_col.try_into().ok()?);
    let author_s = read_col(payload, &mut pos)?;
    let likes_s = read_col(payload, &mut pos)?;
    Some((id_s, body_s, created_us, author_s, likes_s))
}

#[inline]
fn parse_insert_post_datarow(payload: &[u8]) -> Option<(&[u8], i64)> {
    if payload.len() < 2 {
        return None;
    }
    let ncols = u16::from_be_bytes([payload[0], payload[1]]);
    if ncols != 2 {
        return None;
    }
    let mut pos = 2usize;
    let id_s = read_col(payload, &mut pos)?;
    let created_col = read_col(payload, &mut pos)?;
    if created_col.len() != 8 {
        return None;
    }
    let created_us = i64::from_be_bytes(created_col.try_into().ok()?);
    Some((id_s, created_us))
}


fn extract_sqlstate(payload: &[u8]) -> [u8; 5] {
    let mut pos = 0usize;
    while pos < payload.len() {
        let code = payload[pos];
        if code == 0 {
            break;
        }
        pos += 1;
        let start = pos;
        while pos < payload.len() && payload[pos] != 0 {
            pos += 1;
        }
        if code == b'C' && pos - start == 5 {
            let mut out = [0u8; 5];
            out.copy_from_slice(&payload[start..pos]);
            return out;
        }
        pos += 1;
    }
    [0; 5]
}

/* =========================================================================
 * SCRAM-SHA-256 Authentication Helper (Zero External Crates)
 * ========================================================================= */

const B64_STD_CHARS: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

fn b64_std_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity(((data.len() + 2) / 3) * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0];
        let b1 = if chunk.len() > 1 { chunk[1] } else { 0 };
        let b2 = if chunk.len() > 2 { chunk[2] } else { 0 };
        out.push(B64_STD_CHARS[(b0 >> 2) as usize] as char);
        out.push(B64_STD_CHARS[(((b0 & 0x03) << 4) | (b1 >> 4)) as usize] as char);
        if chunk.len() > 1 {
            out.push(B64_STD_CHARS[(((b1 & 0x0f) << 2) | (b2 >> 6)) as usize] as char);
        } else {
            out.push('=');
        }
        if chunk.len() > 2 {
            out.push(B64_STD_CHARS[(b2 & 0x3f) as usize] as char);
        } else {
            out.push('=');
        }
    }
    out
}

fn b64_std_decode(s: &str) -> Option<Vec<u8>> {
    let bytes = s.trim_end_matches('=').as_bytes();
    let mut out = Vec::with_capacity((bytes.len() * 3) / 4);
    let mut buf = 0u32;
    let mut bits = 0u8;
    for &b in bytes {
        let v = match b {
            b'A'..=b'Z' => b - b'A',
            b'a'..=b'z' => b - b'a' + 26,
            b'0'..=b'9' => b - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            _ => return None,
        };
        buf = (buf << 6) | u32::from(v);
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((buf >> bits) as u8);
        }
    }
    Some(out)
}

fn build_scram_client_final(
    password: &str,
    client_first_bare: &str,
    client_nonce: &str,
    server_first: &str,
) -> io::Result<String> {
    let mut full_nonce = "";
    let mut salt_b64 = "";
    let mut iterations = 4096u32;

    for part in server_first.split(',') {
        if let Some(r) = part.strip_prefix("r=") {
            full_nonce = r;
        } else if let Some(s) = part.strip_prefix("s=") {
            salt_b64 = s;
        } else if let Some(i) = part.strip_prefix("i=") {
            iterations = i.parse().unwrap_or(4096);
        }
    }

    if !full_nonce.starts_with(client_nonce) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "SCRAM nonce mismatch",
        ));
    }
    let salt = b64_std_decode(salt_b64)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "bad SCRAM salt"))?;

    // PBKDF2-HMAC-SHA256
    let mut salt_plus_1 = salt;
    salt_plus_1.extend_from_slice(&1u32.to_be_bytes());
    let mut u = hmac_sha256_once(password.as_bytes(), &salt_plus_1);
    let mut salted_pass = u;
    for _ in 1..iterations {
        u = hmac_sha256_once(password.as_bytes(), &u);
        for i in 0..32 {
            salted_pass[i] ^= u[i];
        }
    }

    let client_key = hmac_sha256_once(&salted_pass, b"Client Key");
    let stored_key = sha256_once(&client_key);

    let client_final_without_proof = format!("c=biws,r={full_nonce}");
    let auth_message =
        format!("{client_first_bare},{server_first},{client_final_without_proof}");
    let client_sig = hmac_sha256_once(&stored_key, auth_message.as_bytes());

    let mut client_proof = [0u8; 32];
    for i in 0..32 {
        client_proof[i] = client_key[i] ^ client_sig[i];
    }
    let proof_b64 = b64_std_encode(&client_proof);
    Ok(format!("{client_final_without_proof},p={proof_b64}"))
}
