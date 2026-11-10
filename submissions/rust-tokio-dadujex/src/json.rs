//! JSON output. The response shapes are fixed, so the bytes are written directly.

/// A JSON string, escaped like JavaScript's `JSON.stringify` (non-ASCII passes through as UTF-8).
pub fn string(out: &mut Vec<u8>, s: &str) {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let bytes = s.as_bytes();
    out.push(b'"');
    let mut start = 0;
    for (i, &b) in bytes.iter().enumerate() {
        let escape: &[u8] = match b {
            b'"' => br#"\""#,
            b'\\' => br"\\",
            b'\n' => br"\n",
            b'\r' => br"\r",
            b'\t' => br"\t",
            0x08 => br"\b",
            0x0c => br"\f",
            0x00..=0x1f => b"",
            _ => continue,
        };
        out.extend_from_slice(&bytes[start..i]);
        if escape.is_empty() {
            out.extend_from_slice(&[b'\\', b'u', b'0', b'0', HEX[usize::from(b >> 4)], HEX[usize::from(b & 15)]]);
        } else {
            out.extend_from_slice(escape);
        }
        start = i + 1;
    }
    out.extend_from_slice(&bytes[start..]);
    out.push(b'"');
}

pub fn int(out: &mut Vec<u8>, n: impl itoa::Integer) {
    out.extend_from_slice(itoa::Buffer::new().format(n).as_bytes());
}

/// A TIMESTAMPTZ in Postgres's binary format (microseconds since 2000-01-01 UTC) as `2025-12-31T23:59:19.409Z`:
/// UTC, truncated to milliseconds.
pub fn timestamp(out: &mut Vec<u8>, pg_micros: i64) {
    const UNIX_EPOCH_MS: i64 = 946_684_800_000; // 2000-01-01 in Unix milliseconds
    let ms = pg_micros.div_euclid(1000) + UNIX_EPOCH_MS;
    let secs = ms.div_euclid(1000);
    let day_secs = secs.rem_euclid(86_400);
    let (year, month, day) = civil_from_days(secs.div_euclid(86_400));
    digits(out, year, 4);
    out.push(b'-');
    digits(out, month, 2);
    out.push(b'-');
    digits(out, day, 2);
    out.push(b'T');
    digits(out, day_secs / 3600, 2);
    out.push(b':');
    digits(out, day_secs / 60 % 60, 2);
    out.push(b':');
    digits(out, day_secs % 60, 2);
    out.push(b'.');
    digits(out, ms.rem_euclid(1000), 3);
    out.push(b'Z');
}

/// Days since 1970-01-01 to (year, month, day), proleptic Gregorian (Howard Hinnant's algorithm).
fn civil_from_days(days: i64) -> (i64, i64, i64) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    (year, month, day)
}

/// `n` zero-padded to `width` digits.
fn digits(out: &mut Vec<u8>, mut n: i64, width: usize) {
    let mut buf = [b'0'; 4];
    for d in buf[..width].iter_mut().rev() {
        *d = b'0' + (n % 10) as u8;
        n /= 10;
    }
    out.extend_from_slice(&buf[..width]);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ts(pg_micros: i64) -> String {
        let mut out = Vec::new();
        timestamp(&mut out, pg_micros);
        String::from_utf8(out).unwrap()
    }

    #[test]
    fn timestamps() {
        assert_eq!(ts(0), "2000-01-01T00:00:00.000Z");
        // 2025-12-31T23:59:19.409999Z: truncated, not rounded
        assert_eq!(ts(1_767_225_559_409_999 - 946_684_800_000_000), "2025-12-31T23:59:19.409Z");
        // 2024-02-29T12:34:56.789Z
        assert_eq!(ts(1_709_210_096_789_000 - 946_684_800_000_000), "2024-02-29T12:34:56.789Z");
    }

    #[test]
    fn strings() {
        let mut out = Vec::new();
        string(&mut out, "a\"b\\c\nd\u{1}é ✓ </>");
        assert_eq!(String::from_utf8(out).unwrap(), r#""a\"b\\c\nd\u0001é ✓ </>""#);
    }
}
