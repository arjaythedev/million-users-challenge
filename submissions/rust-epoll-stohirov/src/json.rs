#[derive(Default)]
pub struct Slot {
    pub kind: Kind,
    pub num: f64,
    pub str: Vec<u8>,
}

#[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    #[default]
    Absent,
    Str,
    Num,
    Null,
    Other,
}

pub struct Malformed;

const MAX_DEPTH: u32 = 256;

struct P<'a> {
    s: &'a [u8],
    i: usize,
}

impl<'a> P<'a> {
    #[inline]
    fn ws(&mut self) {
        while self.i < self.s.len() {
            match self.s[self.i] {
                b' ' | b'\t' | b'\n' | b'\r' => self.i += 1,
                _ => return,
            }
        }
    }

    #[inline]
    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn literal(&mut self, w: &[u8]) -> Result<(), Malformed> {
        if self.s.len() - self.i >= w.len() && &self.s[self.i..self.i + w.len()] == w {
            self.i += w.len();
            Ok(())
        } else {
            Err(Malformed)
        }
    }

    fn hex4(&mut self) -> Result<u32, Malformed> {
        if self.s.len() - self.i < 4 {
            return Err(Malformed);
        }
        let mut v = 0u32;
        for _ in 0..4 {
            let c = self.s[self.i];
            let d = match c {
                b'0'..=b'9' => c - b'0',
                b'a'..=b'f' => c - b'a' + 10,
                b'A'..=b'F' => c - b'A' + 10,
                _ => return Err(Malformed),
            };
            v = v << 4 | d as u32;
            self.i += 1;
        }
        Ok(v)
    }

    fn string(&mut self, mut out: Option<&mut Vec<u8>>) -> Result<(), Malformed> {
        self.i += 1;
        if let Some(o) = out.as_deref_mut() {
            o.clear();
        }
        loop {
            let start = self.i;
            while self.i < self.s.len() {
                let c = self.s[self.i];
                if c == b'"' || c == b'\\' || c < 0x20 {
                    break;
                }
                self.i += 1;
            }
            if let Some(o) = out.as_deref_mut() {
                o.extend_from_slice(&self.s[start..self.i]);
            }
            if self.i >= self.s.len() {
                return Err(Malformed);
            }
            let c = self.s[self.i];
            self.i += 1;
            if c == b'"' {
                return Ok(());
            }
            if c < 0x20 {
                return Err(Malformed);
            }
            let e = *self.s.get(self.i).ok_or(Malformed)?;
            self.i += 1;
            let ch: u32 = match e {
                b'"' => '"' as u32,
                b'\\' => '\\' as u32,
                b'/' => '/' as u32,
                b'b' => 8,
                b'f' => 12,
                b'n' => 10,
                b'r' => 13,
                b't' => 9,
                b'u' => {
                    let u = self.hex4()?;
                    if (0xD800..0xDC00).contains(&u) {
                        if self.s.len() - self.i >= 6 && self.s[self.i] == b'\\' && self.s[self.i + 1] == b'u' {
                            let save = self.i;
                            self.i += 2;
                            let lo = self.hex4()?;
                            if (0xDC00..0xE000).contains(&lo) {
                                0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)
                            } else {
                                self.i = save;
                                0xFFFD
                            }
                        } else {
                            0xFFFD
                        }
                    } else if (0xDC00..0xE000).contains(&u) {
                        0xFFFD
                    } else {
                        u
                    }
                }
                _ => return Err(Malformed),
            };
            if let Some(o) = out.as_deref_mut() {
                let mut tmp = [0u8; 4];
                o.extend_from_slice(char::from_u32(ch).unwrap_or('\u{FFFD}').encode_utf8(&mut tmp).as_bytes());
            }
        }
    }

    fn number(&mut self) -> Result<f64, Malformed> {
        let start = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.peek() {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.i += 1;
                }
            }
            _ => return Err(Malformed),
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(Malformed);
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(Malformed);
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        let t = unsafe { std::str::from_utf8_unchecked(&self.s[start..self.i]) };
        t.parse::<f64>().map_err(|_| Malformed)
    }

    fn skip(&mut self, depth: u32) -> Result<(), Malformed> {
        if depth > MAX_DEPTH {
            return Err(Malformed);
        }
        match self.peek().ok_or(Malformed)? {
            b'{' => {
                self.i += 1;
                self.ws();
                if self.peek() == Some(b'}') {
                    self.i += 1;
                    return Ok(());
                }
                loop {
                    self.ws();
                    if self.peek() != Some(b'"') {
                        return Err(Malformed);
                    }
                    self.string(None)?;
                    self.ws();
                    if self.peek() != Some(b':') {
                        return Err(Malformed);
                    }
                    self.i += 1;
                    self.ws();
                    self.skip(depth + 1)?;
                    self.ws();
                    match self.peek() {
                        Some(b',') => self.i += 1,
                        Some(b'}') => {
                            self.i += 1;
                            return Ok(());
                        }
                        _ => return Err(Malformed),
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
                    self.ws();
                    self.skip(depth + 1)?;
                    self.ws();
                    match self.peek() {
                        Some(b',') => self.i += 1,
                        Some(b']') => {
                            self.i += 1;
                            return Ok(());
                        }
                        _ => return Err(Malformed),
                    }
                }
            }
            b'"' => self.string(None),
            b't' => self.literal(b"true"),
            b'f' => self.literal(b"false"),
            b'n' => self.literal(b"null"),
            b'-' | b'0'..=b'9' => self.number().map(|_| ()),
            _ => Err(Malformed),
        }
    }
}

