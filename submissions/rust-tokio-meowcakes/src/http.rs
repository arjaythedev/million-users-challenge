use std::future::poll_fn;
use std::io;
use std::net::SocketAddr;
use std::pin::Pin;
use std::task::{Context, Poll};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use socket2::{Domain, Protocol, Socket, Type};
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};
use tokio::net::{TcpListener, TcpStream};

use crate::db::{self, Query};
use crate::jwt::{self, AuthError, Claims};
use crate::{Worker, json};

const MAX_REQUEST: usize = 64 * 1024;

pub fn bind(addr: SocketAddr) -> io::Result<std::net::TcpListener> {
    let s = Socket::new(Domain::for_address(addr), Type::STREAM, Some(Protocol::TCP))?;
    s.set_reuse_address(true)?;
    s.set_reuse_port(true)?;
    s.set_tcp_nodelay(true)?;
    s.set_nonblocking(true)?;
    s.bind(&addr.into())?;
    s.listen(65535)?;
    quickack_off(std::os::fd::AsRawFd::as_raw_fd(&s));
    Ok(s.into())
}

pub fn quickack_off(fd: std::os::fd::RawFd) {
    let off: libc::c_int = 0;
    unsafe {
        libc::setsockopt(
            fd,
            libc::IPPROTO_TCP,
            libc::TCP_QUICKACK,
            (&off as *const libc::c_int).cast(),
            4,
        )
    };
}

#[derive(Clone, Copy, Default)]
pub struct Flags {
    pub gzip: bool,
    pub close: bool,
}

#[derive(Default)]
pub struct ReqState {
    pub continued: bool,
    pub closing: bool,
}

pub fn handle(
    w: &Worker,
    st: &mut ReqState,
    data: &[u8],
    out: &mut Vec<u8>,
) -> (usize, Option<(Flags, Query)>) {
    let mut used = 0;
    while used < data.len() {
        match parse(w, &data[used..]) {
            Parsed::Partial { expect_continue } => {
                if data.len() - used > MAX_REQUEST {
                    respond(
                        w,
                        out,
                        413,
                        json::TOO_LARGE,
                        Flags {
                            gzip: false,
                            close: true,
                        },
                    );
                    st.closing = true;
                    return (data.len(), None);
                }
                if expect_continue && !st.continued {
                    out.extend_from_slice(b"HTTP/1.1 100 Continue\r\n\r\n");
                    st.continued = true;
                }
                break;
            }
            Parsed::Bad(status, body) => {
                respond(
                    w,
                    out,
                    status,
                    body,
                    Flags {
                        gzip: false,
                        close: true,
                    },
                );
                st.closing = true;
                return (data.len(), None);
            }
            Parsed::Done { len, flags, action } => {
                used += len;
                st.continued = false;
                match action {
                    Action::Reply(status, body) => {
                        respond(w, out, status, body, flags);
                        if flags.close {
                            st.closing = true;
                            return (data.len(), None);
                        }
                    }
                    Action::Db(q) => return (used, Some((flags, q))),
                }
            }
        }
    }
    (used, None)
}

