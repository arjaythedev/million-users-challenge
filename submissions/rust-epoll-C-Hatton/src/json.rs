//! Strict JSON (RFC 8259) parsing for request bodies and JWT parts, plus the output helpers:
//! string escaping and Postgres binary timestamps formatted as UTC with milliseconds (truncated).

/// A top-level member value, decoded only when it is a string or a number.
pub enum Val<'a> {
    Str(String),
    Num(&'a [u8]),
    Other,
}

struct P<'a> {
    s: &'a [u8],
    i: usize,
}

const MAX_DEPTH: u32 = 512;

impl<'a> P<'a> {
    #[inline]
    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    #[inline]
    fn ws(&mut self) {
        while let Some(b' ' | b'\t' | b'\n' | b'\r') = self.peek() {
            self.i += 1;
        }
    }

    fn lit(&mut self, w: &[u8]) -> Result<(), ()> {
        if self.s[self.i..].starts_with(w) {
            self.i += w.len();
            Ok(())
        } else {
            Err(())
        }
    }

    fn digits(&mut self) -> Result<(), ()> {
        let st = self.i;
        while let Some(b'0'..=b'9') = self.peek() {
            self.i += 1;
        }
        if self.i == st { Err(()) } else { Ok(()) }
    }

    fn number(&mut self) -> Result<&'a [u8], ()> {
        let st = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.peek() {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => self.digits()?,
            _ => return Err(()),
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            self.digits()?;
        }
        if let Some(b'e' | b'E') = self.peek() {
            self.i += 1;
            if let Some(b'+' | b'-') = self.peek() {
                self.i += 1;
            }
            self.digits()?;
        }
        Ok(&self.s[st..self.i])
    }

    fn hex4(&mut self) -> Result<u32, ()> {
        if self.i + 4 > self.s.len() {
            return Err(());
        }
        let mut v = 0u32;
        for k in 0..4 {
            let d = match self.s[self.i + k] {
                c @ b'0'..=b'9' => c - b'0',
                c @ b'a'..=b'f' => c - b'a' + 10,
                c @ b'A'..=b'F' => c - b'A' + 10,
                _ => return Err(()),
            };
            v = v << 4 | d as u32;
        }
        self.i += 4;
        Ok(v)
    }

    /// At an opening quote. Decodes into `out` when given. The input is already known to be UTF-8.
    fn string(&mut self, mut out: Option<&mut Vec<u8>>) -> Result<(), ()> {
        self.i += 1;
        loop {
            let st = self.i;
            while let Some(c) = self.peek() {
                if c == b'"' || c == b'\\' || c < 0x20 {
                    break;
                }
                self.i += 1;
            }
            if let Some(o) = out.as_deref_mut() {
                o.extend_from_slice(&self.s[st..self.i]);
            }
            match self.peek() {
                Some(b'"') => {
                    self.i += 1;
                    return Ok(());
                }
                Some(b'\\') => {
                    self.i += 1;
                    let e = self.peek().ok_or(())?;
                    self.i += 1;
                    let ch: char = match e {
                        b'"' => '"',
                        b'\\' => '\\',
                        b'/' => '/',
                        b'b' => '\u{8}',
                        b'f' => '\u{c}',
                        b'n' => '\n',
                        b'r' => '\r',
                        b't' => '\t',
                        b'u' => {
                            let hi = self.hex4()?;
                            let cp = if (0xD800..0xDC00).contains(&hi)
                                && self.s[self.i..].starts_with(b"\\u")
                            {
                                let save = self.i;
                                self.i += 2;
                                let lo = self.hex4()?;
                                if (0xDC00..0xE000).contains(&lo) {
                                    0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00)
                                } else {
                                    self.i = save; // lone high surrogate; the next escape stands alone
                                    0xFFFD
                                }
                            } else {
                                hi
                            };
                            char::from_u32(cp).unwrap_or('\u{FFFD}')
                        }
                        _ => return Err(()),
                    };
                    if let Some(o) = out.as_deref_mut() {
                        let mut b = [0u8; 4];
                        o.extend_from_slice(ch.encode_utf8(&mut b).as_bytes());
                    }
                }
                _ => return Err(()), // control character or end of input
            }
        }
    }

    fn value(&mut self, depth: u32) -> Result<(), ()> {
        if depth > MAX_DEPTH {
            return Err(());
        }
        match self.peek().ok_or(())? {
            b'{' => {
                self.i += 1;
                self.ws();
                if self.peek() == Some(b'}') {
                    self.i += 1;
                    return Ok(());
                }
                loop {
                    if self.peek() != Some(b'"') {
                        return Err(());
                    }
                    self.string(None)?;
                    self.ws();
                    if self.peek() != Some(b':') {
                        return Err(());
                    }
                    self.i += 1;
                    self.ws();
                    self.value(depth + 1)?;
                    self.ws();
                    match self.peek() {
                        Some(b',') => {
                            self.i += 1;
                            self.ws();
                        }
                        Some(b'}') => {
                            self.i += 1;
                            return Ok(());
                        }
                        _ => return Err(()),
                    }
                }
            }
            b'[' => {
                self.i += 1;
                self.ws();
                if self.peek() == Some(b']') {
                    self.i += 1;
                    return Ok(());
                }
                loop {
                    self.value(depth + 1)?;
                    self.ws();
                    match self.peek() {
                        Some(b',') => {
                            self.i += 1;
                            self.ws();
                        }
                        Some(b']') => {
                            self.i += 1;
                            return Ok(());
                        }
                        _ => return Err(()),
                    }
                }
            }
            b'"' => self.string(None),
            b't' => self.lit(b"true"),
            b'f' => self.lit(b"false"),
            b'n' => self.lit(b"null"),
            b'-' | b'0'..=b'9' => self.number().map(|_| ()),
            _ => Err(()),
        }
    }
}

