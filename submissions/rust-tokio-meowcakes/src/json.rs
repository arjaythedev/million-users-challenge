pub const NOT_FOUND: &[u8] = br#"{"error":"not found"}"#;
pub const INVALID_ID: &[u8] = br#"{"error":"invalid post id"}"#;
pub const POST_NOT_FOUND: &[u8] = br#"{"error":"post not found"}"#;
pub const MISSING_TOKEN: &[u8] = br#"{"error":"missing bearer token"}"#;
pub const INVALID_TOKEN: &[u8] = br#"{"error":"invalid or expired token"}"#;
pub const INVALID_PAYLOAD: &[u8] = br#"{"error":"invalid token payload"}"#;
pub const MALFORMED: &[u8] = br#"{"error":"malformed JSON body"}"#;
pub const BODY_REQUIRED: &[u8] = br#"{"error":"body is required"}"#;
pub const BODY_TOO_LONG: &[u8] = br#"{"error":"body must be at most 500 characters"}"#;
pub const INTERNAL: &[u8] = br#"{"error":"internal server error"}"#;
pub const BAD_REQUEST: &[u8] = br#"{"error":"bad request"}"#;
pub const TOO_LARGE: &[u8] = br#"{"error":"request too large"}"#;

pub fn post(
    out: &mut Vec<u8>,
    id: i64,
    body: &[u8],
    created_at: i64,
    author: &[u8],
    like_count: i64,
) {
    let mut n = itoa::Buffer::new();
    out.extend_from_slice(b"{\"id\":");
    out.extend_from_slice(n.format(id).as_bytes());
    out.extend_from_slice(b",\"body\":");
    string(out, body);
    out.extend_from_slice(b",\"created_at\":\"");
    timestamp(out, created_at);
    out.extend_from_slice(b"\",\"author\":");
    string(out, author);
    out.extend_from_slice(b",\"like_count\":");
    out.extend_from_slice(n.format(like_count).as_bytes());
    out.push(b'}');
}

pub fn like(out: &mut Vec<u8>, already: bool, post_id: i64) {
    out.extend_from_slice(if already {
        b"{\"liked\":true,\"already_liked\":true,\"post_id\":"
    } else {
        b"{\"liked\":true,\"already_liked\":false,\"post_id\":"
    });
    out.extend_from_slice(itoa::Buffer::new().format(post_id).as_bytes());
    out.push(b'}');
}

pub fn health(out: &mut Vec<u8>, uptime_s: u64) {
    out.extend_from_slice(b"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    out.extend_from_slice(itoa::Buffer::new().format(uptime_s).as_bytes());
    out.push(b'}');
}

pub fn degraded(out: &mut Vec<u8>, error: &str) {
    out.extend_from_slice(b"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
    string(out, error.as_bytes());
    out.push(b'}');
}

pub fn string(out: &mut Vec<u8>, s: &[u8]) {
    out.reserve(s.len() + 2);
    out.push(b'"');
    let mut start = 0;
    loop {
        let i = next_escape(s, start);
        out.extend_from_slice(&s[start..i]);
        if i == s.len() {
            break;
        }
        escape(out, s[i]);
        start = i + 1;
    }
    out.push(b'"');
}

fn needs_escape(c: u8) -> bool {
    c < 0x20 || c == b'"' || c == b'\\'
}

fn next_escape(s: &[u8], mut i: usize) -> usize {
    const ONES: u64 = 0x0101_0101_0101_0101;
    const HIGH: u64 = 0x8080_8080_8080_8080;
    while i + 8 <= s.len() {
        let x = u64::from_le_bytes(s[i..i + 8].try_into().unwrap());
        let below_space = x.wrapping_sub(ONES * 0x20) & !x;
        let quote = (x ^ (ONES * b'"' as u64)).wrapping_sub(ONES) & !(x ^ (ONES * b'"' as u64));
        let backslash =
            (x ^ (ONES * b'\\' as u64)).wrapping_sub(ONES) & !(x ^ (ONES * b'\\' as u64));
        let hits = (below_space | quote | backslash) & HIGH;
        if hits != 0 {
            return i + hits.trailing_zeros() as usize / 8;
        }
        i += 8;
    }
    while i < s.len() && !needs_escape(s[i]) {
        i += 1;
    }
    i
}

fn escape(out: &mut Vec<u8>, c: u8) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    match c {
        b'"' => out.extend_from_slice(b"\\\""),
        b'\\' => out.extend_from_slice(b"\\\\"),
        b'\n' => out.extend_from_slice(b"\\n"),
        b'\r' => out.extend_from_slice(b"\\r"),
        b'\t' => out.extend_from_slice(b"\\t"),
        0x08 => out.extend_from_slice(b"\\b"),
        0x0c => out.extend_from_slice(b"\\f"),
        _ => out.extend_from_slice(&[
            b'\\',
            b'u',
            b'0',
            b'0',
            HEX[(c >> 4) as usize],
            HEX[(c & 15) as usize],
        ]),
    }
}

pub fn timestamp(out: &mut Vec<u8>, pg_micros: i64) {
    const PG_EPOCH_MS: i64 = 946_684_800_000;
    let ms = pg_micros.div_euclid(1000) + PG_EPOCH_MS;
    let (days, ms_of_day) = (ms.div_euclid(86_400_000), ms.rem_euclid(86_400_000));
    let (y, m, d) = civil_from_days(days);
    let secs = ms_of_day / 1000;
    let mut b = *b"0000-00-00T00:00:00.000Z";
    let mut put = |at: usize, width: usize, mut v: i64| {
        for i in (at..at + width).rev() {
            b[i] = b'0' + (v % 10) as u8;
            v /= 10;
        }
    };
    put(0, 4, y);
    put(5, 2, m);
    put(8, 2, d);
    put(11, 2, secs / 3600);
    put(14, 2, secs / 60 % 60);
    put(17, 2, secs % 60);
    put(20, 3, ms_of_day % 1000);
    out.extend_from_slice(&b);
}

fn civil_from_days(z: i64) -> (i64, i64, i64) {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    (yoe + era * 400 + (m <= 2) as i64, m, d)
}

pub fn new_post(body: &[u8]) -> Result<Box<str>, &'static [u8]> {
    let v: serde_json::Value = serde_json::from_slice(body).map_err(|_| MALFORMED)?;
    let s = v
        .get("body")
        .and_then(|b| b.as_str())
        .ok_or(BODY_REQUIRED)?;
    let s = js_trim(s);
    if s.is_empty() {
        return Err(BODY_REQUIRED);
    }
    if s.len() > 500 && s.chars().count() > 500 {
        return Err(BODY_TOO_LONG);
    }
    Ok(s.into())
}

