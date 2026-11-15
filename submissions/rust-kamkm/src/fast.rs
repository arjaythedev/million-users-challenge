//! Response encoding without intermediate values: JSON written straight from
//! PostgreSQL row values, a fixed-Huffman gzip encoder, and the HTTP response
//! head written into the same buffer as the body.
use std::cell::RefCell;
use std::error::Error;

use tokio_postgres::types::{FromSql, Type};
use tokio_postgres::Row;

const HEX: &[u8; 16] = b"0123456789abcdef";

/// Appends `s` as a JSON string with exactly serde_json's escapes (quote,
/// backslash, `\b \f \n \r \t`, other control bytes as `\u00xx`), so responses
/// keep the bytes the serde-based encoder produced.
pub fn push_str(out: &mut Vec<u8>, s: &str) {
    let bytes = s.as_bytes();
    out.reserve(bytes.len() + 2);
    out.push(b'"');
    let mut start = 0;
    for (i, &c) in bytes.iter().enumerate() {
        if c >= 0x20 && c != b'"' && c != b'\\' {
            continue;
        }
        out.extend_from_slice(&bytes[start..i]);
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
        start = i + 1;
    }
    out.extend_from_slice(&bytes[start..]);
    out.push(b'"');
}

pub fn push_int<I: itoa::Integer>(out: &mut Vec<u8>, value: I) {
    out.extend_from_slice(itoa::Buffer::new().format(value).as_bytes());
}

/// A `timestamptz` as PostgreSQL sends it: microseconds since 2000-01-01 UTC.
pub struct PgMicros(pub i64);

impl<'a> FromSql<'a> for PgMicros {
    fn from_sql(_: &Type, raw: &'a [u8]) -> Result<Self, Box<dyn Error + Sync + Send>> {
        let raw: [u8; 8] = raw.try_into()?;
        Ok(PgMicros(i64::from_be_bytes(raw)))
    }

    fn accepts(ty: &Type) -> bool {
        *ty == Type::TIMESTAMPTZ
    }
}

const PG_EPOCH_UNIX_MICROS: i64 = 946_684_800 * 1_000_000;

/// Appends a timestamp as UTC with milliseconds, truncated rather than rounded:
/// `2025-12-31T23:59:19.409Z`. Returns `None` for timestamps outside years
/// 0-9999 (such as `infinity`), which have no such form.
pub fn push_timestamp(out: &mut Vec<u8>, ts: PgMicros) -> Option<()> {
    let micros = ts.0.checked_add(PG_EPOCH_UNIX_MICROS)?;
    let secs = micros.div_euclid(1_000_000);
    let millis = micros.rem_euclid(1_000_000) / 1000;
    let (year, month, day) = civil_from_days(secs.div_euclid(86_400));
    if !(0..=9999).contains(&year) {
        return None;
    }
    let sod = secs.rem_euclid(86_400);
    let (h, m, s) = (sod / 3600, sod / 60 % 60, sod % 60);
    let d = |v: i64| b'0' + v as u8;
    out.extend_from_slice(&[
        d(year / 1000),
        d(year / 100 % 10),
        d(year / 10 % 10),
        d(year % 10),
        b'-',
        d(month / 10),
        d(month % 10),
        b'-',
        d(day / 10),
        d(day % 10),
        b'T',
        d(h / 10),
        d(h % 10),
        b':',
        d(m / 10),
        d(m % 10),
        b':',
        d(s / 10),
        d(s % 10),
        b'.',
        d(millis / 100),
        d(millis / 10 % 10),
        d(millis % 10),
        b'Z',
    ]);
    Some(())
}

/// Proleptic Gregorian date of a day count since 1970-01-01 (Howard Hinnant's
/// `civil_from_days`).
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