/// Parses a complete JSON text. When the top-level value is an object, `f` gets each member (later
/// duplicates win, as in JavaScript, because the caller simply overwrites). Returns whether it was an object.
pub fn parse_object<'a>(s: &'a [u8], mut f: impl FnMut(&[u8], Val<'a>)) -> Result<bool, ()> {
    if std::str::from_utf8(s).is_err() {
        return Err(());
    }
    let mut p = P { s, i: 0 };
    p.ws();
    let is_obj = p.peek() == Some(b'{');
    if is_obj {
        p.i += 1;
        p.ws();
        if p.peek() == Some(b'}') {
            p.i += 1;
        } else {
            let mut key = Vec::with_capacity(16);
            loop {
                if p.peek() != Some(b'"') {
                    return Err(());
                }
                key.clear();
                p.string(Some(&mut key))?;
                p.ws();
                if p.peek() != Some(b':') {
                    return Err(());
                }
                p.i += 1;
                p.ws();
                let v = match p.peek() {
                    Some(b'"') => {
                        let mut v = Vec::new();
                        p.string(Some(&mut v))?;
                        // valid: built from UTF-8 input runs and encoded chars
                        Val::Str(String::from_utf8(v).map_err(|_| ())?)
                    }
                    Some(b'-' | b'0'..=b'9') => Val::Num(p.number()?),
                    _ => {
                        p.value(1)?;
                        Val::Other
                    }
                };
                f(&key, v);
                p.ws();
                match p.peek() {
                    Some(b',') => {
                        p.i += 1;
                        p.ws();
                    }
                    Some(b'}') => {
                        p.i += 1;
                        break;
                    }
                    _ => return Err(()),
                }
            }
        }
    } else {
        p.value(0)?;
    }
    p.ws();
    if p.i != s.len() {
        return Err(());
    }
    Ok(is_obj)
}

