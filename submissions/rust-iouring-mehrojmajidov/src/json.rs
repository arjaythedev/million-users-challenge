//! JSON: a strict reader for request bodies and token segments, and the string escaping for responses.

/// Appends `s` as the inside of a JSON string. Only what JSON requires is escaped, as `JSON.stringify` does.
pub fn escape_into(out: &mut Vec<u8>, s: &[u8]) {
    let mut start = 0;
    for (i, &b) in s.iter().enumerate() {
        if b >= 0x20 && b != b'"' && b != b'\\' {
            continue;
        }
        out.extend_from_slice(&s[start..i]);
        start = i + 1;
        match b {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            0x08 => out.extend_from_slice(b"\\b"),
            0x0c => out.extend_from_slice(b"\\f"),
            _ => {
                const HEX: &[u8; 16] = b"0123456789abcdef";
                out.extend_from_slice(b"\\u00");
                out.push(HEX[(b >> 4) as usize]);
                out.push(HEX[(b & 15) as usize]);
            }
        }
    }
    out.extend_from_slice(&s[start..]);
}

pub fn push_u64(out: &mut Vec<u8>, mut v: u64) {
    let mut tmp = [0u8; 20];
    let mut i = tmp.len();
    loop {
        i -= 1;
        tmp[i] = b'0' + (v % 10) as u8;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    out.extend_from_slice(&tmp[i..]);
}

/// A top-level member of a JSON object, as far as the server cares.
pub enum Value {
    Str(Vec<u8>),
    Num(f64),
    Other,
}

pub enum Doc {
    /// Valid JSON whose top level is an object; the wanted members, in the order of `keys`.
    Object(Vec<Option<Value>>),
    /// Valid JSON of another kind (`null`, a number, an array, ...).
    NotObject,
}

const MAX_DEPTH: u32 = 256;

struct Reader<'a> {
    s: &'a [u8],
    i: usize,
}

impl<'a> Reader<'a> {
    fn ws(&mut self) {
        while let Some(&b) = self.s.get(self.i) {
            if b == b' ' || b == b'\n' || b == b'\r' || b == b'\t' {
                self.i += 1;
            } else {
                break;
            }
        }
    }

    fn eat(&mut self, lit: &[u8]) -> Result<(), ()> {
        if self.s[self.i..].starts_with(lit) {
            self.i += lit.len();
            Ok(())
        } else {
            Err(())
        }
    }

    fn hex4(&mut self) -> Result<u32, ()> {
        let h = self.s.get(self.i..self.i + 4).ok_or(())?;
        let mut v = 0u32;
        for &c in h {
            v = v << 4 | (c as char).to_digit(16).ok_or(())?;
        }
        self.i += 4;
        Ok(v)
    }

