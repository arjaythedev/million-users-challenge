use crate::fmt::find_escape;

pub enum Val<'a> {
    Str { raw: &'a [u8], esc: bool },
    Num(&'a [u8]),
    Other,
}

struct P<'a> {
    s: &'a [u8],
    i: usize,
}

type R<T> = Result<T, ()>;

impl<'a> P<'a> {
    #[inline]
    fn ws(&mut self) {
        while self.i < self.s.len() {
            match self.s[self.i] {
                b' ' | b'\t' | b'\n' | b'\r' => self.i += 1,
                _ => break,
            }
        }
    }

    #[inline]
    fn peek(&self) -> R<u8> {
        self.s.get(self.i).copied().ok_or(())
    }

    fn string(&mut self) -> R<(&'a [u8], bool)> {
        self.i += 1;
        let start = self.i;
        let mut esc = false;
        loop {
            let j = self.i + find_escape(&self.s[self.i..]);
            if j >= self.s.len() {
                return Err(());
            }
            match self.s[j] {
                b'"' => {
                    self.i = j + 1;
                    return Ok((&self.s[start..j], esc));
                }
                b'\\' => {
                    esc = true;
                    let c = *self.s.get(j + 1).ok_or(())?;
                    match c {
                        b'"' | b'\\' | b'/' | b'b' | b'f' | b'n' | b'r' | b't' => self.i = j + 2,
                        b'u' => {
                            let h = self.s.get(j + 2..j + 6).ok_or(())?;
                            if !h.iter().all(|b| b.is_ascii_hexdigit()) {
                                return Err(());
                            }
                            self.i = j + 6;
                        }
                        _ => return Err(()),
                    }
                }
                _ => return Err(()),
            }
        }
    }

    fn digits(&mut self) -> usize {
        let st = self.i;
        while self.i < self.s.len() && self.s[self.i].is_ascii_digit() {
            self.i += 1;
        }
        self.i - st
    }

    fn number(&mut self) -> R<&'a [u8]> {
        let st = self.i;
        if self.s[self.i] == b'-' {
            self.i += 1;
        }
        match self.peek()? {
            b'0' => self.i += 1,
            b'1'..=b'9' => {
                self.digits();
            }
            _ => return Err(()),
        }
        if self.i < self.s.len() && self.s[self.i] == b'.' {
            self.i += 1;
            if self.digits() == 0 {
                return Err(());
            }
        }
        if self.i < self.s.len() && (self.s[self.i] | 0x20) == b'e' {
            self.i += 1;
            if self.i < self.s.len() && (self.s[self.i] == b'+' || self.s[self.i] == b'-') {
                self.i += 1;
            }
            if self.digits() == 0 {
                return Err(());
            }
        }
        Ok(&self.s[st..self.i])
    }

    fn lit(&mut self, w: &[u8]) -> R<()> {
        if self.s.get(self.i..self.i + w.len()) == Some(w) {
            self.i += w.len();
            Ok(())
        } else {
            Err(())
        }
    }

    fn value(&mut self, depth: u32) -> R<Val<'a>> {
        if depth > 64 {
            return Err(());
        }
        match self.peek()? {
            b'"' => {
                let (raw, esc) = self.string()?;
                Ok(Val::Str { raw, esc })
            }
            b'{' => {
                self.object(depth, &mut |_, _| {})?;
                Ok(Val::Other)
            }
            b'[' => {
                self.i += 1;
                self.ws();
                if self.peek()? == b']' {
                    self.i += 1;
                    return Ok(Val::Other);
                }
                loop {
                    self.ws();
                    self.value(depth + 1)?;
                    self.ws();
                    match self.peek()? {
                        b',' => self.i += 1,
                        b']' => {
                            self.i += 1;
                            return Ok(Val::Other);
                        }
                        _ => return Err(()),
                    }
                }
            }
            b't' => self.lit(b"true").map(|_| Val::Other),
            b'f' => self.lit(b"false").map(|_| Val::Other),
            b'n' => self.lit(b"null").map(|_| Val::Other),
            b'-' | b'0'..=b'9' => self.number().map(Val::Num),
            _ => Err(()),
        }
    }

    fn object(&mut self, depth: u32, f: &mut dyn FnMut(&[u8], Val<'a>)) -> R<()> {
        self.i += 1;
        self.ws();
        if self.peek()? == b'}' {
            self.i += 1;
            return Ok(());
        }
        let mut kbuf = Vec::new();
        loop {
            self.ws();
            if self.peek()? != b'"' {
                return Err(());
            }
            let (kraw, kesc) = self.string()?;
            self.ws();
            if self.peek()? != b':' {
                return Err(());
            }
            self.i += 1;
            self.ws();
            let v = self.value(depth + 1)?;
            if kesc {
                kbuf.clear();
                decode(kraw, &mut kbuf)?;
                f(&kbuf, v);
            } else {
                f(kraw, v);
            }
            self.ws();
            match self.peek()? {
                b',' => self.i += 1,
                b'}' => {
                    self.i += 1;
                    return Ok(());
                }
                _ => return Err(()),
            }
        }
    }
}