/// Appends a post object with the API's key order.
pub fn push_post(
    out: &mut Vec<u8>,
    id: i64,
    body: &str,
    created: PgMicros,
    author: &str,
    likes: i64,
) -> Option<()> {
    out.extend_from_slice(br#"{"id":"#);
    push_int(out, id);
    out.extend_from_slice(br#","body":"#);
    push_str(out, body);
    out.extend_from_slice(br#","created_at":""#);
    push_timestamp(out, created)?;
    out.extend_from_slice(br#"","author":"#);
    push_str(out, author);
    out.extend_from_slice(br#","like_count":"#);
    push_int(out, likes);
    out.push(b'}');
    Some(())
}

/// Appends the post in a row of the spec's post query, reading the text
/// columns in place.
pub fn push_post_row(out: &mut Vec<u8>, row: &Row) -> Option<()> {
    let id: i64 = row.try_get(0).ok()?;
    let body: &str = row.try_get(1).ok()?;
    let created: PgMicros = row.try_get(2).ok()?;
    let author: &str = row.try_get(3).ok()?;
    let likes: i32 = row.try_get(4).ok()?;
    push_post(out, id, body, created, author, i64::from(likes))
}

/// Bodies this long or longer are compressed when the client accepts gzip.
pub const GZIP_MIN_BYTES: usize = 1024;

fn status_line(status: u16) -> &'static [u8] {
    match status {
        200 => b"HTTP/1.1 200 OK\r\n",
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    }
}

thread_local! {
    static GZIP_OUT: RefCell<Vec<u8>> = RefCell::new(Vec::with_capacity(8192));
}

/// The whole HTTP response, head and body, in one buffer for a single write.
pub fn response(status: u16, body: &[u8], gzip: bool, close: bool) -> Vec<u8> {
    let head = |out: &mut Vec<u8>, length: usize, compressed: bool| {
        out.extend_from_slice(status_line(status));
        out.extend_from_slice(b"Content-Type: application/json\r\n");
        if compressed {
            out.extend_from_slice(b"Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n");
        }
        out.extend_from_slice(b"Content-Length: ");
        push_int(out, length);
        out.extend_from_slice(if close {
            b"\r\nConnection: close\r\n\r\n".as_slice()
        } else {
            b"\r\nConnection: keep-alive\r\n\r\n".as_slice()
        });
    };
    if gzip && body.len() >= GZIP_MIN_BYTES {
        GZIP_OUT.with(|scratch| {
            let mut compressed = scratch.borrow_mut();
            compressed.clear();
            gzip_append(&mut compressed, body);
            let mut out = Vec::with_capacity(192 + compressed.len());
            head(&mut out, compressed.len(), true);
            out.extend_from_slice(&compressed);
            out
        })
    } else {
        let mut out = Vec::with_capacity(160 + body.len());
        head(&mut out, body.len(), false);
        out.extend_from_slice(body);
        out
    }
}

// ---------------------------------------------------------------------------
// gzip
//
// Responses are a few KiB of JSON. For inputs that small a general-purpose
// compressor spends much of its time building and choosing Huffman tables for
// each response. This encoder writes one DEFLATE block with the fixed Huffman
// codes of RFC 1951 (section 3.2.6), so there are no tables to build: it finds
// repeats with a single-probe hash table (greedy LZ77, as gzip -1 does) and
// emits each literal or match with a table lookup. The output is standard gzip.

const HASH_BITS: u32 = 13;
const MIN_MATCH: usize = 4;
const MAX_MATCH: usize = 258;
const WINDOW: usize = 1 << 15;

/// Entries store a position plus `base`; raising `base` past the input length
/// on each call makes older entries fall below it, so the table never needs
/// clearing.
struct Encoder {
    table: Box<[i32; 1 << HASH_BITS]>,
    base: i32,
    acc: u64,
    bits: u32,
}

thread_local! {
    static ENCODER: RefCell<Encoder> = RefCell::new(Encoder {
        table: Box::new([0; 1 << HASH_BITS]),
        base: 0,
        acc: 0,
        bits: 0,
    });
}

// Magic, CM=8 (deflate), no flags, no mtime, XFL=4 (fastest), OS=255 (unknown).
const GZIP_HEADER: [u8; 10] = [0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 4, 255];

/// Appends a gzip member holding `src` to `dst`.
pub fn gzip_append(dst: &mut Vec<u8>, src: &[u8]) {
    dst.extend_from_slice(&GZIP_HEADER);
    ENCODER.with(|e| e.borrow_mut().deflate(dst, src));
    dst.extend_from_slice(&crc32fast::hash(src).to_le_bytes());
    dst.extend_from_slice(&(src.len() as u32).to_le_bytes());
}

fn load32(s: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(s[i..i + 4].try_into().unwrap())
}

fn hash4(u: u32) -> usize {
    (u.wrapping_mul(0x1e35_a7bd) >> (32 - HASH_BITS)) as usize
}

/// The number of leading bytes `a` and `b` share, up to `limit`.
fn match_length(a: &[u8], b: &[u8], limit: usize) -> usize {
    let limit = limit.min(b.len());
    let mut n = 0;
    while n + 8 <= limit {
        let x = u64::from_le_bytes(a[n..n + 8].try_into().unwrap())
            ^ u64::from_le_bytes(b[n..n + 8].try_into().unwrap());
        if x != 0 {
            return n + (x.trailing_zeros() / 8) as usize;
        }
        n += 8;
    }
    while n < limit && a[n] == b[n] {
        n += 1;
    }
    n
}

impl Encoder {
    fn deflate(&mut self, out: &mut Vec<u8>, src: &[u8]) {
        if src.len() > 1 << 30 || self.base > 1 << 30 {
            self.table.fill(0);
            self.base = 0;
        }
        let base = self.base + 1;
        self.base += src.len() as i32 + 1;
        self.acc = 0;
        self.bits = 0;
        self.put(out, 3, 3); // BFINAL=1, BTYPE=01 (fixed Huffman)

        let n = src.len();
        let mut literals = 0;
        let mut i = 0;
        while i + MIN_MATCH <= n {
            let current = load32(src, i);
            let h = hash4(current);
            let candidate = self.table[h] - base;
            self.table[h] = i as i32 + base;
            if candidate >= 0 {
                let candidate = candidate as usize;
                if i - candidate <= WINDOW && load32(src, candidate) == current {
                    let length = MIN_MATCH
                        + match_length(
                            &src[candidate + MIN_MATCH..],
                            &src[i + MIN_MATCH..],
                            MAX_MATCH - MIN_MATCH,
                        );
                    self.literals(out, &src[literals..i]);
                    self.emit_match(out, length, i - candidate);
                    i += length;
                    literals = i;
                    // Index the position just before the next one so a run of
                    // repeats is found again at once.
                    if i - 1 + MIN_MATCH <= n {
                        self.table[hash4(load32(src, i - 1))] = (i - 1) as i32 + base;
                    }
                    continue;
                }
            }
            // Skip ahead faster through data that has not matched for a while.
            i += 1 + ((i - literals) >> 5);
        }
        self.literals(out, &src[literals..]);
        self.put(out, 0, 7); // end of block (symbol 256)
        while self.bits > 0 {
            out.push(self.acc as u8);
            self.acc >>= 8;
            self.bits = self.bits.saturating_sub(8);
        }
    }

    fn put(&mut self, out: &mut Vec<u8>, value: u64, n: u32) {
        self.acc |= value << self.bits;
        self.bits += n;
        if self.bits >= 32 {
            out.extend_from_slice(&(self.acc as u32).to_le_bytes());
            self.acc >>= 32;
            self.bits -= 32;
        }
    }

    fn literals(&mut self, out: &mut Vec<u8>, s: &[u8]) {
        for &c in s {
            let code = LITERAL_CODES[c as usize];
            self.put(out, u64::from(code & 0xffff), code >> 16);
        }
    }

    /// A length/distance pair: the length symbol and its extra bits, then the
    /// distance code and its extra bits.
    fn emit_match(&mut self, out: &mut Vec<u8>, length: usize, distance: usize) {
        let code = LENGTH_CODES[length - MIN_MATCH];
        self.put(out, u64::from(code & 0xffff), code >> 16);
        let d = (distance - 1) as u32;
        let (symbol, extra, extra_bits) = if d < 4 {
            (d, 0, 0)
        } else {
            let top = 31 - d.leading_zeros();
            let extra_bits = top - 1;
            (
                2 * top + ((d >> extra_bits) & 1),
                d & ((1 << extra_bits) - 1),
                extra_bits,
            )
        };
        self.put(
            out,
            u64::from(reverse_bits(symbol, 5)) | u64::from(extra) << 5,
            5 + extra_bits,
        );
    }
}

/// Each fixed code, bit-reversed for the LSB-first bit stream and followed by
/// any extra bits, in the low 16 bits, with its total bit count above.
static LITERAL_CODES: [u32; 256] = literal_codes();
static LENGTH_CODES: [u32; MAX_MATCH - MIN_MATCH + 1] = length_codes();

const fn reverse_bits(v: u32, n: u32) -> u32 {
    v.reverse_bits() >> (32 - n)
}

const fn literal_codes() -> [u32; 256] {
    let mut codes = [0; 256];
    let mut c = 0;
    while c < 256 {
        codes[c] = if c < 144 {
            8 << 16 | reverse_bits(0x30 + c as u32, 8)
        } else {
            9 << 16 | reverse_bits(0x190 + c as u32 - 144, 9)
        };
        c += 1;
    }
    codes
}

const fn length_codes() -> [u32; MAX_MATCH - MIN_MATCH + 1] {
    // Length symbols 257-285: base lengths and extra bit counts (RFC 1951, 3.2.5).
    const BASES: [usize; 29] = [
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
        131, 163, 195, 227, 258,
    ];
    const EXTRAS: [u32; 29] = [
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    ];
    let mut codes = [0; MAX_MATCH - MIN_MATCH + 1];
    let mut l = MIN_MATCH;
    while l <= MAX_MATCH {
        let mut s = 28;
        while BASES[s] > l {
            s -= 1;
        }
        let symbol = 257 + s as u32;
        let (code, n) = if symbol < 280 {
            (reverse_bits(symbol - 256, 7), 7)
        } else {
            (reverse_bits(0xc0 + symbol - 280, 8), 8)
        };
        codes[l - MIN_MATCH] = (n + EXTRAS[s]) << 16 | code | ((l - BASES[s]) as u32) << n;
        l += 1;
    }
    codes
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    fn gunzip(data: &[u8]) -> Vec<u8> {
        let mut decoder = flate2::read::GzDecoder::new(data);
        let mut out = Vec::new();
        decoder.read_to_end(&mut out).expect("valid gzip");
        let mut rest = Vec::new();
        decoder.into_inner().read_to_end(&mut rest).unwrap();
        assert!(rest.is_empty(), "trailing data after the gzip member");
        out
    }

    fn round_trip(body: &[u8]) -> Vec<u8> {
        let mut out = b"prefix".to_vec();
        gzip_append(&mut out, body);
        assert!(out.starts_with(b"prefix"));
        assert_eq!(
            gunzip(&out[6..]),
            body,
            "round trip of {} bytes",
            body.len()
        );
        out[6..].to_vec()
    }

    struct Rng(u64);
    impl Rng {
        fn next(&mut self) -> u64 {
            self.0 ^= self.0 << 13;
            self.0 ^= self.0 >> 7;
            self.0 ^= self.0 << 17;
            self.0
        }
        fn byte(&mut self) -> u8 {
            self.next() as u8
        }
    }

    fn feed_like(posts: usize) -> Vec<u8> {
        let mut b = br#"{"posts":["#.to_vec();
        for i in 0..posts {
            if i > 0 {
                b.push(b',');
            }
            let text = format!("word{} ", i % 7).repeat(3 + i % 11);
            push_post(
                &mut b,
                500_000 - i as i64,
                &text,
                PgMicros(820_000_000_000_000 + i as i64 * 1000),
                &format!("user{}", 1000 + i),
                (i % 5) as i64,
            )
            .unwrap();
        }
        b.extend_from_slice(b"]}");
        b
    }

    #[test]
    fn gzip_round_trips() {
        let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
        let mut inputs: Vec<Vec<u8>> = vec![
            vec![],
            b"a".to_vec(),
            b"abcd".to_vec(),
            vec![b'a'; 58],
            vec![b'x'; 100_000],
            b"0123456789".repeat(10_000),
            feed_like(20),
            feed_like(200),
            (0..1024).map(|i| i as u8).collect(),
        ];
        for size in [1, 7, 64, 1000, 4096, 40_000, 70_000] {
            inputs.push((0..size).map(|_| rng.byte()).collect());
            inputs.push(
                (0..size)
                    .map(|_| b"ab"[(rng.next() & 1) as usize])
                    .collect(),
            );
        }
        // A repeat just inside and just outside the 32 KiB window.
        for gap in [WINDOW - 4, WINDOW - 1, WINDOW, WINDOW + 1] {
            let mut b: Vec<u8> = (0..gap + 64).map(|_| rng.byte()).collect();
            let head = b[..64].to_vec();
            b[gap..].copy_from_slice(&head);
            inputs.push(b);
        }
        for body in &inputs {
            round_trip(body);
        }
        // Encoders are reused per thread: a reused encoder must not match into
        // earlier input.
        for i in 0..50 {
            round_trip(&feed_like(1 + i % 20));
        }
    }

    #[test]
    fn gzip_every_length_and_distance() {
        let mut rng = Rng(42);
        for distance in [
            1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 16, 17, 24, 25, 100, 1000, 5000, 20_000, 32_767,
            32_768,
        ] {
            let mut length = MIN_MATCH;
            while length <= MAX_MATCH + 2 {
                let mut b: Vec<u8> = (0..distance).map(|_| rng.byte()).collect();
                for i in distance..distance + length {
                    b.push(b[i - distance]);
                }
                b.extend((0..8).map(|_| rng.byte()));
                round_trip(&b);
                length += 1 + length / 32;
            }
        }
    }

    #[test]
    fn gzip_compresses_a_feed() {
        let body = feed_like(20);
        let out = round_trip(&body);
        assert!(
            out.len() * 2 < body.len(),
            "{} -> {}",
            body.len(),
            out.len()
        );
    }

    #[test]
    fn strings_match_serde_json() {
        for c in 0u8..128 {
            let s = format!("a{}b", c as char);
            let mut ours = Vec::new();
            push_str(&mut ours, &s);
            assert_eq!(ours, serde_json::to_vec(&s).unwrap(), "byte {c}");
        }
        let s = "plain \"quoted\" back\\slash\nnew\ttab é ü 中文 \u{1f600} \u{7f} </script>";
        let mut ours = Vec::new();
        push_str(&mut ours, s);
        assert_eq!(ours, serde_json::to_vec(&s).unwrap());
    }

    #[test]
    fn timestamps_match_chrono() {
        use chrono::{DateTime, SecondsFormat, Utc};
        let mut rng = Rng(7);
        let mut cases = vec![
            0i64,
            1,
            999,
            1000,
            -1,
            -999_999,
            -PG_EPOCH_UNIX_MICROS,
            1_767_225_599_409_123 - PG_EPOCH_UNIX_MICROS,
        ];
        // 2024-02-29 and 2100-03-01 cover leap-year rules.
        cases.push(1_709_164_800_000_000 - PG_EPOCH_UNIX_MICROS);
        cases.push(4_107_542_400_000_000 - PG_EPOCH_UNIX_MICROS);
        for _ in 0..100_000 {
            // Anywhere from 1970 to about 2100.
            cases.push((rng.next() % 4_102_444_800_000_000) as i64 - PG_EPOCH_UNIX_MICROS);
        }
        for pg in cases {
            let mut ours = Vec::new();
            push_timestamp(&mut ours, PgMicros(pg)).unwrap();
            let theirs = DateTime::<Utc>::from_timestamp_micros(pg + PG_EPOCH_UNIX_MICROS)
                .unwrap()
                .to_rfc3339_opts(SecondsFormat::Millis, true);
            assert_eq!(String::from_utf8(ours).unwrap(), theirs, "{pg}");
        }
        let mut out = Vec::new();
        assert!(push_timestamp(&mut out, PgMicros(i64::MAX)).is_none());
    }

    #[test]
    fn response_head() {
        let wire = response(404, br#"{"error":"not found"}"#, true, false);
        assert_eq!(
            wire,
            b"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 21\r\nConnection: keep-alive\r\n\r\n{\"error\":\"not found\"}".to_vec()
        );
        let body = feed_like(20);
        let wire = response(200, &body, true, true);
        let split = wire.windows(4).position(|w| w == b"\r\n\r\n").unwrap() + 4;
        let head = std::str::from_utf8(&wire[..split]).unwrap();
        assert!(
            head.contains("Content-Encoding: gzip\r\n") && head.contains("Connection: close\r\n"),
            "{head}"
        );
        assert!(head.contains(&format!("Content-Length: {}\r\n", wire.len() - split)));
        assert_eq!(gunzip(&wire[split..]), body);
    }
}
