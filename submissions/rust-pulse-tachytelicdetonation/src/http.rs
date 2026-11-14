pub const MAX_HEAD: usize = 16 * 1024;
pub const MAX_BODY: usize = 64 * 1024;

#[derive(Clone, Copy, PartialEq)]
pub enum Method {
    Get,
    Post,
    Other,
}

pub struct Req<'a> {
    pub method: Method,
    pub path: &'a [u8],
    pub auth: Option<&'a [u8]>,
    pub gzip: bool,
    pub close: bool,
    pub body: &'a [u8],
}

pub enum Parsed<'a> {
    Done(Req<'a>, usize),
    Partial,
    Bad,
}

#[inline]
fn find_nl(b: &[u8], from: usize) -> Option<usize> {
    b[from..].iter().position(|&c| c == b'\n').map(|p| p + from)
}

#[inline]
fn eq_ci(a: &[u8], lower: &[u8]) -> bool {
    a.len() == lower.len() && a.iter().zip(lower).all(|(x, y)| x.to_ascii_lowercase() == *y)
}

fn contains_ci(h: &[u8], needle: &[u8]) -> bool {
    h.windows(needle.len()).any(|w| eq_ci(w, needle))
}

fn trim(mut v: &[u8]) -> &[u8] {
    while let [b' ' | b'\t', rest @ ..] = v {
        v = rest;
    }
    while let [rest @ .., b' ' | b'\t' | b'\r'] = v {
        v = rest;
    }
    v
}

pub fn parse(b: &[u8]) -> Parsed<'_> {
    let Some(le) = find_nl(b, 0) else {
        return if b.len() > MAX_HEAD { Parsed::Bad } else { Parsed::Partial };
    };
    let line = trim(&b[..le]);
    let mut parts = line.split(|&c| c == b' ').filter(|s| !s.is_empty());
    let (Some(m), Some(path), Some(ver)) = (parts.next(), parts.next(), parts.next()) else {
        return Parsed::Bad;
    };
    let method = match m {
        b"GET" => Method::Get,
        b"POST" => Method::Post,
        _ => Method::Other,
    };
    let http10 = match ver {
        b"HTTP/1.1" => false,
        b"HTTP/1.0" => true,
        _ => return Parsed::Bad,
    };
    let mut auth = None;
    let mut gzip = false;
    let mut close = http10;
    let mut clen = 0usize;
    let mut pos = le + 1;
    loop {
        let Some(e) = find_nl(b, pos) else {
            return if b.len() > MAX_HEAD { Parsed::Bad } else { Parsed::Partial };
        };
        let l = &b[pos..e];
        let l = l.strip_suffix(b"\r").unwrap_or(l);
        pos = e + 1;
        if l.is_empty() {
            break;
        }
        let Some(colon) = l.iter().position(|&c| c == b':') else {
            return Parsed::Bad;
        };
        let name = &l[..colon];
        let val = trim(&l[colon + 1..]);
        match name.len() {
            13 if eq_ci(name, b"authorization") => auth = Some(val),
            14 if eq_ci(name, b"content-length") => {
                if val.is_empty() || val.len() > 9 || !val.iter().all(u8::is_ascii_digit) {
                    return Parsed::Bad;
                }
                clen = val.iter().fold(0usize, |a, &d| a * 10 + (d - b'0') as usize);
                if clen > MAX_BODY {
                    return Parsed::Bad;
                }
            }
            15 if eq_ci(name, b"accept-encoding") => gzip = contains_ci(val, b"gzip"),
            10 if eq_ci(name, b"connection") => {
                if contains_ci(val, b"close") {
                    close = true;
                } else if contains_ci(val, b"keep-alive") {
                    close = false;
                }
            }
            17 if eq_ci(name, b"transfer-encoding") => return Parsed::Bad,
            _ => {}
        }
    }
    if b.len() - pos < clen {
        return Parsed::Partial;
    }
    let body = &b[pos..pos + clen];
    Parsed::Done(Req { method, path, auth, gzip, close, body }, pos + clen)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn basic() {
        let r = b"GET /feed HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\nPOST /posts HTTP/1.1\r\nauthorization: Bearer t\r\nContent-Length: 3\r\n\r\nabcGET";
        let Parsed::Done(a, n) = parse(r) else { panic!() };
        assert!(a.method == Method::Get && a.path == b"/feed" && a.gzip && !a.close);
        let Parsed::Done(b, m) = parse(&r[n..]) else { panic!() };
        assert!(b.method == Method::Post && b.auth == Some(&b"Bearer t"[..]) && b.body == b"abc");
        assert!(matches!(parse(&r[n + m..]), Parsed::Partial));
        assert!(matches!(parse(b"GET / HTTP/1.1\r\nContent-Length: 5\r\n\r\nab"), Parsed::Partial));
        assert!(matches!(parse(b"garbage\r\n\r\n"), Parsed::Bad));
    }
}
