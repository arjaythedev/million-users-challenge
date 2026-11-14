static DIG2: &[u8; 200] = b"00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";

#[inline]
pub fn push2(out: &mut Vec<u8>, v: u32) {
    let i = (v as usize) * 2;
    out.extend_from_slice(&DIG2[i..i + 2]);
}

pub fn itoa(out: &mut Vec<u8>, v: i64) {
    let mut buf = [0u8; 20];
    let mut i = 20;
    let neg = v < 0;
    let mut u = v.unsigned_abs();
    while u >= 100 {
        let r = (u % 100) as usize * 2;
        u /= 100;
        i -= 2;
        buf[i..i + 2].copy_from_slice(&DIG2[r..r + 2]);
    }
    if u >= 10 {
        let r = u as usize * 2;
        i -= 2;
        buf[i..i + 2].copy_from_slice(&DIG2[r..r + 2]);
    } else {
        i -= 1;
        buf[i] = b'0' + u as u8;
    }
    if neg {
        out.push(b'-');
    }
    out.extend_from_slice(&buf[i..]);
}

pub struct DayCache {
    day: i64,
    ymd: [u8; 11],
}

impl DayCache {
    pub fn new() -> Self {
        DayCache { day: i64::MIN, ymd: [0; 11] }
    }
}

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = (z - era * 146097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

const PG_EPOCH_DAYS: i64 = 10957;

pub fn timestamp(out: &mut Vec<u8>, pg_us: i64, dc: &mut DayCache) {
    let ms = pg_us.div_euclid(1000);
    let secs = ms.div_euclid(1000);
    let msr = ms.rem_euclid(1000) as u32;
    let day = secs.div_euclid(86400) + PG_EPOCH_DAYS;
    let sod = secs.rem_euclid(86400) as u32;
    if day != dc.day {
        let (y, m, d) = civil_from_days(day);
        let y = y.clamp(0, 9999) as u32;
        let mut v = Vec::with_capacity(11);
        push2(&mut v, y / 100);
        push2(&mut v, y % 100);
        v.push(b'-');
        push2(&mut v, m);
        v.push(b'-');
        push2(&mut v, d);
        v.push(b'T');
        dc.ymd.copy_from_slice(&v);
        dc.day = day;
    }
    out.extend_from_slice(&dc.ymd);
    push2(out, sod / 3600);
    out.push(b':');
    push2(out, (sod / 60) % 60);
    out.push(b':');
    push2(out, sod % 60);
    out.push(b'.');
    out.push(b'0' + (msr / 100) as u8);
    push2(out, msr % 100);
    out.push(b'Z');
}

#[inline]
fn needs_escape(b: u8) -> bool {
    b < 0x20 || b == b'"' || b == b'\\'
}

#[cfg(target_arch = "aarch64")]
#[inline]
pub fn find_escape(s: &[u8]) -> usize {
    use std::arch::aarch64::*;
    let mut i = 0;
    unsafe {
        let lim = vdupq_n_u8(0x20);
        let q = vdupq_n_u8(b'"');
        let bs = vdupq_n_u8(b'\\');
        while i + 16 <= s.len() {
            let v = vld1q_u8(s.as_ptr().add(i));
            let m = vorrq_u8(vorrq_u8(vcltq_u8(v, lim), vceqq_u8(v, q)), vceqq_u8(v, bs));
            let nib = vget_lane_u64::<0>(vreinterpret_u64_u8(vshrn_n_u16::<4>(vreinterpretq_u16_u8(m))));
            if nib != 0 {
                return i + (nib.trailing_zeros() >> 2) as usize;
            }
            i += 16;
        }
    }
    while i < s.len() {
        if needs_escape(s[i]) {
            return i;
        }
        i += 1;
    }
    s.len()
}

#[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
#[inline]
pub fn find_escape(s: &[u8]) -> usize {
    use std::arch::x86_64::*;
    let mut i = 0;
    unsafe {
        let lim = _mm256_set1_epi8(0x1f);
        let q = _mm256_set1_epi8(b'"' as i8);
        let bs = _mm256_set1_epi8(b'\\' as i8);
        while i + 32 <= s.len() {
            let v = _mm256_loadu_si256(s.as_ptr().add(i) as *const __m256i);
            let ctl = _mm256_cmpeq_epi8(_mm256_max_epu8(v, lim), lim);
            let m = _mm256_or_si256(_mm256_or_si256(ctl, _mm256_cmpeq_epi8(v, q)), _mm256_cmpeq_epi8(v, bs));
            let bits = _mm256_movemask_epi8(m) as u32;
            if bits != 0 {
                return i + bits.trailing_zeros() as usize;
            }
            i += 32;
        }
    }
    while i < s.len() {
        if needs_escape(s[i]) {
            return i;
        }
        i += 1;
    }
    s.len()
}

#[cfg(all(target_arch = "x86_64", not(target_feature = "avx2")))]
#[inline]
pub fn find_escape(s: &[u8]) -> usize {
    use std::arch::x86_64::*;
    let mut i = 0;
    unsafe {
        let lim = _mm_set1_epi8(0x1f);
        let q = _mm_set1_epi8(b'"' as i8);
        let bs = _mm_set1_epi8(b'\\' as i8);
        while i + 16 <= s.len() {
            let v = _mm_loadu_si128(s.as_ptr().add(i) as *const __m128i);
            let ctl = _mm_cmpeq_epi8(_mm_max_epu8(v, lim), lim);
            let m = _mm_or_si128(_mm_or_si128(ctl, _mm_cmpeq_epi8(v, q)), _mm_cmpeq_epi8(v, bs));
            let bits = _mm_movemask_epi8(m) as u32;
            if bits != 0 {
                return i + bits.trailing_zeros() as usize;
            }
            i += 16;
        }
    }
    while i < s.len() {
        if needs_escape(s[i]) {
            return i;
        }
        i += 1;
    }
    s.len()
}

#[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
pub fn find_escape(s: &[u8]) -> usize {
    s.iter().position(|&b| needs_escape(b)).unwrap_or(s.len())
}

static HEX: &[u8; 16] = b"0123456789abcdef";

pub fn escape(out: &mut Vec<u8>, s: &[u8]) {
    out.reserve(s.len() + 8);
    let mut start = 0;
    loop {
        let j = start + find_escape(&s[start..]);
        if j >= s.len() {
            out.extend_from_slice(&s[start..]);
            return;
        }
        out.extend_from_slice(&s[start..j]);
        let b = s[j];
        match b {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            0x08 => out.extend_from_slice(b"\\b"),
            0x0c => out.extend_from_slice(b"\\f"),
            _ => {
                out.extend_from_slice(b"\\u00");
                out.push(HEX[(b >> 4) as usize]);
                out.push(HEX[(b & 15) as usize]);
            }
        }
        start = j + 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ts() {
        let mut dc = DayCache::new();
        let mut o = Vec::new();
        timestamp(&mut o, 0, &mut dc);
        assert_eq!(o, b"2000-01-01T00:00:00.000Z");
        o.clear();
        timestamp(&mut o, -1, &mut dc);
        assert_eq!(o, b"1999-12-31T23:59:59.999Z");
        o.clear();
        let us = (1767225559i64 - 946684800) * 1_000_000 + 409_999;
        timestamp(&mut o, us, &mut dc);
        assert_eq!(o, b"2025-12-31T23:59:19.409Z");
        o.clear();
        let us = (1709210096i64 - 946684800) * 1_000_000 + 7_000;
        timestamp(&mut o, us, &mut dc);
        assert_eq!(o, b"2024-02-29T12:34:56.007Z");
    }

    #[test]
    fn esc() {
        let mut o = Vec::new();
        let s = "a\"b\\c\nd\u{1}é ✓ long string with no escapes at all here, really long 0123456789\"".as_bytes();
        escape(&mut o, s);
        assert_eq!(
            std::str::from_utf8(&o).unwrap(),
            "a\\\"b\\\\c\\nd\\u0001é ✓ long string with no escapes at all here, really long 0123456789\\\""
        );
        for n in 0..80 {
            let mut v = vec![b'x'; n];
            for k in 0..n {
                v[k] = b'"';
                assert_eq!(find_escape(&v), k);
                v[k] = b'x';
            }
            assert_eq!(find_escape(&v), n);
        }
        let mut o = Vec::new();
        itoa(&mut o, 0);
        o.push(b' ');
        itoa(&mut o, 9223372036854775807);
        o.push(b' ');
        itoa(&mut o, -45);
        assert_eq!(o, b"0 9223372036854775807 -45");
    }
}