    /// Reads a string (the opening quote already consumed), decoding into `out` when given.
    fn string(&mut self, mut out: Option<&mut Vec<u8>>) -> Result<(), ()> {
        let begin = self.i;
        loop {
            let b = *self.s.get(self.i).ok_or(())?;
            match b {
                b'"' => {
                    // the raw bytes must be UTF-8; escapes produce valid UTF-8 by construction
                    core::str::from_utf8(&self.s[begin..self.i]).map_err(|_| ())?;
                    self.i += 1;
                    return Ok(());
                }
                b'\\' => {
                    self.i += 1;
                    let e = *self.s.get(self.i).ok_or(())?;
                    self.i += 1;
                    let ch = match e {
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
                            let cp = if (0xd800..0xdc00).contains(&hi) && self.s[self.i..].starts_with(b"\\u") {
                                let save = self.i;
                                self.i += 2;
                                let lo = self.hex4()?;
                                if (0xdc00..0xe000).contains(&lo) {
                                    0x10000 + ((hi - 0xd800) << 10) + (lo - 0xdc00)
                                } else {
                                    self.i = save;
                                    hi
                                }
                            } else {
                                hi
                            };
                            // a lone surrogate has no UTF-8 form; JavaScript would keep it, Postgres could not store it
                            char::from_u32(cp).unwrap_or('\u{fffd}')
                        }
                        _ => return Err(()),
                    };
                    if let Some(o) = out.as_deref_mut() {
                        let mut tmp = [0u8; 4];
                        o.extend_from_slice(ch.encode_utf8(&mut tmp).as_bytes());
                    }
                }
                0..=0x1f => return Err(()),
                _ => {
                    if let Some(o) = out.as_deref_mut() {
                        o.push(b);
                    }
                    self.i += 1;
                }
            }
        }
    }

    fn number(&mut self) -> Result<f64, ()> {
        let start = self.i;
        let digits = |r: &mut Reader| {
            let s = r.i;
            while r.s.get(r.i).is_some_and(|c| c.is_ascii_digit()) {
                r.i += 1;
            }
            r.i > s
        };
        if self.s.get(self.i) == Some(&b'-') {
            self.i += 1;
        }
        if self.s.get(self.i) == Some(&b'0') {
            self.i += 1;
        } else if !digits(self) {
            return Err(());
        }
        if self.s.get(self.i) == Some(&b'.') {
            self.i += 1;
            if !digits(self) {
                return Err(());
            }
        }
        if matches!(self.s.get(self.i), Some(b'e' | b'E')) {
            self.i += 1;
            if matches!(self.s.get(self.i), Some(b'+' | b'-')) {
                self.i += 1;
            }
            if !digits(self) {
                return Err(());
            }
        }
        core::str::from_utf8(&self.s[start..self.i]).ok().and_then(|t| t.parse().ok()).ok_or(())
    }

    /// Reads any value. `capture` receives it when it is a string or a number.
    fn value(&mut self, depth: u32, capture: Option<&mut Option<Value>>) -> Result<(), ()> {
        if depth > MAX_DEPTH {
            return Err(());
        }
        self.ws();
        match *self.s.get(self.i).ok_or(())? {
            b'"' => {
                self.i += 1;
                match capture {
                    Some(slot) => {
                        let mut v = Vec::new();
                        self.string(Some(&mut v))?;
                        *slot = Some(Value::Str(v));
                    }
                    None => self.string(None)?,
                }
                return Ok(());
            }
            b'{' => {
                self.i += 1;
                self.ws();
                if self.s.get(self.i) == Some(&b'}') {
                    self.i += 1;
                } else {
                    loop {
                        self.ws();
                        self.eat(b"\"")?;
                        self.string(None)?;
                        self.ws();
                        self.eat(b":")?;
                        self.value(depth + 1, None)?;
                        self.ws();
                        match self.s.get(self.i) {
                            Some(b',') => self.i += 1,
                            Some(b'}') => {
                                self.i += 1;
                                break;
                            }
                            _ => return Err(()),
                        }
                    }
                }
            }
            b'[' => {
                self.i += 1;
                self.ws();
                if self.s.get(self.i) == Some(&b']') {
                    self.i += 1;
                } else {
                    loop {
                        self.value(depth + 1, None)?;
                        self.ws();
                        match self.s.get(self.i) {
                            Some(b',') => self.i += 1,
                            Some(b']') => {
                                self.i += 1;
                                break;
                            }
                            _ => return Err(()),
                        }
                    }
                }
            }
            b't' => self.eat(b"true")?,
            b'f' => self.eat(b"false")?,
            b'n' => self.eat(b"null")?,
            b'-' | b'0'..=b'9' => {
                let n = self.number()?;
                if let Some(slot) = capture {
                    *slot = Some(Value::Num(n));
                }
                return Ok(());
            }
            _ => return Err(()),
        }
        if let Some(slot) = capture {
            *slot = Some(Value::Other);
        }
        Ok(())
    }
}

/// Validates the whole document. For a top-level object, returns the members named in `keys`
/// (the last one wins when a name repeats, as in `JSON.parse`). `Err` means it is not valid JSON.
pub fn parse_object(input: &[u8], keys: &[&[u8]]) -> Result<Doc, ()> {
    let mut r = Reader { s: input, i: 0 };
    r.ws();
    if r.s.get(r.i) != Some(&b'{') {
        r.value(0, None)?;
        r.ws();
        return if r.i == input.len() { Ok(Doc::NotObject) } else { Err(()) };
    }
    r.i += 1;
    let mut found: Vec<Option<Value>> = keys.iter().map(|_| None).collect();
    r.ws();
    if r.s.get(r.i) == Some(&b'}') {
        r.i += 1;
    } else {
        let mut name = Vec::new();
        loop {
            r.ws();
            r.eat(b"\"")?;
            name.clear();
            r.string(Some(&mut name))?;
            r.ws();
            r.eat(b":")?;
            match keys.iter().position(|k| *k == name.as_slice()) {
                Some(k) => r.value(1, Some(&mut found[k]))?,
                None => r.value(1, None)?,
            }
            r.ws();
            match r.s.get(r.i) {
                Some(b',') => r.i += 1,
                Some(b'}') => {
                    r.i += 1;
                    break;
                }
                _ => return Err(()),
            }
        }
    }
    r.ws();
    if r.i == input.len() {
        Ok(Doc::Object(found))
    } else {
        Err(())
    }
}

/// What JavaScript's `String.prototype.trim` removes: WhiteSpace and LineTerminator.
fn is_js_space(c: char) -> bool {
    matches!(
        c,
        '\u{9}'..='\u{d}'
            | ' '
            | '\u{a0}'
            | '\u{1680}'
            | '\u{2000}'..='\u{200a}'
            | '\u{2028}'
            | '\u{2029}'
            | '\u{202f}'
            | '\u{205f}'
            | '\u{3000}'
            | '\u{feff}'
    )
}

