//! HTTP/1.1 keep-alive front. It does not parse HTTP: the connection buffer's first bytes go to the page as a *frame pass*
//! (gen/http.py) and the CSS says whether the head is complete, how long it is, the Content-Length, whether
//! to close, the method type, and where the request-target and the Authorization value are. This file only moves bytes: it slices the
//! ranges the CSS gave, builds the request fragment, queues it by the CSS-given class (Authorization value non-empty = heavy), and
//! writes the response the page produced.
use std::cell::RefCell;
use std::io::{self, ErrorKind::WouldBlock};
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::net::TcpStream;

use crate::fragment::{self, push_num};
use crate::hub::{Class, Hub, Kind, FRAME_BYTES};

const REQ_TIMEOUT: Duration = Duration::from_secs(45);
/// Frame passes over an incomplete head before we give up on the connection. Each costs the light page ms of CSS, so after
/// an incomplete pass we only ask the CSS again once a new LF has arrived (a head can't become complete before a line ends;
/// whether it is complete is still the CSS's call). That bounds the passes by the line count (≤ 64 headers + 2).
const MAX_PARTIAL: u32 = 66;
/// Upper bound on head + body whatever the page says (the CSS already rejects Content-Length > 48 KB).
const MAX_BODY: usize = 64 * 1024;

thread_local! {
    // scratch for reads: borrowed only between awaits, so idle connections own no buffer
    static SCRATCH: RefCell<Vec<u8>> = RefCell::new(vec![0; 32 * 1024]);
}

/// The two responses the front writes itself (the page is not involved): queue full, and the page died / timed out.
fn canned(status: u16, reason: &str, body: &str, close: bool) -> Vec<u8> {
    let mut o = Vec::with_capacity(160);
    o.extend_from_slice(b"HTTP/1.1 ");
    push_num(&mut o, status as u64);
    o.push(b' ');
    o.extend_from_slice(reason.as_bytes());
    o.extend_from_slice(b"\r\nContent-Type: application/json\r\n");
    if close {
        o.extend_from_slice(b"Connection: close\r\n");
    }
    o.extend_from_slice(b"Content-Length: ");
    push_num(&mut o, body.len() as u64);
    o.extend_from_slice(b"\r\n\r\n");
    o.extend_from_slice(body.as_bytes());
    o
}

fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64
}

/// What the CSS says about the head of the connection buffer (the `Z` counters of the frame pass, in this order).
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct Frame {
    pub ok: bool,
    pub bad: bool,
    /// head length (just after the blank line) and head + Content-Length
    pub hl: usize,
    pub used: usize,
    pub close: bool,
    pub cont: bool,
    /// 0 GET, 1 POST, 2 other
    pub mt: u8,
    /// request-target = buf[ps..pe], Authorization value = buf[as_..ae]
    pub ps: usize,
    pub pe: usize,
    pub as_: usize,
    pub ae: usize,
    /// phase 1 only: the head needs the chunk elements
    pub slow: bool,
}

impl Frame {
    /// 13 little endian u32: ok bad hl cl used close cont mt ps pe as ae slow
    pub fn parse(b: &[u8]) -> Option<Frame> {
        if b.len() != FRAME_BYTES {
            return None;
        }
        let v = |i: usize| u32::from_le_bytes(b[4 * i..4 * i + 4].try_into().unwrap());
        Some(Frame {
            ok: v(0) == 1,
            bad: v(1) == 1,
            hl: v(2) as usize,
            used: v(4) as usize,
            close: v(5) == 1,
            cont: v(6) == 1,
            mt: v(7).min(2) as u8,
            ps: v(8) as usize,
            pe: v(9) as usize,
            as_: v(10) as usize,
            ae: v(11) as usize,
            slow: v(12) == 1,
        })
    }
    /// The ranges must lie inside the buffer in the right order, whatever the page said.
    fn sane(&self, len: usize) -> bool {
        self.ps <= self.pe && self.pe <= self.hl && self.hl <= self.used && self.as_ <= self.ae && self.ae <= self.hl && self.used <= len
    }
}

enum Fail {
    Busy,
    Broken,
}

/// One frame pass over `win`: tier S (strings only) first, the chunk chain when the CSS asks for it (or the head is long anyway).
async fn frame(hub: &Hub, win: &[u8]) -> Result<Frame, Fail> {
    let k = &hub.cfg.k;
    let mut chunks = win.len() >= k.frame_big;
    loop {
        let rid = hub.alloc_rid();
        let mut html = Vec::with_capacity(win.len() * 4 + if chunks { win.len() * 12 } else { 0 } + 512);
        fragment::build_frame(&mut html, rid, win, chunks, k);
        let kind = if chunks { Kind::Frame2 } else { Kind::Frame1 };
        let Some((_ticket, rx)) = hub.submit(rid, html, Class::Light, kind) else { return Err(Fail::Busy) };
        let f = match tokio::time::timeout(REQ_TIMEOUT, rx).await {
            Ok(Ok(b)) => Frame::parse(&b).ok_or(Fail::Broken)?,
            _ => return Err(Fail::Broken),
        };
        if !f.slow || chunks {
            return Ok(f);
        }
        chunks = true;
    }
}

/// Waits until the socket is readable and appends what it has to `buf`. Err: closed / failed.
async fn read_more(s: &TcpStream, buf: &mut Vec<u8>) -> Result<(), ()> {
    loop {
        // poll_read_ready keeps the waker in the socket's own slot: no readiness future per idle connection
        if std::future::poll_fn(|cx| s.poll_read_ready(cx)).await.is_err() {
            return Err(());
        }
        let got = SCRATCH.with_borrow_mut(|inp| match s.try_read(inp) {
            Ok(0) => Err(()),
            Ok(n) => {
                buf.extend_from_slice(&inp[..n]);
                Ok(true)
            }
            Err(e) if e.kind() == WouldBlock => Ok(false),
            Err(_) => Err(()),
        })?;
        if got {
            return Ok(());
        }
    }
}

