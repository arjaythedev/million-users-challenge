pub const MAX_HEAD: usize = 16 * 1024;
pub const MAX_BODY: usize = 32 * 1024;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Method {
    Get,
    Post,
    Other,
}

pub struct Req {
    pub method: Method,
    pub path: (usize, usize),
    pub auth: Option<(usize, usize)>,
    pub body: (usize, usize),
    pub chunked: bool,
    pub total: usize,
    pub close: bool,
    pub gzip: bool,
}

pub enum Parsed {
    Incomplete,
    Bad(&'static [u8], &'static [u8]),
    Ok(Req),
}

pub const BAD_REQUEST: (&[u8], &[u8]) = (b"400 Bad Request", b"{\"error\":\"bad request\"}");
const TOO_LARGE: (&[u8], &[u8]) = (b"413 Content Too Large", b"{\"error\":\"request too large\"}");
const HEAD_TOO_LARGE: (&[u8], &[u8]) = (b"431 Request Header Fields Too Large", b"{\"error\":\"request too large\"}");
const NOT_IMPLEMENTED: (&[u8], &[u8]) = (b"501 Not Implemented", b"{\"error\":\"not implemented\"}");

#[inline]
fn bad(e: (&'static [u8], &'static [u8])) -> Parsed {
    Parsed::Bad(e.0, e.1)
}