pub fn trim_js(s: &str) -> &str {
    s.trim_matches(is_js_space)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn esc(s: &str) -> String {
        let mut o = Vec::new();
        escape_into(&mut o, s.as_bytes());
        String::from_utf8(o).unwrap()
    }

    fn body(s: &str) -> Result<Option<String>, ()> {
        match parse_object(s.as_bytes(), &[b"body"])? {
            Doc::Object(mut f) => Ok(match f.remove(0) {
                Some(Value::Str(v)) => Some(String::from_utf8(v).unwrap()),
                _ => None,
            }),
            Doc::NotObject => Ok(None),
        }
    }

    #[test]
    fn escaping() {
        assert_eq!(esc("plain / <b>&amp;</b> café ✓"), "plain / <b>&amp;</b> café ✓");
        assert_eq!(esc("a\"b\\c"), "a\\\"b\\\\c");
        assert_eq!(esc("\n\r\t\u{8}\u{c}\u{1}\u{1f}"), "\\n\\r\\t\\b\\f\\u0001\\u001f");
        assert_eq!(esc(""), "");
    }

    #[test]
    fn numbers() {
        let mut o = Vec::new();
        push_u64(&mut o, 0);
        o.push(b' ');
        push_u64(&mut o, 500_000);
        o.push(b' ');
        push_u64(&mut o, u64::MAX);
        assert_eq!(o, b"0 500000 18446744073709551615");
    }

    #[test]
    fn bodies() {
        assert_eq!(body(r#"{"body":"x"}"#), Ok(Some("x".into())));
        assert_eq!(body(r#" { "a" : [1, {"b": null}], "body" : "hi\n\"there\" \u00e9 \ud83d\ude00" } "#),
            Ok(Some("hi\n\"there\" é 😀".into())));
        assert_eq!(body(r#"{"body":"a","body":"b"}"#), Ok(Some("b".into())), "last one wins");
        assert_eq!(body(r#"{"body":"a","body":1}"#), Ok(None));
        assert_eq!(body(r#"{"body":123}"#), Ok(None));
        assert_eq!(body(r#"{"body":null}"#), Ok(None));
        assert_eq!(body("{}"), Ok(None));
        assert_eq!(body("[]"), Ok(None));
        assert_eq!(body("null"), Ok(None));
        assert_eq!(body("\"str\""), Ok(None));
        assert_eq!(body("12.5e3"), Ok(None));
        assert_eq!(body(r#"{"x":{"body":"nested"}}"#), Ok(None));
        assert_eq!(body(r#"{"body":"\ud800"}"#), Ok(Some("\u{fffd}".into())));
    }

    #[test]
    fn malformed() {
        for bad in [
            "", " ", "{bad json", "{", "{\"body\":\"x\"", "{\"body\":\"x\"}x", "{\"body\":'x'}", "{body:\"x\"}",
            "{\"body\":\"x\",}", "{\"a\":01}", "{\"a\":1.}", "{\"a\":.5}", "{\"a\":+1}", "{\"a\":1e}", "{\"a\":tru}",
            "{\"a\":\"\\x\"}", "{\"a\":\"\\u12g4\"}", "{\"a\":\"line\nbreak\"}", "[1,]", "[1 2]", "nul", "{\"a\"}",
            "{\"a\":}", "\"open", "{\"a\":\"\\",
        ] {
            assert!(body(bad).is_err(), "should be malformed: {bad:?}");
        }
        assert!(parse_object(b"{\"body\":\"\xff\"}", &[b"body"]).is_err(), "invalid UTF-8");
        let deep = "[".repeat(300) + &"]".repeat(300);
        assert!(body(&deep).is_err());
    }

    #[test]
    fn token_members() {
        let Ok(Doc::Object(f)) =
            parse_object(br#"{"sub":"42","username":"golden_ember_1","iat":1,"exp":1.9e9}"#, &[b"sub", b"username", b"exp", b"nbf"])
        else {
            panic!()
        };
        assert!(matches!(&f[0], Some(Value::Str(s)) if s == b"42"));
        assert!(matches!(&f[1], Some(Value::Str(s)) if s == b"golden_ember_1"));
        assert!(matches!(f[2], Some(Value::Num(n)) if n == 1.9e9));
        assert!(f[3].is_none());
    }

    #[test]
    fn trimming() {
        assert_eq!(trim_js("  test: café ✓  "), "test: café ✓");
        assert_eq!(trim_js("\u{feff}\u{a0}\t\n x \u{2028}\u{3000}\r"), "x");
        assert_eq!(trim_js("   "), "");
        assert_eq!(trim_js("\u{200b}x"), "\u{200b}x", "zero-width space is not whitespace in JavaScript");
    }
}