/// Appends `s` as the inside of a JSON string.
#[inline]
pub fn esc(out: &mut Vec<u8>, s: &[u8]) {
    let mut st = 0;
    for (i, &b) in s.iter().enumerate() {
        if b == b'"' || b == b'\\' || b < 0x20 {
            out.extend_from_slice(&s[st..i]);
            match b {
                b'"' => out.extend_from_slice(b"\\\""),
                b'\\' => out.extend_from_slice(b"\\\\"),
                b'\n' => out.extend_from_slice(b"\\n"),
                b'\r' => out.extend_from_slice(b"\\r"),
                b'\t' => out.extend_from_slice(b"\\t"),
                _ => {
                    const H: &[u8; 16] = b"0123456789abcdef";
                    out.extend_from_slice(b"\\u00");
                    out.push(H[(b >> 4) as usize]);
                    out.push(H[(b & 15) as usize]);
                }
            }
            st = i + 1;
        }
    }
    out.extend_from_slice(&s[st..]);
}

#[inline]
fn d2(out: &mut Vec<u8>, v: u32) {
    out.push(b'0' + (v / 10) as u8);
    out.push(b'0' + (v % 10) as u8);
}

/// Postgres binary TIMESTAMPTZ (microseconds since 2000-01-01 UTC) as `YYYY-MM-DDTHH:MM:SS.mmmZ`, truncated.
pub fn ts(out: &mut Vec<u8>, pg_us: i64) {
    let unix_ms = (pg_us + 946_684_800_000_000).div_euclid(1000);
    let secs = unix_ms.div_euclid(1000);
    let ms = unix_ms.rem_euclid(1000) as u32;
    let days = secs.div_euclid(86400);
    let sod = secs.rem_euclid(86400) as u32;
    // civil_from_days (H. Hinnant)
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = (yoe + era * 400 + if m <= 2 { 1 } else { 0 }) as u32;
    d2(out, y / 100 % 100);
    d2(out, y % 100);
    out.push(b'-');
    d2(out, m);
    out.push(b'-');
    d2(out, d);
    out.push(b'T');
    d2(out, sod / 3600);
    out.push(b':');
    d2(out, sod / 60 % 60);
    out.push(b':');
    d2(out, sod % 60);
    out.push(b'.');
    out.push(b'0' + (ms / 100) as u8);
    d2(out, ms % 100);
    out.push(b'Z');
}

/// Appends a decimal integer.
#[inline]
pub fn int(out: &mut Vec<u8>, v: i64) {
    let mut b = [0u8; 20];
    let mut i = b.len();
    let neg = v < 0;
    let mut u = v.unsigned_abs();
    loop {
        i -= 1;
        b[i] = b'0' + (u % 10) as u8;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    if neg {
        out.push(b'-');
    }
    out.extend_from_slice(&b[i..]);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn timestamps() {
        let mut o = Vec::new();
        // 2025-12-31T23:59:19.409999Z
        let unix_us: i64 = 1_767_225_559_409_999;
        ts(&mut o, unix_us - 946_684_800_000_000);
        assert_eq!(std::str::from_utf8(&o).unwrap(), "2025-12-31T23:59:19.409Z");
        o.clear();
        ts(&mut o, -1); // 1999-12-31T23:59:59.999999
        assert_eq!(std::str::from_utf8(&o).unwrap(), "1999-12-31T23:59:59.999Z");
    }

    #[test]
    fn parsing() {
        let mut body = None;
        assert!(parse_object(br#" {"a":[1,{"b":null}],"body":"x\u00e9\ud83d\ude00\n"} "#, |k, v| {
            if k == b"body" {
                if let Val::Str(s) = v {
                    body = Some(s)
                }
            }
        })
        .unwrap());
        assert_eq!(body.unwrap(), "xé😀\n");
        for bad in [&b"{bad json"[..], b"", b"{\"a\":01}", b"[1,]", b"{\"a\":1}x", b"\"\x01\""] {
            assert!(parse_object(bad, |_, _| {}).is_err(), "{:?}", bad);
        }
        assert_eq!(parse_object(b"123", |_, _| {}), Ok(false));
    }
}
