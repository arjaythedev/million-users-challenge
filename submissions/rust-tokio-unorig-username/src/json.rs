//! Strict RFC 8259 validation with just enough decoding for the JWT and request-body fields we read.

struct J<'a> {
    s: &'a [u8],
    p: usize,
}

impl J<'_> {
    fn peek(&self) -> Option<u8> {
        self.s.get(self.p).copied()
    }
    fn ws(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.p += 1;
        }
    }
    fn lit(&mut self, l: &[u8]) -> bool {
        let ok = self.s[self.p..].starts_with(l);
        if ok {
            self.p += l.len();
        }
        ok
    }
    fn digits(&mut self) -> usize {
        let st = self.p;
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.p += 1;
        }
        self.p - st
    }
    fn number(&mut self) -> bool {
        if self.peek() == Some(b'-') {
            self.p += 1;
        }
        match self.peek() {
            Some(b'0') => self.p += 1,
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => return false,
        }
        if self.peek() == Some(b'.') {
            self.p += 1;
            if self.digits() == 0 {
                return false;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.p += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.p += 1;
            }
            if self.digits() == 0 {
                return false;
            }
        }
        true
    }
    fn hex4(&self, at: usize) -> Option<u32> {
        let h = self.s.get(at..at + 4)?;
        let mut v = 0;
        for &c in h {
            v = v << 4 | (c as char).to_digit(16)?;
        }
        Some(v)
    }
    /// The string at p (on its opening quote). Decodes into `out` when given. False on any syntax error.
    fn str(&mut self, mut out: Option<&mut Vec<u8>>) -> bool {
        let s = self.s;
        let mut p = self.p + 1;
        while p < s.len() {
            let c = s[p];
            match c {
                b'"' => {
                    self.p = p + 1;
                    return true;
                }
                0..0x20 => return false,
                b'\\' => {
                    let e = match s.get(p + 1) {
                        Some(b'"') => b'"',
                        Some(b'\\') => b'\\',
                        Some(b'/') => b'/',
                        Some(b'b') => 8,
                        Some(b'f') => 12,
                        Some(b'n') => b'\n',
                        Some(b'r') => b'\r',
                        Some(b't') => b'\t',
                        Some(b'u') => {
                            let Some(mut cp) = self.hex4(p + 2) else { return false };
                            p += 6;
                            if (0xd800..0xdc00).contains(&cp) && s.get(p..p + 2) == Some(&b"\\u"[..]) {
                                let Some(lo) = self.hex4(p + 2) else { return false };
                                if (0xdc00..0xe000).contains(&lo) {
                                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                    p += 6;
                                }
                            }
                            // a lone surrogate becomes U+FFFD, as JavaScript's UTF-8 encoders do
                            let ch = char::from_u32(cp).unwrap_or('\u{fffd}');
                            if let Some(o) = out.as_deref_mut() {
                                o.extend_from_slice(ch.encode_utf8(&mut [0; 4]).as_bytes());
                            }
                            continue;
                        }
                        _ => return false,
                    };
                    if let Some(o) = out.as_deref_mut() {
                        o.push(e);
                    }
                    p += 2;
                }
                _ => {
                    let l = utf8_len(&s[p..]);
                    if l == 0 {
                        return false;
                    }
                    if let Some(o) = out.as_deref_mut() {
                        o.extend_from_slice(&s[p..p + l]);
                    }
                    p += l;
                }
            }
        }
        false
    }
    fn value(&mut self, depth: u32) -> bool {
        if depth > 64 {
            return false;
        }
        self.ws();
        match self.peek() {
            Some(b'"') => self.str(None),
            Some(b'{') => {
                self.p += 1;
                self.ws();
                if self.peek() == Some(b'}') {
                    self.p += 1;
                    return true;
                }
                loop {
                    self.ws();
                    if self.peek() != Some(b'"') || !self.str(None) {
                        return false;
                    }
                    self.ws();
                    if !self.lit(b":") || !self.value(depth + 1) {
                        return false;
                    }
                    self.ws();
                    if self.lit(b"}") {
                        return true;
                    }
                    if !self.lit(b",") {
                        return false;
                    }
                }
            }
            Some(b'[') => {
                self.p += 1;
                self.ws();
                if self.lit(b"]") {
                    return true;
                }
                loop {
                    if !self.value(depth + 1) {
                        return false;
                    }
                    self.ws();
                    if self.lit(b"]") {
                        return true;
                    }
                    if !self.lit(b",") {
                        return false;
                    }
                }
            }
            Some(b't') => self.lit(b"true"),
            Some(b'f') => self.lit(b"false"),
            Some(b'n') => self.lit(b"null"),
            Some(_) => self.number(),
            None => false,
        }
    }
}