pub fn parse_top(s: &[u8], keys: &[&[u8]], slots: &mut [Slot], key: &mut Vec<u8>) -> Result<bool, Malformed> {
    for sl in slots.iter_mut() {
        sl.kind = Kind::Absent;
    }
    let mut p = P { s, i: 0 };
    p.ws();
    if p.peek() != Some(b'{') {
        p.skip(0)?;
        p.ws();
        return if p.i == s.len() { Ok(false) } else { Err(Malformed) };
    }
    p.i += 1;
    p.ws();
    if p.peek() == Some(b'}') {
        p.i += 1;
    } else {
        loop {
            p.ws();
            if p.peek() != Some(b'"') {
                return Err(Malformed);
            }
            p.string(Some(key))?;
            p.ws();
            if p.peek() != Some(b':') {
                return Err(Malformed);
            }
            p.i += 1;
            p.ws();
            match keys.iter().position(|k| *k == key.as_slice()) {
                None => p.skip(1)?,
                Some(k) => {
                    let sl = &mut slots[k];
                    match p.peek().ok_or(Malformed)? {
                        b'"' => {
                            p.string(Some(&mut sl.str))?;
                            sl.kind = Kind::Str;
                        }
                        b'-' | b'0'..=b'9' => {
                            sl.num = p.number()?;
                            sl.kind = Kind::Num;
                        }
                        b'n' => {
                            p.literal(b"null")?;
                            sl.kind = Kind::Null;
                        }
                        _ => {
                            p.skip(1)?;
                            sl.kind = Kind::Other;
                        }
                    }
                }
            }
            p.ws();
            match p.peek() {
                Some(b',') => p.i += 1,
                Some(b'}') => {
                    p.i += 1;
                    break;
                }
                _ => return Err(Malformed),
            }
        }
    }
    p.ws();
    if p.i != s.len() {
        return Err(Malformed);
    }
    Ok(true)
}

#[inline]
fn js_space(c: char) -> bool {
    matches!(c, '\t' | '\n' | '\u{0B}' | '\u{0C}' | '\r' | ' ' | '\u{A0}' | '\u{1680}' | '\u{2000}'..='\u{200A}'
        | '\u{2028}' | '\u{2029}' | '\u{202F}' | '\u{205F}' | '\u{3000}' | '\u{FEFF}')
}

pub fn trim(s: &str) -> &str {
    s.trim_matches(js_space)
}

pub const DIGITS2: &[u8; 200] = b"0001020304050607080910111213141516171819\
2021222324252627282930313233343536373839\
4041424344454647484950515253545556575859\
6061626364656667686970717273747576777879\
8081828384858687888990919293949596979899";

#[inline]
pub fn put_u64(o: &mut Vec<u8>, mut v: u64) {
    let mut tmp = [0u8; 20];
    let mut p = 20;
    while v >= 100 {
        let d = (v % 100) as usize * 2;
        v /= 100;
        p -= 2;
        tmp[p] = DIGITS2[d];
        tmp[p + 1] = DIGITS2[d + 1];
    }
    if v >= 10 {
        let d = v as usize * 2;
        p -= 2;
        tmp[p] = DIGITS2[d];
        tmp[p + 1] = DIGITS2[d + 1];
    } else {
        p -= 1;
        tmp[p] = b'0' + v as u8;
    }
    o.extend_from_slice(&tmp[p..]);
}

#[inline]
pub fn put_i64(o: &mut Vec<u8>, v: i64) {
    if v < 0 {
        o.push(b'-');
        put_u64(o, v.unsigned_abs());
    } else {
        put_u64(o, v as u64);
    }
}

const HEX: &[u8; 16] = b"0123456789abcdef";

static NEEDS_ESCAPE: [bool; 256] = {
    let mut t = [false; 256];
    let mut i = 0;
    while i < 0x20 {
        t[i] = true;
        i += 1;
    }
    t[b'"' as usize] = true;
    t[b'\\' as usize] = true;
    t
};

