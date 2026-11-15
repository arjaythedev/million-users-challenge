//! Minimal RFC 6455 server side, just enough for one local Chromium page: handshake, text/binary messages
//! (fragmented or not), ping/pong, close. No extensions, no TLS.
use std::io;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::tcp::{OwnedReadHalf, OwnedWriteHalf};
use tokio::net::TcpStream;

/// Room reserved at the start of every outgoing message buffer for the frame header (max 10 bytes).
pub const HEADROOM: usize = 10;
const MAX_MSG: usize = 256 << 20;

fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&(data.len() as u64 * 8).to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[4 * i..4 * i + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = h;
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, y) in h.iter_mut().zip([a, b, c, d, e]) {
            *x = x.wrapping_add(y);
        }
    }
    let mut out = [0u8; 20];
    for (i, x) in h.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&x.to_be_bytes());
    }
    out
}

fn b64(d: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in d.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        s.push(T[(n >> 18) as usize & 63] as char);
        s.push(T[(n >> 12) as usize & 63] as char);
        s.push(if c.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        s.push(if c.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    s
}

pub fn accept_key(key: &[u8]) -> String {
    let mut v = key.to_vec();
    v.extend_from_slice(b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    b64(&sha1(&v))
}

/// Reads the HTTP upgrade request and answers it. Returns the request path (with query) and any bytes that
/// followed the request headers.
pub async fn handshake(s: &mut TcpStream) -> io::Result<(String, Vec<u8>)> {
    let bad = |m: &str| io::Error::new(io::ErrorKind::InvalidData, m.to_string());
    let mut buf = Vec::with_capacity(1024);
    loop {
        // the upgrade request of our own page (Chromium) or of a relay socket: request line + headers up to the blank line
        if let Some(end) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            let head = std::str::from_utf8(&buf[..end]).map_err(|_| bad("bad upgrade request"))?;
            let mut lines = head.split("\r\n");
            let path = lines.next().and_then(|l| l.strip_prefix("GET ")).and_then(|l| l.split(' ').next()).ok_or_else(|| bad("bad upgrade request"))?.to_string();
            let key = lines
                .filter_map(|l| l.split_once(':'))
                .find(|(n, _)| n.eq_ignore_ascii_case("sec-websocket-key"))
                .map(|(_, v)| v.trim().as_bytes().to_vec())
                .ok_or_else(|| bad("no Sec-WebSocket-Key"))?;
            let resp = format!(
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
                accept_key(&key)
            );
            s.write_all(resp.as_bytes()).await?;
            return Ok((path, buf[end + 4..].to_vec()));
        }
        if buf.len() > 16384 {
            return Err(bad("upgrade request too large"));
        }
        let mut tmp = [0u8; 2048];
        let n = s.read(&mut tmp).await?;
        if n == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        buf.extend_from_slice(&tmp[..n]);
    }
}

pub enum Ev {
    Msg(Vec<u8>),
    Ping(Vec<u8>),
    Close,
}

pub struct Reader {
    rd: OwnedReadHalf,
    buf: Vec<u8>,
    pos: usize,
    msg: Vec<u8>,
}

impl Reader {
    pub fn new(rd: OwnedReadHalf, initial: Vec<u8>) -> Self {
        Reader { rd, buf: initial, pos: 0, msg: Vec::new() }
    }

    /// Next complete message / control event.
    pub async fn next(&mut self) -> io::Result<Ev> {
        loop {
            if let Some(ev) = self.try_frame()? {
                return Ok(ev);
            }
            if self.pos > 0 && self.pos == self.buf.len() {
                self.buf.clear();
                self.pos = 0;
            } else if self.pos > (1 << 20) {
                self.buf.drain(..self.pos);
                self.pos = 0;
            }
            self.buf.reserve(64 * 1024);
            if self.rd.read_buf(&mut self.buf).await? == 0 {
                return Ok(Ev::Close);
            }
        }
    }

    fn try_frame(&mut self) -> io::Result<Option<Ev>> {
        let b = &self.buf[self.pos..];
        if b.len() < 2 {
            return Ok(None);
        }
        let (fin, op) = (b[0] & 0x80 != 0, b[0] & 0x0f);
        let masked = b[1] & 0x80 != 0;
        let mut len = (b[1] & 0x7f) as usize;
        let mut off = 2;
        if len == 126 {
            if b.len() < 4 {
                return Ok(None);
            }
            len = u16::from_be_bytes([b[2], b[3]]) as usize;
            off = 4;
        } else if len == 127 {
            if b.len() < 10 {
                return Ok(None);
            }
            len = u64::from_be_bytes(b[2..10].try_into().unwrap()) as usize;
            off = 10;
        }
        if len > MAX_MSG {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "ws frame too large"));
        }
        let mask = if masked {
            if b.len() < off + 4 {
                return Ok(None);
            }
            let m = [b[off], b[off + 1], b[off + 2], b[off + 3]];
            off += 4;
            Some(m)
        } else {
            None
        };
        if b.len() < off + len {
            return Ok(None);
        }
        let payload = &b[off..off + len];
        let start = self.msg.len();
        self.msg.extend_from_slice(payload);
        if let Some(m) = mask {
            for (i, x) in self.msg[start..].iter_mut().enumerate() {
                *x ^= m[i & 3];
            }
        }
        self.pos += off + len;
        match op {
            0x0 | 0x1 | 0x2 => {
                if fin {
                    Ok(Some(Ev::Msg(std::mem::take(&mut self.msg))))
                } else {
                    self.try_frame()
                }
            }
            0x8 => Ok(Some(Ev::Close)),
            0x9 => {
                let p = self.msg.split_off(start); // control frames may interleave with a fragmented message
                Ok(Some(Ev::Ping(p)))
            }
            0xA => {
                self.msg.truncate(start);
                self.try_frame()
            }
            _ => Err(io::Error::new(io::ErrorKind::InvalidData, "bad ws opcode")),
        }
    }
}

