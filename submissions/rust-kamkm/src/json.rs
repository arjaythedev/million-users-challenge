//! A strict RFC 8259 validator that decodes only what the API reads. `doc` walks a whole document
//! once and hands back the raw byte spans of the wanted keys (the last duplicate wins, matching
//! what JSON.parse keeps); `string` decodes such a span; `esc` writes string contents back out.

struct P<'a> {
    s: &'a [u8],
    i: usize,
}

impl P<'_> {
    fn peek(&self) -> Option<u8> {
        self.s.get(self.i).copied()
    }

    fn ws(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.i += 1;
        }
    }

    fn eats(&mut self, l: &[u8]) -> bool {
        if self.s[self.i..].starts_with(l) {
            self.i += l.len();
            true
        } else {
            false
        }
    }

    fn digits(&mut self) -> usize {
        let at = self.i;
        while matches!(self.peek(), Some(b'0'..=b'9')) {
            self.i += 1;
        }
        self.i - at
    }

    fn number(&mut self) -> bool {
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.peek() {
            Some(b'0') => self.i += 1,
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => return false,
        }
        if self.peek() == Some(b'.') {
            self.i += 1;
            if self.digits() == 0 {
                return false;
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if self.digits() == 0 {
                return false;
            }
        }
        true
    }

    /// The string starting at the opening quote. Contents go into `out` when given. Control
    /// characters must be escaped, bad UTF-8 is an error, and a lone \uD800.. surrogate becomes
    /// U+FFFD the way the common encoders do it.
    fn str(&mut self, mut out: Option<&mut Vec<u8>>) -> bool {
        let s = self.s;
        let mut i = self.i + 1;
        while i < s.len() {
            match s[i] {
                b'"' => {
                    self.i = i + 1;
                    return true;
                }
                0..0x20 => return false,
                b'\\' => {
                    let e = match s.get(i + 1) {
                        Some(b'"') => b'"',
                        Some(b'\\') => b'\\',
                        Some(b'/') => b'/',
                        Some(b'b') => 8,
                        Some(b'f') => 12,
                        Some(b'n') => b'\n',
                        Some(b'r') => b'\r',
                        Some(b't') => b'\t',
                        Some(b'u') => {
                            let Some(mut cp) = hex4(s, i + 2) else { return false };
                            i += 6;
                            if (0xd800..0xdc00).contains(&cp) && s.get(i..i + 2) == Some(b"\\u".as_slice()) {
                                let Some(lo) = hex4(s, i + 2) else { return false };
                                if (0xdc00..0xe000).contains(&lo) {
                                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                                    i += 6;
                                }
                            }
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
                    i += 2;
                }
                _ => {
                    let n = utf8_len(&s[i..]);
                    if n == 0 {
                        return false;
                    }
                    if let Some(o) = out.as_deref_mut() {
                        o.extend_from_slice(&s[i..i + n]);
                    }
                    i += n;
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
                self.i += 1;
                self.ws();
                if self.eats(b"}") {
                    return true;
                }
                loop {
                    self.ws();
                    if self.peek() != Some(b'"') || !self.str(None) {
                        return false;
                    }
                    self.ws();
                    if !self.eats(b":") || !self.value(depth + 1) {
                        return false;
                    }
                    self.ws();
                    if self.eats(b"}") {
                        return true;
                    }
                    if !self.eats(b",") {
                        return false;
                    }
                }
            }
            Some(b'[') => {
                self.i += 1;
                self.ws();
                if self.eats(b"]") {
                    return true;
                }
                loop {
                    if !self.value(depth + 1) {
                        return false;
                    }
                    self.ws();
                    if self.eats(b"]") {
                        return true;
                    }
                    if !self.eats(b",") {
                        return false;
                    }
                }
            }
            Some(b't') => self.eats(b"true"),
            Some(b'f') => self.eats(b"false"),
            Some(b'n') => self.eats(b"null"),
            Some(_) => self.number(),
            None => false,
        }
    }
}

fn hex4(s: &[u8], at: usize) -> Option<u32> {
    let h = s.get(at..at + 4)?;
    let mut v = 0;
    for &c in h {
        v = v << 4 | (c as char).to_digit(16)?;
    }
    Some(v)
}

/// Length of the UTF-8 sequence at the start of s, or 0 if invalid (also 0 for a bare
/// continuation or overlong/invalid code point).
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

/// Validates the whole document `s`. For a top-level object the raw span of the last value of
/// each wanted key is returned; for anything else the spans stay None. None means invalid input.
pub fn doc<'a, const N: usize>(s: &'a [u8], keys: [&[u8]; N]) -> Option<[Option<&'a [u8]>; N]> {
    let mut want: [Option<&'a [u8]>; N] = [None; N];
    let mut p = P { s, i: 0 };
    p.ws();
    if p.eats(b"{") {
        p.ws();
        if !p.eats(b"}") {
            let mut k = Vec::new();
            loop {
                p.ws();
                k.clear();
                if p.peek() != Some(b'"') || !p.str(Some(&mut k)) {
                    return None;
                }
                p.ws();
                if !p.eats(b":") {
                    return None;
                }
                p.ws();
                let at = p.i;
                if !p.value(1) {
                    return None;
                }
                if let Some(slot) = keys.iter().position(|k2| *k2 == k) {
                    want[slot] = Some(&s[at..p.i]);
                }
                p.ws();
                if p.eats(b"}") {
                    break;
                }
                if !p.eats(b",") {
                    return None;
                }
            }
        }
    } else if !p.value(0) {
        return None;
    }
    p.ws();
    (p.i == s.len()).then_some(want)
}

/// Decodes a span returned by `doc` when it holds a string.
pub fn string(v: &[u8]) -> Option<String> {
    if v.first() != Some(&b'"') {
        return None;
    }
    let mut o = Vec::with_capacity(v.len());
    P { s: v, i: 0 }.str(Some(&mut o));
    String::from_utf8(o).ok()
}

/// Appends `s` to `o` as string contents (no quotes), escaping what JSON requires. The common
/// case is a run of plain bytes copied in one extend.
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
        assert!(doc(b"{\"body\":\"\xff\"}", [b"body"]).is_none());
        assert!(doc(b"{}\n\n", [b"body"]).is_some());
        let w = doc(b" [1,{\"body\":2}] ", [b"body"]).unwrap();
        assert_eq!(w[0], None); // only a top-level object's keys are extracted
        let w = doc(b"{\"body\":\"x\",\"b\\u006fdy\":\"y\\u00e9\\ud83d\\ude00\"}", [b"body"]).unwrap();
        assert_eq!(string(w[0].unwrap()).unwrap(), "y\u{e9}\u{1f600}");
        assert!(string(b"123").is_none());
        let mut o = Vec::new();
        esc(&mut o, b"a\"b\\c\nd\x01");
        assert_eq!(o, b"a\\\"b\\\\c\\nd\\u0001");
    }
}