#[inline]
pub fn put_escaped(o: &mut Vec<u8>, s: &[u8]) {
    let mut run = 0;
    for (k, &c) in s.iter().enumerate() {
        if !NEEDS_ESCAPE[c as usize] {
            continue;
        }
        o.extend_from_slice(&s[run..k]);
        run = k + 1;
        match c {
            b'"' => o.extend_from_slice(b"\\\""),
            b'\\' => o.extend_from_slice(b"\\\\"),
            b'\n' => o.extend_from_slice(b"\\n"),
            b'\r' => o.extend_from_slice(b"\\r"),
            b'\t' => o.extend_from_slice(b"\\t"),
            8 => o.extend_from_slice(b"\\b"),
            12 => o.extend_from_slice(b"\\f"),
            _ => o.extend_from_slice(&[b'\\', b'u', b'0', b'0', HEX[(c >> 4) as usize], HEX[(c & 15) as usize]]),
        }
    }
    o.extend_from_slice(&s[run..]);
}

#[inline]
pub fn put_timestamp(o: &mut Vec<u8>, pg_micros: i64) {
    const PG_EPOCH_MILLIS: i64 = 946_684_800_000;
    const DAY: i64 = 86_400_000;
    let millis = pg_micros.div_euclid(1000) + PG_EPOCH_MILLIS;
    let days = millis.div_euclid(DAY);
    let ms = millis.rem_euclid(DAY) as u32;
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = (z - era * 146_097) as u32;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = (yoe as i64 + era * 400 + if month <= 2 { 1 } else { 0 }) as u32;
    let h = ms / 3_600_000;
    let mi = ms / 60_000 % 60;
    let s = ms / 1000 % 60;
    let f = ms % 1000;
    let d2 = |v: u32| [DIGITS2[v as usize * 2], DIGITS2[v as usize * 2 + 1]];
    let y = year % 10000;
    let (yh, yl) = (d2(y / 100), d2(y % 100));
    let (mo, da, hh, mm, ss) = (d2(month), d2(day), d2(h), d2(mi), d2(s));
    o.extend_from_slice(&[
        yh[0], yh[1], yl[0], yl[1], b'-', mo[0], mo[1], b'-', da[0], da[1], b'T', hh[0], hh[1], b':', mm[0], mm[1],
        b':', ss[0], ss[1], b'.', b'0' + (f / 100) as u8, b'0' + (f / 10 % 10) as u8, b'0' + (f % 10) as u8, b'Z',
    ]);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn top(s: &str) -> (Result<bool, ()>, Kind, String) {
        let mut slots = [Slot::default()];
        let mut key = Vec::new();
        let r = parse_top(s.as_bytes(), &[b"body"], &mut slots, &mut key).map_err(|_| ());
        (r, slots[0].kind, String::from_utf8_lossy(&slots[0].str).into_owned())
    }

    #[test]
    fn parse() {
        assert_eq!(top(r#"{"body":"x"}"#), (Ok(true), Kind::Str, "x".into()));
        assert_eq!(top(r#" { "a" : [1, {"b": null}], "body" : "a\"bé😀" } "#).2, "a\"bé😀");
        assert_eq!(top(r#"{"body":123}"#).1, Kind::Num);
        assert_eq!(top(r#"{}"#), (Ok(true), Kind::Absent, "".into()));
        assert!(top(r#"{bad json"#).0.is_err());
        assert!(top(r#"{"body":"x"} x"#).0.is_err());
        assert!(top(r#"{"body":"x",}"#).0.is_err());
        assert_eq!(top(r#"[1,2]"#).0, Ok(false));
        assert_eq!(top(r#"{"body":"a","body":"b"}"#).2, "b");
        assert_eq!(top(r#"{"body":"k"}"#).2, "k");
        assert!(top("").0.is_err());
        assert!(top(r#"{"body":01}"#).0.is_err());
    }

    #[test]
    fn writers() {
        let mut o = Vec::new();
        put_timestamp(&mut o, 0);
        assert_eq!(o, b"2000-01-01T00:00:00.000Z");
        o.clear();
        let secs = 1767225559i64 - 946_684_800;
        put_timestamp(&mut o, secs * 1_000_000 + 409_876);
        assert_eq!(std::str::from_utf8(&o).unwrap(), "2025-12-31T23:59:19.409Z");
        o.clear();
        put_timestamp(&mut o, -1);
        assert_eq!(std::str::from_utf8(&o).unwrap(), "1999-12-31T23:59:59.999Z");
        o.clear();
        for v in [0u64, 7, 10, 99, 100, 12345, u64::MAX] {
            o.clear();
            put_u64(&mut o, v);
            assert_eq!(o, v.to_string().as_bytes());
        }
        o.clear();
        put_escaped(&mut o, "a\"b\\c\nd\u{1}é".as_bytes());
        assert_eq!(std::str::from_utf8(&o).unwrap(), "a\\\"b\\\\c\\nd\\u0001é");
        assert_eq!(trim("\u{3000} x y \u{FEFF}"), "x y");
    }
}