/// Frames `buf` (whose first HEADROOM bytes are reserved, payload follows) as a single frame with opcode `op`
/// and returns the slice to write.
pub fn frame(buf: &mut [u8], op: u8) -> &[u8] {
    let n = buf.len() - HEADROOM;
    let start = if n < 126 {
        buf[HEADROOM - 2] = 0x80 | op;
        buf[HEADROOM - 1] = n as u8;
        HEADROOM - 2
    } else if n < 65536 {
        buf[HEADROOM - 4] = 0x80 | op;
        buf[HEADROOM - 3] = 126;
        buf[HEADROOM - 2..HEADROOM].copy_from_slice(&(n as u16).to_be_bytes());
        HEADROOM - 4
    } else {
        buf[0] = 0x80 | op;
        buf[1] = 127;
        buf[2..10].copy_from_slice(&(n as u64).to_be_bytes());
        0
    };
    &buf[start..]
}

/// A close frame (status code + short reason, which the page shows in its error text).
pub fn close_frame(code: u16, reason: &str) -> Vec<u8> {
    let mut r = reason.as_bytes();
    if r.len() > 120 {
        let mut n = 120;
        while !reason.is_char_boundary(n) {
            n -= 1;
        }
        r = &reason.as_bytes()[..n];
    }
    let mut v = vec![0x88, (2 + r.len()) as u8];
    v.extend_from_slice(&code.to_be_bytes());
    v.extend_from_slice(r);
    v
}

pub async fn write_frame(w: &mut OwnedWriteHalf, buf: &mut [u8], op: u8) -> io::Result<()> {
    w.write_all(frame(buf, op)).await
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rfc_accept() {
        assert_eq!(accept_key(b"dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    }
    #[test]
    fn framing() {
        let mut v = vec![0u8; HEADROOM];
        v.extend_from_slice(b"hi");
        assert_eq!(frame(&mut v, 1), &[0x81, 2, b'h', b'i']);
        let mut v = vec![0u8; HEADROOM];
        v.extend_from_slice(&[b'x'; 300]);
        let f = frame(&mut v, 1);
        assert_eq!(&f[..4], &[0x81, 126, 1, 44]);
        assert_eq!(f.len(), 304);
    }
}