pub async fn conn(s: TcpStream, hub: Arc<Hub>) {
    let head_max = hub.cfg.k.head_max;
    let mut buf: Vec<u8> = Vec::new(); // unprocessed bytes: the next request's head (and body), partial or pipelined
    let mut partial = 0u32;
    loop {
        if buf.is_empty() && read_more(&s, &mut buf).await.is_err() {
            return;
        }
        // ---- frame pass over the head window
        let win = buf.len().min(head_max);
        // (boxed: the frame future is big, and an idle connection's task must stay small)
        let f = match Box::pin(frame(&hub, &buf[..win])).await {
            Ok(f) => f,
            Err(Fail::Busy) => {
                let _ = Box::pin(send_all(&s, canned(503, "Service Unavailable", "{\"error\":\"server busy\"}", true))).await;
                return;
            }
            Err(Fail::Broken) => {
                let _ = Box::pin(send_all(&s, canned(500, "Internal Server Error", "{\"error\":\"internal server error\"}", true))).await;
                return;
            }
        };
        if f.bad {
            return;
        }
        if !f.ok {
            // head incomplete: more bytes, unless the window is already as long as it gets
            partial += 1;
            if win >= head_max || partial > MAX_PARTIAL {
                return;
            }
            loop {
                let seen = buf.len();
                if read_more(&s, &mut buf).await.is_err() {
                    return;
                }
                if buf[seen..].contains(&b'\n') || buf.len() >= head_max {
                    break;
                }
            }
            continue;
        }
        partial = 0;
        if !f.sane(usize::MAX) || f.used > head_max + MAX_BODY {
            return;
        }
        // ---- the body: wait for the bytes the head announced
        let mut sent_continue = false;
        while buf.len() < f.used {
            if f.cont && !sent_continue && buf.len() == f.hl {
                sent_continue = true;
                if Box::pin(send_all(&s, b"HTTP/1.1 100 Continue\r\n\r\n".to_vec())).await.is_err() {
                    return;
                }
            }
            if read_more(&s, &mut buf).await.is_err() {
                return;
            }
        }
        // ---- the request chain
        let rid = hub.alloc_rid();
        let heavy = f.ae > f.as_;
        let mut html = Vec::with_capacity(1536 + (f.used - f.hl) * 12);
        let r = fragment::Req {
            mt: f.mt,
            path: &buf[f.ps..f.pe],
            auth: if heavy { Some(&buf[f.as_..f.ae]) } else { None },
            body: &buf[f.hl..f.used],
            close: f.close,
        };
        fragment::build(&mut html, rid, &r, now_ms(), &hub.cfg.k);
        buf.drain(..f.used);
        if buf.is_empty() && buf.capacity() > 4096 {
            buf = Vec::new();
        }
        // every await but the idle read is boxed, so an idle connection's task stays small
        let resp = Box::pin(handle(&hub, rid, html, heavy, f.close)).await;
        match write_now(&s, &resp) {
            Ok(0) => {}
            Ok(off) => {
                if Box::pin(send_all(&s, resp[off..].to_vec())).await.is_err() {
                    return;
                }
            }
            Err(_) => return,
        }
        if f.close {
            return;
        }
    }
}

/// One request through the page: the response comes back as complete raw HTTP bytes (stage 1, the Postgres round trip over
/// the page's own connection, stage 3 and the JSON templates all happen in the page).
async fn handle(hub: &Hub, rid: u32, html: Vec<u8>, heavy: bool, close: bool) -> Vec<u8> {
    let class = if heavy { Class::Heavy } else { Class::Light };
    // queue full: answer at once rather than let the backlog (and the wait) grow
    let Some((_ticket, rx)) = hub.submit(rid, html, class, Kind::Request) else {
        return canned(503, "Service Unavailable", "{\"error\":\"server busy\"}", close);
    };
    match tokio::time::timeout(REQ_TIMEOUT, rx).await {
        Ok(Ok(resp)) => resp,
        _ => canned(500, "Internal Server Error", "{\"error\":\"internal server error\"}", close),
    }
}

/// Writes as much as the socket takes right now; returns how many bytes went out.
fn write_now(s: &TcpStream, v: &[u8]) -> io::Result<usize> {
    let mut off = 0;
    while off < v.len() {
        match s.try_write(&v[off..]) {
            Ok(n) => off += n,
            Err(e) if e.kind() == WouldBlock => break,
            Err(e) => return Err(e),
        }
    }
    Ok(off)
}

async fn send_all(s: &TcpStream, v: Vec<u8>) -> io::Result<()> {
    let mut off = 0;
    while off < v.len() {
        std::future::poll_fn(|cx| s.poll_write_ready(cx)).await?;
        match s.try_write(&v[off..]) {
            Ok(n) => off += n,
            Err(e) if e.kind() == WouldBlock => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn frame_bytes() {
        let mut b = Vec::new();
        for v in [1u32, 0, 31, 0, 31, 1, 0, 1, 4, 9, 0, 0, 0] {
            b.extend_from_slice(&v.to_le_bytes());
        }
        let f = Frame::parse(&b).unwrap();
        assert!(f.ok && !f.bad && f.close && !f.cont && !f.slow);
        assert_eq!((f.hl, f.used, f.mt, f.ps, f.pe), (31, 31, 1, 4, 9));
        assert!(f.sane(31) && !f.sane(30));
        assert!(Frame::parse(&b[..51]).is_none());
        // ranges in the wrong order are insane
        let mut g = f;
        g.ps = 10;
        assert!(!g.sane(100));
    }
}