fn js_trim(s: &str) -> &str {
    let ws = |c: char| {
        matches!(
            c,
            '\t' | '\n' | '\u{0B}' | '\u{0C}' | '\r' | ' ' | '\u{A0}' | '\u{1680}' | '\u{2000}'
                ..='\u{200A}'
        ) || matches!(
            c,
            '\u{2028}' | '\u{2029}' | '\u{202F}' | '\u{205F}' | '\u{3000}' | '\u{FEFF}'
        )
    };
    s.trim_matches(ws)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ts(us: i64) -> String {
        let mut v = Vec::new();
        timestamp(&mut v, us);
        String::from_utf8(v).unwrap()
    }

    #[test]
    fn timestamps() {
        assert_eq!(ts(0), "2000-01-01T00:00:00.000Z");
        assert_eq!(ts(999), "2000-01-01T00:00:00.000Z");
        assert_eq!(ts(-1), "1999-12-31T23:59:59.999Z");
        assert_eq!(
            ts(1_767_225_559_409_876 - 946_684_800_000_000i64),
            "2025-12-31T23:59:19.409Z"
        );
        assert_eq!(
            ts(951_782_400_000_000 - 946_684_800_000_000i64),
            "2000-02-29T00:00:00.000Z"
        );
    }

    #[test]
    fn strings() {
        let mut v = Vec::new();
        string(&mut v, "a\"b\\c\nd\u{1}é✓".as_bytes());
        assert_eq!(String::from_utf8(v).unwrap(), r#""a\"b\\c\nd\u0001é✓""#);
    }

    fn string_reference(s: &[u8]) -> Vec<u8> {
        let mut out = vec![b'"'];
        for &c in s {
            if needs_escape(c) {
                escape(&mut out, c)
            } else {
                out.push(c)
            }
        }
        out.push(b'"');
        out
    }

    #[test]
    fn strings_match_reference() {
        let mut seed = 0x9E37_79B9_7F4A_7C15u64;
        let mut rng = || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let alphabet: &[u8] = b"abcXYZ 019\"\\\n\t\x01\x1f\x7f\x80\xc3\xa9";
        for len in 0..200 {
            for _ in 0..20 {
                let s: Vec<u8> = (0..len)
                    .map(|_| {
                        let r = rng();
                        if r % 10 == 0 {
                            alphabet[(r >> 8) as usize % alphabet.len()]
                        } else {
                            b'a' + (r >> 16) as u8 % 26
                        }
                    })
                    .collect();
                let mut out = Vec::new();
                string(&mut out, &s);
                assert_eq!(out, string_reference(&s), "input {s:?}");
            }
        }
        for c in 0..=255u8 {
            let s = [b'x', b'y', c, b'z'];
            let mut out = Vec::new();
            string(&mut out, &s);
            assert_eq!(out, string_reference(&s), "byte {c}");
        }
    }

    #[test]
    fn new_posts() {
        assert_eq!(&*new_post(br#"{"body":"  hi \u0009"}"#).unwrap(), "hi");
        assert_eq!(new_post(br#"{"body":"   "}"#), Err(BODY_REQUIRED));
        assert_eq!(new_post(br#"{"body":123}"#), Err(BODY_REQUIRED));
        assert_eq!(new_post(br#"{}"#), Err(BODY_REQUIRED));
        assert_eq!(new_post(br#"{bad"#), Err(MALFORMED));
        let long = format!(r#"{{"body":"{}"}}"#, "é".repeat(501));
        assert_eq!(new_post(long.as_bytes()), Err(BODY_TOO_LONG));
        let ok = format!(r#"{{"body":" {} "}}"#, "é".repeat(500));
        assert_eq!(new_post(ok.as_bytes()).unwrap().chars().count(), 500);
    }
}