pub fn fields<'a>(s: &'a [u8], f: &mut dyn FnMut(&[u8], Val<'a>)) -> Result<bool, ()> {
    let mut p = P { s, i: 0 };
    p.ws();
    let is_obj = p.peek()? == b'{';
    if is_obj {
        p.object(0, f)?;
    } else {
        p.value(0)?;
    }
    p.ws();
    if p.i != s.len() {
        return Err(());
    }
    Ok(is_obj)
}

fn hex4(h: &[u8]) -> u32 {
    let mut v = 0u32;
    for &c in h {
        let d = match c {
            b'0'..=b'9' => c - b'0',
            b'a'..=b'f' => c - b'a' + 10,
            _ => c - b'A' + 10,
        };
        v = v * 16 + d as u32;
    }
    v
}

fn push_char(out: &mut Vec<u8>, c: u32) {
    let ch = char::from_u32(c).unwrap_or('\u{FFFD}');
    let mut b = [0u8; 4];
    out.extend_from_slice(ch.encode_utf8(&mut b).as_bytes());
}

pub fn decode(raw: &[u8], out: &mut Vec<u8>) -> R<()> {
    let mut i = 0;
    while i < raw.len() {
        let b = raw[i];
        if b != b'\\' {
            let j = raw[i..].iter().position(|&c| c == b'\\').map(|p| p + i).unwrap_or(raw.len());
            out.extend_from_slice(&raw[i..j]);
            i = j;
            continue;
        }
        let c = raw[i + 1];
        i += 2;
        match c {
            b'"' => out.push(b'"'),
            b'\\' => out.push(b'\\'),
            b'/' => out.push(b'/'),
            b'b' => out.push(8),
            b'f' => out.push(12),
            b'n' => out.push(b'\n'),
            b'r' => out.push(b'\r'),
            b't' => out.push(b'\t'),
            b'u' => {
                let mut cp = hex4(&raw[i..i + 4]);
                i += 4;
                if (0xD800..0xDC00).contains(&cp)
                    && raw.len() >= i + 6
                    && raw[i] == b'\\'
                    && raw[i + 1] == b'u'
                {
                    let lo = hex4(&raw[i + 2..i + 6]);
                    if (0xDC00..0xE000).contains(&lo) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                push_char(out, cp);
            }
            _ => return Err(()),
        }
    }
    Ok(())
}

pub fn js_trim(s: &str) -> &str {
    s.trim_matches(|c: char| {
        matches!(
            c,
            '\u{0009}'..='\u{000D}'
                | ' '
                | '\u{00A0}'
                | '\u{1680}'
                | '\u{2000}'..='\u{200A}'
                | '\u{2028}'
                | '\u{2029}'
                | '\u{202F}'
                | '\u{205F}'
                | '\u{3000}'
                | '\u{FEFF}'
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn body_of(s: &str) -> Result<Option<String>, ()> {
        let mut out = None;
        let mut buf = Vec::new();
        fields(s.as_bytes(), &mut |k, v| {
            if k == b"body" {
                out = match v {
                    Val::Str { raw, .. } => {
                        buf.clear();
                        decode(raw, &mut buf).unwrap();
                        Some(String::from_utf8(buf.clone()).unwrap())
                    }
                    _ => None,
                }
            }
        })?;
        Ok(out)
    }

    #[test]
    fn parse() {
        assert_eq!(body_of(r#"{"body":"x"}"#), Ok(Some("x".into())));
        assert_eq!(body_of(r#" { "a" : [1, {"b":null}], "body" : "q\"é😀" } "#), Ok(Some("q\"é😀".into())));
        assert_eq!(body_of(r#"{"body":123}"#), Ok(None));
        assert_eq!(body_of(r#"{}"#), Ok(None));
        assert_eq!(body_of(r#"[]"#), Ok(None));
        assert_eq!(body_of(r#"{"body":"k"}"#), Ok(Some("k".into())));
        assert_eq!(body_of(r#"{"body":"a","body":"b"}"#), Ok(Some("b".into())));
        assert!(body_of("{bad json").is_err());
        assert!(body_of(r#"{"body":"x"} x"#).is_err());
        assert!(body_of(r#"{"body":"x",}"#).is_err());
        assert!(body_of("{\"body\":\"a\nb\"}").is_err());
        assert!(body_of(r#"{"body":01}"#).is_err());
        assert!(body_of(r#"{"body":1.}"#).is_err());
        assert!(body_of("").is_err());
        assert_eq!(js_trim("\u{FEFF}\u{3000} x \u{00A0}\n"), "x");
        assert_eq!(js_trim("\u{0085}x"), "\u{0085}x");
    }
}
