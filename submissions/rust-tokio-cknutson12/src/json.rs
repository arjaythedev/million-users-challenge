//! Hand-written JSON output: compact, keys in the spec's order.

/// Appends `s` as a JSON string (quotes included). Escapes `"`, `\` and control characters; everything else,
/// non-ASCII included, is copied as-is.
pub fn string(out: &mut Vec<u8>, s: &str) {
    out.push(b'"');
    let bytes = s.as_bytes();
    let mut start = 0;
    for (i, &b) in bytes.iter().enumerate() {
        let esc: &[u8] = match b {
            b'"' => b"\\\"",
            b'\\' => b"\\\\",
            b'\n' => b"\\n",
            b'\r' => b"\\r",
            b'\t' => b"\\t",
            0x08 => b"\\b",
            0x0c => b"\\f",
            0..=0x1f => {
                out.extend_from_slice(&bytes[start..i]);
                const HEX: &[u8; 16] = b"0123456789abcdef";
                out.extend_from_slice(&[b'\\', b'u', b'0', b'0', HEX[(b >> 4) as usize], HEX[(b & 15) as usize]]);
                start = i + 1;
                continue;
            }
            _ => continue,
        };
        out.extend_from_slice(&bytes[start..i]);
        out.extend_from_slice(esc);
        start = i + 1;
    }
    out.extend_from_slice(&bytes[start..]);
    out.push(b'"');
}

pub fn int(out: &mut Vec<u8>, n: impl itoa::Integer) {
    out.extend_from_slice(itoa::Buffer::new().format(n).as_bytes());
}

/// A post object: `{"id":…,"body":…,"created_at":…,"author":…,"like_count":…}`.
pub fn post(out: &mut Vec<u8>, id: i64, body: &str, created_us: i64, author: &str, like_count: i32) {
    out.extend_from_slice(b"{\"id\":");
    int(out, id);
    out.extend_from_slice(b",\"body\":");
    string(out, body);
    out.extend_from_slice(b",\"created_at\":\"");
    timestamp(out, created_us);
    out.extend_from_slice(b"\",\"author\":");
    string(out, author);
    out.extend_from_slice(b",\"like_count\":");
    int(out, like_count);
    out.push(b'}');
}

/// `us` is a Postgres TIMESTAMPTZ (microseconds since 2000-01-01 UTC), written as `2025-12-31T23:59:19.409Z`:
/// UTC, truncated (not rounded) to milliseconds.
pub fn timestamp(out: &mut Vec<u8>, us: i64) {
    let ms = us.div_euclid(1000);
    let secs = ms.div_euclid(1000);
    let millis = ms.rem_euclid(1000) as u32;
    let sod = secs.rem_euclid(86_400) as u32;
    // 10,957 days from 1970-01-01 to 2000-01-01.
    let (y, m, d) = civil_from_days(secs.div_euclid(86_400) + 10_957);
    let two = |out: &mut Vec<u8>, v: u32| out.extend_from_slice(&[b'0' + (v / 10) as u8, b'0' + (v % 10) as u8]);
    let y = y as u32;
    two(out, y / 100);
    two(out, y % 100);
    out.push(b'-');
    two(out, m);
    out.push(b'-');
    two(out, d);
    out.push(b'T');
    two(out, sod / 3600);
    out.push(b':');
    two(out, sod / 60 % 60);
    out.push(b':');
    two(out, sod % 60);
    out.extend_from_slice(&[b'.', b'0' + (millis / 100) as u8, b'0' + (millis / 10 % 10) as u8, b'0' + (millis % 10) as u8, b'Z']);
}

/// Days since 1970-01-01 -> (year, month, day), proleptic Gregorian (Howard Hinnant's algorithm).
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (yoe + era * 400 + i64::from(m <= 2), m, d)
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
        assert_eq!(ts(-1), "1999-12-31T23:59:59.999Z");
        // 2025-12-31T23:59:59.999999Z truncates to .999
        assert_eq!(ts(820_540_799_999_999), "2025-12-31T23:59:59.999Z");
        // 2024-02-29T12:34:56.789123Z
        assert_eq!(ts(762_525_296_789_123), "2024-02-29T12:34:56.789Z");
    }

    #[test]
    fn strings() {
        let mut v = Vec::new();
        string(&mut v, "a\"b\\c\nd\u{1}é✓</>");
        assert_eq!(String::from_utf8(v).unwrap(), r#""a\"b\\c\nd\u0001é✓</>""#);
    }
}