/// Length of one valid UTF-8 sequence at the start of s, 0 if invalid.
fn utf8_len(s: &[u8]) -> usize {
    let n = match s[0] {
        0..0x80 => return 1,
        0xc2..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf4 => 4,
        _ => return 0,
    };
    match s.get(..n) {
        Some(b) if std::str::from_utf8(b).is_ok() => n,
        _ => 0,
    }
}

/// Validates a whole JSON document. For a top-level object, returns the raw span of the last value of each
/// wanted key (the last duplicate wins, as in JSON.parse). None if the document is invalid.
pub fn doc<'a, const N: usize>(s: &'a [u8], keys: [&[u8]; N]) -> Option<[Option<&'a [u8]>; N]> {
    let mut w = [None; N];
    let mut j = J { s, p: 0 };
    j.ws();
    if j.lit(b"{") {
        j.ws();
        if !j.lit(b"}") {
            let mut k = Vec::new();
            loop {
                j.ws();
                k.clear();
                if j.peek() != Some(b'"') || !j.str(Some(&mut k)) {
                    return None;
                }
                j.ws();
                if !j.lit(b":") {
                    return None;
                }
                j.ws();
                let vs = j.p;
                if !j.value(1) {
                    return None;
                }
                if let Some(i) = keys.iter().position(|x| *x == k) {
                    w[i] = Some(&s[vs..j.p]);
                }
                j.ws();
                if j.lit(b"}") {
                    break;
                }
                if !j.lit(b",") {
                    return None;
                }
            }
        }
    } else if !j.value(0) {
        return None;
    }
    j.ws();
    (j.p == s.len()).then_some(w)
}

/// Decodes a raw span that `doc` returned, if it is a string.
pub fn string(v: &[u8]) -> Option<String> {
    if v.first() != Some(&b'"') {
        return None;
    }
    let mut o = Vec::with_capacity(v.len());
    J { s: v, p: 0 }.str(Some(&mut o));
    String::from_utf8(o).ok()
}

/// Appends s as JSON string contents (quotes not included).
pub fn esc(o: &mut Vec<u8>, s: &[u8]) {
    let mut run = 0;
    for (i, &c) in s.iter().enumerate() {
        if c >= 0x20 && c != b'"' && c != b'\\' {
            continue;
        }
        o.extend_from_slice(&s[run..i]);
        run = i + 1;
        match c {
            b'"' => o.extend_from_slice(b"\\\""),
            b'\\' => o.extend_from_slice(b"\\\\"),
            b'\n' => o.extend_from_slice(b"\\n"),
            b'\r' => o.extend_from_slice(b"\\r"),
            b'\t' => o.extend_from_slice(b"\\t"),
            8 => o.extend_from_slice(b"\\b"),
            12 => o.extend_from_slice(b"\\f"),
            _ => {
                const H: &[u8; 16] = b"0123456789abcdef";
                o.extend_from_slice(&[b'\\', b'u', b'0', b'0', H[(c >> 4) as usize], H[(c & 15) as usize]]);
            }
        }
    }
    o.extend_from_slice(&s[run..]);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn docs() {
        assert!(doc(b"{bad json", [b"body"]).is_none());
        assert!(doc(b"", [b"body"]).is_none());
        assert!(doc(b"{\"a\":1,}", [b"body"]).is_none());
        assert!(doc(b"[1,2] x", [b"body"]).is_none());
        assert!(doc(b"{\"a\":01}", [b"a"]).is_none());
        assert_eq!(doc(b" [1,{\"body\":2}] ", [b"body"]), Some([None]));
        let w = doc(b"{\"body\":\"x\",\"b\\u006fdy\":\"y\\u00e9\\ud83d\\ude00\"}", [b"body"]).unwrap();
        assert_eq!(string(w[0].unwrap()).unwrap(), "y\u{e9}\u{1f600}");
        assert_eq!(doc(b"{\"body\":123}", [b"body"]).unwrap()[0], Some(&b"123"[..]));
        assert!(string(b"123").is_none());
        assert!(doc(b"{\"body\":\"\xff\"}", [b"body"]).is_none());
        let mut o = Vec::new();
        esc(&mut o, b"a\"b\\c\nd\x01");
        assert_eq!(o, b"a\\\"b\\\\c\\nd\\u0001");
    }
}
