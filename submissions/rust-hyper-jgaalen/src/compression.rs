//! Per-response compression only: never retain response data between requests.
use bytes::Bytes;
use flate2::{Compression, write::GzEncoder};
use std::{
    io::{self, Write},
    sync::OnceLock,
};

// Conservative parsing: malformed or conflicting qualities disable that coding.
fn quality(value: &str) -> Option<u16> {
    let (whole, frac) = value.split_once('.').unwrap_or((value, ""));
    if frac.len() > 3 || !frac.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    match whole {
        "0" => Some(frac.parse::<u16>().unwrap_or(0) * 10u16.pow((3 - frac.len()) as u32)),
        "1" if frac.bytes().all(|b| b == b'0') => Some(1000),
        _ => None,
    }
}
pub fn accepted(headers: &[httparse::Header<'_>]) -> bool {
    let mut explicit: Option<u16> = None;
    let mut wildcard: Option<u16> = None;
    for h in headers
        .iter()
        .filter(|h| h.name.eq_ignore_ascii_case("accept-encoding"))
    {
        let Ok(value) = std::str::from_utf8(h.value) else {
            return false;
        };
        for item in value.split(',') {
            let mut parts = item.split(';');
            let coding = parts.next().unwrap_or("").trim();
            let target = if coding.eq_ignore_ascii_case("gzip") {
                &mut explicit
            } else if coding == "*" {
                &mut wildcard
            } else {
                continue;
            };
            let mut q = 1000;
            if let Some(param) = parts.next() {
                q = if parts.next().is_none() {
                    param
                        .trim()
                        .split_once('=')
                        .filter(|(k, _)| k.trim().eq_ignore_ascii_case("q"))
                        .and_then(|(_, v)| quality(v.trim()))
                        .unwrap_or(0)
                } else {
                    0
                };
            }
            *target = Some(target.map_or(q, |old| old.min(q)));
        }
    }
    explicit.or(wildcard).unwrap_or(0) > 0
}
pub fn encode(body: Bytes, allowed: bool) -> io::Result<(Bytes, bool)> {
    static MIN_BYTES: OnceLock<usize> = OnceLock::new();
    let min = *MIN_BYTES.get_or_init(|| {
        std::env::var("GZIP_MIN_BYTES")
            .ok()
            .map(|v| {
                v.parse()
                    .expect("GZIP_MIN_BYTES must be an unsigned integer")
            })
            .unwrap_or(0)
    });
    encode_above(body, allowed, min)
}
fn encode_above(body: Bytes, allowed: bool, min: usize) -> io::Result<(Bytes, bool)> {
    if !allowed || min == 0 || body.len() < min {
        return Ok((body, false));
    }
    let mut encoder = GzEncoder::new(Vec::with_capacity(body.len() / 2), Compression::fast());
    encoder.write_all(&body)?;
    let encoded = encoder.finish()?;
    if encoded.len() >= body.len() {
        return Ok((body, false));
    }
    Ok((Bytes::from(encoded), true))
}
#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;
    #[test]
    fn negotiation() {
        for (value, yes) in [
            ("gzip", true),
            ("GZip; q=0.500", true),
            ("gzip;q=0", false),
            ("*;q=1, gzip;q=0", false),
            ("gzip;q=0,*", false),
            ("br, *", true),
            ("gzip;q=bogus", false),
            ("gzip;q=1.001", false),
            ("gzip;q=0.0001", false),
            ("gzip;q=1;q=0", false),
            ("gzip;q=0,gzip", false),
            ("", false),
        ] {
            assert_eq!(
                accepted(&[httparse::Header {
                    name: "Accept-Encoding",
                    value: value.as_bytes()
                }]),
                yes,
                "{value}"
            );
        }
        assert!(!accepted(&[]));
        assert!(!accepted(&[
            httparse::Header {
                name: "Accept-Encoding",
                value: b"gzip"
            },
            httparse::Header {
                name: "accept-encoding",
                value: b"gzip;q=0"
            }
        ]));
    }
    #[test]
    fn lossless_threshold_and_unprofitable_fallback() {
        let body = Bytes::from("{\"text\":\"Unicode café 🦀\\n\"}".repeat(100));
        let (encoded, yes) = encode_above(body.clone(), true, 1024).unwrap();
        assert!(yes && encoded.len() < body.len());
        let mut decoded = Vec::new();
        flate2::read::GzDecoder::new(&encoded[..])
            .read_to_end(&mut decoded)
            .unwrap();
        assert_eq!(decoded, body);
        for (allowed, min) in [(false, 1), (true, 0), (true, body.len() + 1)] {
            assert_eq!(
                encode_above(body.clone(), allowed, min).unwrap(),
                (body.clone(), false)
            );
        }
        assert_eq!(
            encode_above(Bytes::from_static(b"{}"), true, 1).unwrap(),
            (Bytes::from_static(b"{}"), false)
        );
    }
}