#[inline]
pub fn memchr(x: u8, b: &[u8]) -> Option<usize> {
    const LO: u64 = 0x0101_0101_0101_0101;
    const HI: u64 = 0x8080_8080_8080_8080;
    let pat = LO * x as u64;
    let mut i = 0;
    while i + 8 <= b.len() {
        let v = u64::from_le_bytes(b[i..i + 8].try_into().unwrap()) ^ pat;
        let z = v.wrapping_sub(LO) & !v & HI;
        if z != 0 {
            return Some(i + (z.trailing_zeros() >> 3) as usize);
        }
        i += 8;
    }
    while i < b.len() {
        if b[i] == x {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[inline]
fn eq_lower(b: &[u8], lower: &[u8]) -> bool {
    b.len() == lower.len() && b.iter().zip(lower).all(|(&c, &l)| c | 0x20 == l)
}

fn contains_lower(b: &[u8], lower: &[u8]) -> bool {
    b.len() >= lower.len() && b.windows(lower.len()).any(|w| eq_lower(w, lower))
}

#[inline]
fn trim(b: &[u8], mut s: usize, mut e: usize) -> (usize, usize) {
    while s < e && (b[s] == b' ' || b[s] == b'\t') {
        s += 1;
    }
    while e > s && (b[e - 1] == b' ' || b[e - 1] == b'\t') {
        e -= 1;
    }
    (s, e)
}

pub fn parse(b: &[u8], chunk_out: &mut Vec<u8>) -> Parsed {
    let nl = match memchr(b'\n', b) {
        Some(n) => n,
        None => return if b.len() > MAX_HEAD { bad(HEAD_TOO_LARGE) } else { Parsed::Incomplete },
    };
    let le = if nl > 0 && b[nl - 1] == b'\r' { nl - 1 } else { nl };
    let line = &b[..le];
    let sp1 = match memchr(b' ', line) {
        Some(s) => s,
        None => return bad(BAD_REQUEST),
    };
    let sp2 = match memchr(b' ', &line[sp1 + 1..]) {
        Some(s) => sp1 + 1 + s,
        None => return bad(BAD_REQUEST),
    };
    let version = &line[sp2 + 1..];
    if version.len() != 8 || &version[..7] != b"HTTP/1." {
        return bad(BAD_REQUEST);
    }
    let http11 = version[7] == b'1';
    let method = match &line[..sp1] {
        b"GET" => Method::Get,
        b"POST" => Method::Post,
        _ => Method::Other,
    };
    let mut path = (sp1 + 1, sp2);
    if let Some(q) = memchr(b'?', &b[path.0..path.1]) {
        path.1 = path.0 + q;
    }

    let mut auth = None;
    let mut content_length: Option<usize> = None;
    let mut te: Option<(usize, usize)> = None;
    let (mut conn_close, mut conn_keep_alive, mut gzip) = (false, false, false);
    let mut p = nl + 1;
    let he;
    loop {
        let nl = match memchr(b'\n', &b[p..]) {
            Some(n) => p + n,
            None => return if b.len() > MAX_HEAD { bad(HEAD_TOO_LARGE) } else { Parsed::Incomplete },
        };
        let le = if nl > p && b[nl - 1] == b'\r' { nl - 1 } else { nl };
        if le == p {
            he = nl + 1;
            break;
        }
        if nl > MAX_HEAD {
            return bad(HEAD_TOO_LARGE);
        }
        if let Some(c) = memchr(b':', &b[p..le]) {
            let name = &b[p..p + c];
            let (vs, ve) = trim(b, p + c + 1, le);
            let v = &b[vs..ve];
            match name.len() {
                13 if eq_lower(name, b"authorization") => auth = Some((vs, ve)),
                14 if eq_lower(name, b"content-length") => {
                    if v.is_empty() {
                        return bad(BAD_REQUEST);
                    }
                    let mut n: usize = 0;
                    for &d in v {
                        if !d.is_ascii_digit() {
                            return bad(BAD_REQUEST);
                        }
                        n = n * 10 + (d - b'0') as usize;
                        if n > MAX_BODY {
                            return bad(TOO_LARGE);
                        }
                    }
                    content_length = Some(n);
                }
                10 if eq_lower(name, b"connection") => {
                    conn_close |= contains_lower(v, b"close");
                    conn_keep_alive |= contains_lower(v, b"keep-alive");
                }
                15 if eq_lower(name, b"accept-encoding") => gzip = contains_lower(v, b"gzip"),
                17 if eq_lower(name, b"transfer-encoding") => {
                    if te.is_some() {
                        return bad(BAD_REQUEST);
                    }
                    te = Some((vs, ve));
                }
                _ => {}
            }
        }
        p = nl + 1;
    }

    let close = if http11 { conn_close } else { !conn_keep_alive };
    if let Some((ts, te_)) = te {
        if !http11 || content_length.is_some() {
            return bad(BAD_REQUEST);
        }
        let v = &b[ts..te_];
        if !eq_lower(v, b"chunked") {
            let last = v.len() > 7 && eq_lower(&v[v.len() - 7..], b"chunked");
            return bad(if last { NOT_IMPLEMENTED } else { BAD_REQUEST });
        }
        return match dechunk(b, he, chunk_out) {
            Chunked::Done(total) => Parsed::Ok(Req { method, path, auth, body: (0, 0), chunked: true, total, close, gzip }),
            Chunked::Incomplete => Parsed::Incomplete,
            Chunked::Malformed => bad(BAD_REQUEST),
            Chunked::TooLarge => bad(TOO_LARGE),
        };
    }
    let len = content_length.unwrap_or(0);
    let total = he + len;
    if b.len() < total {
        return Parsed::Incomplete;
    }
    Parsed::Ok(Req { method, path, auth, body: (he, total), chunked: false, total, close, gzip })
}

enum Chunked {
    Done(usize),
    Incomplete,
    Malformed,
    TooLarge,
}

fn dechunk(b: &[u8], from: usize, out: &mut Vec<u8>) -> Chunked {
    out.clear();
    let mut k = from;
    loop {
        let start = k;
        let mut size: usize = 0;
        while k < b.len() {
            let d = match b[k] {
                c @ b'0'..=b'9' => c - b'0',
                c @ b'a'..=b'f' => c - b'a' + 10,
                c @ b'A'..=b'F' => c - b'A' + 10,
                _ => break,
            };
            size = size * 16 + d as usize;
            if out.len() + size > MAX_BODY {
                return Chunked::TooLarge;
            }
            k += 1;
        }
        if k == b.len() {
            return Chunked::Incomplete;
        }
        if k == start || !matches!(b[k], b'\r' | b';' | b' ' | b'\t') {
            return Chunked::Malformed;
        }
        let eol = match memchr(b'\r', &b[k..]) {
            Some(e) => k + e,
            None => return Chunked::Incomplete,
        };
        if eol + 1 >= b.len() {
            return Chunked::Incomplete;
        }
        if b[eol + 1] != b'\n' {
            return Chunked::Malformed;
        }
        k = eol + 2;
        if size == 0 {
            break;
        }
        if b.len() - k < size + 2 {
            return Chunked::Incomplete;
        }
        if b[k + size] != b'\r' || b[k + size + 1] != b'\n' {
            return Chunked::Malformed;
        }
        out.extend_from_slice(&b[k..k + size]);
        k += size + 2;
    }
    loop {
        let eol = match memchr(b'\r', &b[k..]) {
            Some(e) => k + e,
            None => return Chunked::Incomplete,
        };
        if eol + 1 >= b.len() {
            return Chunked::Incomplete;
        }
        if b[eol + 1] != b'\n' {
            return Chunked::Malformed;
        }
        if eol == k {
            return Chunked::Done(eol + 2);
        }
        k = eol + 2;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requests() {
        let mut tmp = Vec::new();
        let r = b"GET /feed?x=1 HTTP/1.1\r\nHost: a\r\nAccept-Encoding: gzip, deflate\r\n\r\nGET";
        match parse(r, &mut tmp) {
            Parsed::Ok(q) => {
                assert!(q.method == Method::Get);
                assert_eq!(&r[q.path.0..q.path.1], b"/feed");
                assert!(q.gzip && !q.close);
                assert_eq!(q.total, r.len() - 3);
            }
            _ => panic!(),
        }
        let r = b"POST /posts HTTP/1.1\r\nauthorization:  Bearer x \r\nContent-Length: 4\r\nConnection: close\r\n\r\nabcd";
        match parse(r, &mut tmp) {
            Parsed::Ok(q) => {
                let a = q.auth.unwrap();
                assert_eq!(&r[a.0..a.1], b"Bearer x");
                assert_eq!(&r[q.body.0..q.body.1], b"abcd");
                assert!(q.close);
            }
            _ => panic!(),
        }
        assert!(matches!(parse(b"POST /posts HTTP/1.1\r\nContent-Length: 4\r\n\r\nab", &mut tmp), Parsed::Incomplete));
        assert!(matches!(parse(b"GET / HTTP/1.1\r\nHost", &mut tmp), Parsed::Incomplete));
        assert!(matches!(parse(b"garbage\r\n\r\n", &mut tmp), Parsed::Bad(..)));
        let r = b"POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2;x\r\nde\r\n0\r\n\r\nNEXT";
        match parse(r, &mut tmp) {
            Parsed::Ok(q) => {
                assert!(q.chunked);
                assert_eq!(tmp, b"abcde");
                assert_eq!(q.total, r.len() - 4);
            }
            _ => panic!(),
        }
        match parse(b"GET / HTTP/1.0\r\n\r\n", &mut tmp) {
            Parsed::Ok(q) => assert!(q.close),
            _ => panic!(),
        }
        assert_eq!(memchr(b'x', b"0123456789abcdefx"), Some(16));
        assert_eq!(memchr(b'x', b"0123x"), Some(4));
        assert_eq!(memchr(b'x', b"0123"), None);
    }
}
