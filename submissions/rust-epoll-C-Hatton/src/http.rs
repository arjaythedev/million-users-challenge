//! HTTP/1.1 request parsing, in place over the bytes received so far.

use memchr::memmem;

pub struct Req<'a> {
    pub method: &'a [u8],
    pub path: &'a [u8],
    pub auth: Option<&'a [u8]>,
    pub body: &'a [u8],
    pub gzip: bool,
    pub close: bool,
}

pub enum Parsed<'a> {
    /// A request and the number of bytes it used.
    Done(Req<'a>, usize),
    /// Need more bytes; `true` when the client waits for `100 Continue` before sending its body.
    Partial(bool),
    Bad,
}

/// Largest request head we wait for, and largest body we accept.
pub const MAX_HEAD: usize = 16 * 1024;
pub const MAX_BODY: usize = 64 * 1024;

#[inline]
fn contains_ci(h: &[u8], needle: &[u8]) -> bool {
    h.windows(needle.len()).any(|w| w.eq_ignore_ascii_case(needle))
}

pub fn parse(buf: &[u8]) -> Parsed<'_> {
    let Some(hend) = memmem::find(buf, b"\r\n\r\n") else {
        return if buf.len() > MAX_HEAD { Parsed::Bad } else { Parsed::Partial(false) };
    };
    let head = &buf[..hend];
    let mut lines = head.split(|&c| c == b'\n');
    let rl = lines.next().unwrap_or(b"");
    let rl = rl.strip_suffix(b"\r").unwrap_or(rl);
    let mut it = rl.split(|&c| c == b' ').filter(|s| !s.is_empty());
    let (Some(method), Some(target), Some(version)) = (it.next(), it.next(), it.next()) else {
        return Parsed::Bad;
    };
    let mut close = match version {
        b"HTTP/1.1" => false,
        b"HTTP/1.0" => true,
        _ => return Parsed::Bad,
    };
    let path = match memchr::memchr(b'?', target) {
        Some(q) => &target[..q],
        None => target,
    };
    let mut clen = 0usize;
    let mut auth = None;
    let mut gzip = false;
    let mut expect = false;
    for line in lines {
        let line = line.strip_suffix(b"\r").unwrap_or(line);
        let Some(colon) = memchr::memchr(b':', line) else { continue };
        let name = &line[..colon];
        let v = line[colon + 1..].trim_ascii();
        match name.len() {
            14 if name.eq_ignore_ascii_case(b"content-length") => {
                if v.is_empty() || v.len() > 9 || !v.iter().all(u8::is_ascii_digit) {
                    return Parsed::Bad;
                }
                clen = v.iter().fold(0, |a, &c| a * 10 + (c - b'0') as usize);
                if clen > MAX_BODY {
                    return Parsed::Bad;
                }
            }
            13 if name.eq_ignore_ascii_case(b"authorization") => auth = Some(v),
            15 if name.eq_ignore_ascii_case(b"accept-encoding") => gzip = contains_ci(v, b"gzip"),
            10 if name.eq_ignore_ascii_case(b"connection") => {
                if contains_ci(v, b"close") {
                    close = true;
                } else if contains_ci(v, b"keep-alive") {
                    close = false;
                }
            }
            17 if name.eq_ignore_ascii_case(b"transfer-encoding") => return Parsed::Bad,
            6 if name.eq_ignore_ascii_case(b"expect") => expect = contains_ci(v, b"100-continue"),
            _ => {}
        }
    }
    let total = hend + 4 + clen;
    if buf.len() < total {
        return Parsed::Partial(expect);
    }
    Parsed::Done(Req { method, path, auth, body: &buf[hend + 4..total], gzip, close }, total)
}