pub async fn accept_loop(listener: TcpListener, w: &'static Worker) {
    loop {
        match listener.accept().await {
            Ok((s, _)) => {
                tokio::task::spawn_local(serve(s, w));
            }
            Err(e)
                if matches!(
                    e.raw_os_error(),
                    Some(libc::EMFILE | libc::ENFILE | libc::ENOBUFS | libc::ENOMEM)
                ) =>
            {
                eprintln!("accept: {e}");
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
            Err(_) => {}
        }
    }
}

enum Next {
    Db(Flags, Query),
    Close,
}

struct Conn {
    stream: TcpStream,
    carry: Vec<u8>,
    carry_ready: bool,
    wbuf: Vec<u8>,
    req: ReqState,
}

async fn serve(stream: TcpStream, w: &'static Worker) {
    let mut c = Conn {
        stream,
        carry: Vec::new(),
        carry_ready: false,
        wbuf: Vec::new(),
        req: ReqState::default(),
    };
    loop {
        let (flags, q) = match poll_fn(|cx| c.poll_next(cx, w)).await {
            Next::Db(flags, q) => (flags, q),
            Next::Close => return,
        };
        let (status, body) = db::submit(w, q).await;
        {
            let mut out = w.out.borrow_mut();
            out.clear();
            respond(w, &mut out, status, &body, flags);
        }
        w.give_buf(body);
        if !poll_fn(|cx| Poll::Ready(c.write_out(cx, w))).await {
            return;
        }
        c.req.closing |= flags.close;
    }
}

impl Conn {
    fn poll_next(&mut self, cx: &mut Context, w: &'static Worker) -> Poll<Next> {
        loop {
            if !self.wbuf.is_empty() {
                match write_some(&mut self.stream, cx, &self.wbuf) {
                    Err(_) => return Poll::Ready(Next::Close),
                    Ok(n) if n < self.wbuf.len() => {
                        self.wbuf.drain(..n);
                        return Poll::Pending;
                    }
                    Ok(_) => self.wbuf = Vec::new(),
                }
            }
            if self.req.closing {
                return Poll::Ready(Next::Close);
            }

            if self.carry_ready {
                self.carry_ready = false;
                let carry = std::mem::take(&mut self.carry);
                let (used, next) = self.handle(cx, w, &carry);
                self.keep(&carry[used..]);
                match next {
                    Some(next) => return Poll::Ready(next),
                    None => continue,
                }
            }

            let rbuf = &mut *w.rbuf.borrow_mut();
            let mut rb = ReadBuf::new(rbuf);
            match Pin::new(&mut self.stream).poll_read(cx, &mut rb) {
                Poll::Pending => return Poll::Pending,
                Poll::Ready(Err(_)) => return Poll::Ready(Next::Close),
                Poll::Ready(Ok(())) => {}
            }
            let n = rb.filled().len();
            if n == 0 {
                return Poll::Ready(Next::Close);
            }
            let next = if self.carry.is_empty() {
                let (used, next) = self.handle(cx, w, &rbuf[..n]);
                self.keep(&rbuf[used..n]);
                next
            } else {
                let mut carry = std::mem::take(&mut self.carry);
                carry.extend_from_slice(&rbuf[..n]);
                let (used, next) = self.handle(cx, w, &carry);
                self.keep(&carry[used..]);
                next
            };
            if let Some(next) = next {
                return Poll::Ready(next);
            }
        }
    }

    fn keep(&mut self, rest: &[u8]) {
        if !rest.is_empty() && !self.req.closing {
            self.carry.extend_from_slice(rest);
        }
    }

    fn handle(
        &mut self,
        cx: &mut Context,
        w: &'static Worker,
        data: &[u8],
    ) -> (usize, Option<Next>) {
        let (used, db_request) = {
            let mut out = w.out.borrow_mut();
            out.clear();
            handle(w, &mut self.req, data, &mut out)
        };
        if !self.write_out(cx, w) {
            return (data.len(), Some(Next::Close));
        }
        match db_request {
            Some((flags, q)) => {
                self.carry_ready = used < data.len();
                (used, Some(Next::Db(flags, q)))
            }
            None => (used, None),
        }
    }

    fn write_out(&mut self, cx: &mut Context, w: &Worker) -> bool {
        let out = w.out.borrow();
        if out.is_empty() {
            return true;
        }
        if !self.wbuf.is_empty() {
            self.wbuf.extend_from_slice(&out);
            return true;
        }
        match write_some(&mut self.stream, cx, &out) {
            Ok(n) => {
                self.wbuf.extend_from_slice(&out[n..]);
                true
            }
            Err(_) => false,
        }
    }
}

pub fn write_some(s: &mut TcpStream, cx: &mut Context, data: &[u8]) -> io::Result<usize> {
    let mut done = 0;
    while done < data.len() {
        match Pin::new(&mut *s).poll_write(cx, &data[done..]) {
            Poll::Ready(Ok(0)) => return Err(io::ErrorKind::WriteZero.into()),
            Poll::Ready(Ok(n)) => done += n,
            Poll::Ready(Err(e)) => return Err(e),
            Poll::Pending => break,
        }
    }
    Ok(done)
}

enum Parsed {
    Partial {
        expect_continue: bool,
    },
    Bad(u16, &'static [u8]),
    Done {
        len: usize,
        flags: Flags,
        action: Action,
    },
}

enum Action {
    Reply(u16, &'static [u8]),
    Db(Query),
}

fn parse(w: &Worker, data: &[u8]) -> Parsed {
    let mut headers = [httparse::EMPTY_HEADER; 32];
    let mut req = httparse::Request::new(&mut headers);
    let head = match req.parse(data) {
        Ok(httparse::Status::Complete(n)) => n,
        Ok(httparse::Status::Partial) => {
            return Parsed::Partial {
                expect_continue: false,
            };
        }
        Err(_) => return Parsed::Bad(400, json::BAD_REQUEST),
    };
    let mut content_length = 0;
    let mut authorization = None;
    let mut gzip = false;
    let mut close = req.version == Some(0);
    let mut expect_continue = false;
    for h in req.headers.iter() {
        let name = h.name.as_bytes();
        if name.eq_ignore_ascii_case(b"content-length") {
            match std::str::from_utf8(h.value)
                .ok()
                .and_then(|v| v.trim().parse::<usize>().ok())
            {
                Some(n) => content_length = n,
                None => return Parsed::Bad(400, json::BAD_REQUEST),
            }
        } else if name.eq_ignore_ascii_case(b"authorization") {
            authorization = Some(h.value);
        } else if name.eq_ignore_ascii_case(b"accept-encoding") {
            gzip = accepts_gzip(h.value);
        } else if name.eq_ignore_ascii_case(b"connection") {
            close |= h
                .value
                .split(|&b| b == b',')
                .any(|t| t.trim_ascii().eq_ignore_ascii_case(b"close"));
        } else if name.eq_ignore_ascii_case(b"expect") {
            expect_continue = h.value.eq_ignore_ascii_case(b"100-continue");
        } else if name.eq_ignore_ascii_case(b"transfer-encoding") {
            return Parsed::Bad(400, json::BAD_REQUEST);
        }
    }
    if content_length > MAX_REQUEST {
        return Parsed::Bad(413, json::TOO_LARGE);
    }
    if data.len() - head < content_length {
        return Parsed::Partial { expect_continue };
    }
    let body = &data[head..head + content_length];
    let action = route(
        w,
        req.method.unwrap(),
        req.path.unwrap(),
        authorization,
        body,
    );
    Parsed::Done {
        len: head + content_length,
        flags: Flags { gzip, close },
        action,
    }
}

fn accepts_gzip(v: &[u8]) -> bool {
    v.split(|&b| b == b',').any(|coding| {
        let mut parts = coding.split(|&b| b == b';');
        let name = parts.next().unwrap_or_default().trim_ascii();
        (name.eq_ignore_ascii_case(b"gzip") || name == b"*")
            && !parts.any(|p| {
                let p = p.trim_ascii();
                p.strip_prefix(b"q=")
                    .is_some_and(|q| q.iter().all(|&c| c == b'0' || c == b'.'))
            })
    })
}

fn route(
    w: &Worker,
    method: &str,
    path: &str,
    authorization: Option<&[u8]>,
    body: &[u8],
) -> Action {
    let path = path.split_once('?').map_or(path, |(p, _)| p);
    match (method, path) {
        ("GET", "/feed") => return Action::Db(Query::Feed),
        ("GET", "/health") => return Action::Db(Query::Health),
        ("POST", "/posts") => {
            let claims = match auth(w, authorization) {
                Ok(c) => c,
                Err(reply) => return reply,
            };
            return match json::new_post(body) {
                Ok(body) => Action::Db(Query::Create {
                    user_id: claims.user_id,
                    username: claims.username,
                    body,
                }),
                Err(e) => Action::Reply(400, e),
            };
        }
        _ => {}
    }
    let Some(rest) = path.strip_prefix("/posts/") else {
        return Action::Reply(404, json::NOT_FOUND);
    };
    if method == "GET" && !rest.contains('/') {
        return match post_id(rest) {
            Ok(id) => Action::Db(Query::Post(id)),
            Err(reply) => reply,
        };
    }
    if method == "POST"
        && let Some(id) = rest.strip_suffix("/like")
        && !id.contains('/')
    {
        let claims = match auth(w, authorization) {
            Ok(c) => c,
            Err(reply) => return reply,
        };
        return match post_id(id) {
            Ok(post_id) => Action::Db(Query::Like {
                user_id: claims.user_id,
                post_id,
            }),
            Err(reply) => reply,
        };
    }
    Action::Reply(404, json::NOT_FOUND)
}

fn auth(w: &Worker, authorization: Option<&[u8]>) -> Result<Claims, Action> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0.0, |d| d.as_secs_f64());
    jwt::verify(&w.cfg.jwt, authorization, now).map_err(|e| match e {
        AuthError::Missing => Action::Reply(401, json::MISSING_TOKEN),
        AuthError::Invalid => Action::Reply(401, json::INVALID_TOKEN),
        AuthError::Payload => Action::Reply(401, json::INVALID_PAYLOAD),
    })
}

fn post_id(s: &str) -> Result<i64, Action> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return Err(Action::Reply(400, json::INVALID_ID));
    }
    match s.parse::<i64>() {
        Ok(0) => Err(Action::Reply(400, json::INVALID_ID)),
        Ok(id) => Ok(id),
        Err(_) => Err(Action::Reply(404, json::POST_NOT_FOUND)),
    }
}

pub fn respond(w: &Worker, out: &mut Vec<u8>, status: u16, body: &[u8], flags: Flags) {
    out.extend_from_slice(match status {
        200 => b"HTTP/1.1 200 OK\r\n".as_slice(),
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        413 => b"HTTP/1.1 413 Content Too Large\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    });
    out.extend_from_slice(b"Content-Type: application/json\r\n");
    if flags.close {
        out.extend_from_slice(b"Connection: close\r\n");
    }
    let mut n = itoa::Buffer::new();
    if flags.gzip && body.len() >= w.cfg.gzip_min {
        let mut gz = w.gzip.borrow_mut();
        let (encoder, buf) = &mut *gz;
        encoder.compress(body, buf);
        out.extend_from_slice(b"Content-Encoding: gzip\r\nContent-Length: ");
        out.extend_from_slice(n.format(buf.len()).as_bytes());
        out.extend_from_slice(b"\r\n\r\n");
        out.extend_from_slice(buf);
        return;
    }
    out.extend_from_slice(b"Content-Length: ");
    out.extend_from_slice(n.format(body.len()).as_bytes());
    out.extend_from_slice(b"\r\n\r\n");
    out.extend_from_slice(body);
}
