const DIGITS_200: &[u8; 200] = b"\
0001020304050607080910111213141516171819\
2021222324252627282930313233343536373839\
4041424344454647484950515253545556575859\
6061626364656667686970717273747576777879\
8081828384858687888990919293949596979899";

const HEX: &[u8; 16] = b"0123456789abcdef";

#[inline(always)]
pub fn write_u64(out: &mut Vec<u8>, mut v: u64) {
    if v < 10 {
        out.push(b'0' + v as u8);
        return;
    }
    let mut buf = [0u8; 20];
    let mut idx = 20usize;
    while v >= 100 {
        let rem = ((v % 100) as usize) * 2;
        v /= 100;
        idx -= 2;
        buf[idx] = DIGITS_200[rem];
        buf[idx + 1] = DIGITS_200[rem + 1];
    }
    if v < 10 {
        idx -= 1;
        buf[idx] = b'0' + v as u8;
    } else {
        let rem = (v as usize) * 2;
        idx -= 2;
        buf[idx] = DIGITS_200[rem];
        buf[idx + 1] = DIGITS_200[rem + 1];
    }
    out.extend_from_slice(&buf[idx..]);
}

#[cfg(test)]
#[inline(always)]
pub fn write_i64(out: &mut Vec<u8>, v: i64) {
    if v < 0 {
        out.push(b'-');
        write_u64(out, v.wrapping_neg() as u64);
    } else {
        write_u64(out, v as u64);
    }
}

#[derive(Clone, Copy)]
pub struct DayPrefixCache {
    day: i64,
    w0_le: u64,
    d_t_le: u64,
}

impl DayPrefixCache {
    #[inline(always)]
    pub const fn new() -> Self {
        Self {
            day: i64::MIN,
            w0_le: 0,
            d_t_le: 0,
        }
    }
}

#[inline(always)]
fn digit_pair_le(idx2: usize) -> u64 {
    u16::from_le_bytes([DIGITS_200[idx2], DIGITS_200[idx2 + 1]]) as u64
}

#[inline(always)]
fn format_civil_day(days_since_epoch: i64, cache: &mut DayPrefixCache) {
    let z = days_since_epoch + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u32;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y_base = (yoe as i64) + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as usize;
    let m = (if mp < 10 { mp + 3 } else { mp - 9 }) as usize;
    let y = (y_base + if m <= 2 { 1 } else { 0 }).clamp(0, 9999) as usize;

    let yh = (y / 100) * 2;
    let yl = (y % 100) * 2;
    let mm = m * 2;
    let dd = d * 2;

    cache.w0_le = u64::from_le_bytes([
        DIGITS_200[yh],
        DIGITS_200[yh + 1],
        DIGITS_200[yl],
        DIGITS_200[yl + 1],
        b'-',
        DIGITS_200[mm],
        DIGITS_200[mm + 1],
        b'-',
    ]);
    cache.d_t_le = digit_pair_le(dd) | ((b'T' as u64) << 16);
    cache.day = days_since_epoch;
}

#[cfg(test)]
#[inline(always)]
pub fn write_json_escaped(out: &mut Vec<u8>, s: &[u8]) {
    if s.is_empty() {
        return;
    }
    let cur = out.len();
    let max_add = s.len() * 6;
    if out.capacity() - cur < max_add {
        out.reserve(max_add);
    }
    unsafe {
        let base = out.as_mut_ptr();
        let end = write_json_escaped_ptr(base.add(cur), s);
        out.set_len((end as usize) - (base as usize));
    }
}

#[inline(always)]
pub unsafe fn write_u64_ptr(ptr: *mut u8, mut v: u64) -> *mut u8 {
    if v < 10 {
        *ptr = b'0' + v as u8;
        return ptr.add(1);
    }
    if v < 100 {
        let rem = (v as usize) * 2;
        *ptr = DIGITS_200[rem];
        *ptr.add(1) = DIGITS_200[rem + 1];
        return ptr.add(2);
    }
    let mut buf = [0u8; 20];
    let mut idx = 20usize;
    while v >= 100 {
        let rem = ((v % 100) as usize) * 2;
        v /= 100;
        idx -= 2;
        buf[idx] = DIGITS_200[rem];
        buf[idx + 1] = DIGITS_200[rem + 1];
    }
    if v < 10 {
        idx -= 1;
        buf[idx] = b'0' + v as u8;
    } else {
        let rem = (v as usize) * 2;
        idx -= 2;
        buf[idx] = DIGITS_200[rem];
        buf[idx + 1] = DIGITS_200[rem + 1];
    }
    let len = 20 - idx;
    std::ptr::copy_nonoverlapping(buf.as_ptr().add(idx), ptr, len);
    ptr.add(len)
}

#[inline(always)]
pub unsafe fn write_i64_ptr(mut ptr: *mut u8, v: i64) -> *mut u8 {
    if v < 0 {
        *ptr = b'-';
        ptr = ptr.add(1);
        write_u64_ptr(ptr, v.wrapping_neg() as u64)
    } else {
        write_u64_ptr(ptr, v as u64)
    }
}

#[inline(always)]
pub unsafe fn write_pg_timestamptz_ptr(
    ptr: *mut u8,
    pg_us: i64,
    cache: &mut DayPrefixCache,
) -> *mut u8 {
    let mut sec = pg_us / 1_000_000;
    let mut rem_us = pg_us % 1_000_000;
    if rem_us < 0 {
        rem_us += 1_000_000;
        sec -= 1;
    }
    let unix_sec = sec + 946_684_800;
    let mut days = unix_sec / 86_400;
    let mut sod = unix_sec % 86_400;
    if sod < 0 {
        sod += 86_400;
        days -= 1;
    }

    if days != cache.day {
        format_civil_day(days, cache);
    }

    let sod_u = sod as u32;
    let hh = ((sod_u / 3600) as usize) * 2;
    let rem_s = sod_u % 3600;
    let mi = ((rem_s / 60) as usize) * 2;
    let ss = ((rem_s % 60) as usize) * 2;
    let ms = (rem_us as u32) / 1000;
    let ms_h = (ms / 100) as u8 + b'0';
    let ms_l = ((ms % 100) as usize) * 2;

    let w1_le =
        cache.d_t_le | (digit_pair_le(hh) << 24) | ((b':' as u64) << 40) | (digit_pair_le(mi) << 48);
    let w2_le = (b':' as u64)
        | (digit_pair_le(ss) << 8)
        | ((b'.' as u64) << 24)
        | ((ms_h as u64) << 32)
        | (digit_pair_le(ms_l) << 40)
        | ((b'Z' as u64) << 56);

    (ptr as *mut u64).write_unaligned(cache.w0_le.to_le());
    (ptr.add(8) as *mut u64).write_unaligned(w1_le.to_le());
    (ptr.add(16) as *mut u64).write_unaligned(w2_le.to_le());
    ptr.add(24)
}

#[cold]
#[inline(never)]
unsafe fn emit_escaped_byte_ptr(ptr: *mut u8, c: u8) -> *mut u8 {
    match c {
        b'"' => {
            *ptr = b'\\';
            *ptr.add(1) = b'"';
            ptr.add(2)
        }
        b'\\' => {
            *ptr = b'\\';
            *ptr.add(1) = b'\\';
            ptr.add(2)
        }
        b'\n' => {
            *ptr = b'\\';
            *ptr.add(1) = b'n';
            ptr.add(2)
        }
        b'\r' => {
            *ptr = b'\\';
            *ptr.add(1) = b'r';
            ptr.add(2)
        }
        b'\t' => {
            *ptr = b'\\';
            *ptr.add(1) = b't';
            ptr.add(2)
        }
        b'\x08' => {
            *ptr = b'\\';
            *ptr.add(1) = b'b';
            ptr.add(2)
        }
        b'\x0c' => {
            *ptr = b'\\';
            *ptr.add(1) = b'f';
            ptr.add(2)
        }
        _ => {
            *ptr = b'\\';
            *ptr.add(1) = b'u';
            *ptr.add(2) = b'0';
            *ptr.add(3) = b'0';
            *ptr.add(4) = HEX[(c >> 4) as usize];
            *ptr.add(5) = HEX[(c & 15) as usize];
            ptr.add(6)
        }
    }
}

#[inline(always)]
pub unsafe fn write_json_escaped_ptr(mut ptr: *mut u8, s: &[u8]) -> *mut u8 {
    let n = s.len();
    let mut i = 0usize;

    #[cfg(all(
        target_arch = "x86_64",
        target_feature = "avx512bw",
        target_feature = "vaes"
    ))]
    {
        use std::arch::x86_64::*;
        let v_quote = _mm512_set1_epi8(b'"' as i8);
        let v_bslash = _mm512_set1_epi8(b'\\' as i8);
        let v_space = _mm512_set1_epi8(0x20);

        while i + 64 <= n {
            let chunk = _mm512_loadu_si512(s.as_ptr().add(i) as *const _);
            _mm512_storeu_si512(ptr as *mut _, chunk);
            let m_q = _mm512_cmpeq_epi8_mask(chunk, v_quote);
            let m_b = _mm512_cmpeq_epi8_mask(chunk, v_bslash);
            let m_c = _mm512_cmplt_epu8_mask(chunk, v_space);
            let mask = m_q | m_b | m_c;
            if mask == 0 {
                ptr = ptr.add(64);
                i += 64;
                continue;
            }
            std::hint::cold_path();
            let first = mask.trailing_zeros() as usize;
            ptr = emit_escaped_byte_ptr(ptr.add(first), *s.get_unchecked(i + first));
            i += first + 1;
        }
    }

    #[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
    {
        use std::arch::x86_64::*;
        let v_quote = _mm256_set1_epi8(b'"' as i8);
        let v_bslash = _mm256_set1_epi8(b'\\' as i8);
        let v_ctrl_bias = _mm256_set1_epi8(-128i8);
        let v_ctrl_lim = _mm256_set1_epi8((0x20i16 - 128) as i8);

        while i + 32 <= n {
            let chunk = _mm256_loadu_si256(s.as_ptr().add(i) as *const __m256i);
            _mm256_storeu_si256(ptr as *mut __m256i, chunk);
            let eq_q = _mm256_cmpeq_epi8(chunk, v_quote);
            let eq_b = _mm256_cmpeq_epi8(chunk, v_bslash);
            let biased = _mm256_add_epi8(chunk, v_ctrl_bias);
            let lt_ctrl = _mm256_cmpgt_epi8(v_ctrl_lim, biased);
            let hits = _mm256_or_si256(_mm256_or_si256(eq_q, eq_b), lt_ctrl);
            let mask = _mm256_movemask_epi8(hits) as u32;
            if mask == 0 {
                ptr = ptr.add(32);
                i += 32;
                continue;
            }
            std::hint::cold_path();
            let first = mask.trailing_zeros() as usize;
            ptr = emit_escaped_byte_ptr(ptr.add(first), *s.get_unchecked(i + first));
            i += first + 1;
        }
    }

    #[cfg(target_arch = "x86_64")]
    {
        use std::arch::x86_64::*;
        let v_quote = _mm_set1_epi8(b'"' as i8);
        let v_bslash = _mm_set1_epi8(b'\\' as i8);
        let v_ctrl_bias = _mm_set1_epi8(-128i8);
        let v_ctrl_lim = _mm_set1_epi8((0x20i16 - 128) as i8);

        while i + 16 <= n {
            let chunk = _mm_loadu_si128(s.as_ptr().add(i) as *const __m128i);
            _mm_storeu_si128(ptr as *mut __m128i, chunk);
            let eq_q = _mm_cmpeq_epi8(chunk, v_quote);
            let eq_b = _mm_cmpeq_epi8(chunk, v_bslash);
            let biased = _mm_add_epi8(chunk, v_ctrl_bias);
            let lt_ctrl = _mm_cmpgt_epi8(v_ctrl_lim, biased);
            let hits = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_ctrl);
            let mask = _mm_movemask_epi8(hits) as u32;
            if mask == 0 {
                ptr = ptr.add(16);
                i += 16;
                continue;
            }
            std::hint::cold_path();
            let first = mask.trailing_zeros() as usize;
            ptr = emit_escaped_byte_ptr(ptr.add(first), *s.get_unchecked(i + first));
            i += first + 1;
        }

        while i + 8 <= n {
            let raw8 = std::ptr::read_unaligned(s.as_ptr().add(i) as *const u64);
            std::ptr::write_unaligned(ptr as *mut u64, raw8);
            let chunk = _mm_cvtsi64_si128(raw8 as i64);
            let eq_q = _mm_cmpeq_epi8(chunk, v_quote);
            let eq_b = _mm_cmpeq_epi8(chunk, v_bslash);
            let biased = _mm_add_epi8(chunk, v_ctrl_bias);
            let lt_ctrl = _mm_cmpgt_epi8(v_ctrl_lim, biased);
            let hits = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_ctrl);
            let mask = (_mm_movemask_epi8(hits) as u32) & 0xFF;
            if mask == 0 {
                ptr = ptr.add(8);
                i += 8;
                continue;
            }
            std::hint::cold_path();
            let first = mask.trailing_zeros() as usize;
            ptr = emit_escaped_byte_ptr(ptr.add(first), *s.get_unchecked(i + first));
            i += first + 1;
        }
    }

    while i < n {
        let c = *s.get_unchecked(i);
        if c >= 0x20 && c != b'"' && c != b'\\' {
            *ptr = c;
            ptr = ptr.add(1);
            i += 1;
            continue;
        }
        std::hint::cold_path();
        ptr = emit_escaped_byte_ptr(ptr, c);
        i += 1;
    }
    ptr
}

#[inline(always)]
fn hex_val(b: u8) -> Option<u16> {
    match b {
        b'0'..=b'9' => Some((b - b'0') as u16),
        b'a'..=b'f' => Some((b - b'a' + 10) as u16),
        b'A'..=b'F' => Some((b - b'A' + 10) as u16),
        _ => None,
    }
}

#[inline(always)]
fn utf8_seq_len(s: &[u8]) -> usize {
    let b0 = s[0];
    if b0 < 0x80 {
        return 1;
    }
    if (0xC2..=0xDF).contains(&b0) {
        return if s.len() >= 2 && (s[1] & 0xC0) == 0x80 { 2 } else { 0 };
    }
    if (0xE0..=0xEF).contains(&b0) {
        if s.len() < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 {
            return 0;
        }
        if (b0 == 0xE0 && s[1] < 0xA0) || (b0 == 0xED && s[1] > 0x9F) {
            return 0;
        }
        return 3;
    }
    if (0xF0..=0xF4).contains(&b0) {
        if s.len() < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80 {
            return 0;
        }
        if (b0 == 0xF0 && s[1] < 0x90) || (b0 == 0xF4 && s[1] > 0x8F) {
            return 0;
        }
        return 4;
    }
    0
}

#[derive(Clone, Copy)]
enum StrSpan {
    Raw(usize, usize),
    Scratch,
    Omitted,
}

#[inline(always)]
fn scan_plain_string_prefix(s: &[u8], start: usize) -> (usize, bool) {
    let n = s.len();
    let mut i = start;
    let mut non_ascii_acc: u32 = 0;

    #[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
    unsafe {
        use std::arch::x86_64::*;
        let v_quote = _mm256_set1_epi8(b'"' as i8);
        let v_bslash = _mm256_set1_epi8(b'\\' as i8);
        let v_ctrl_bias = _mm256_set1_epi8(-128i8);
        let v_ctrl_lim = _mm256_set1_epi8((0x20i16 - 128) as i8);

        while i + 32 <= n {
            let chunk = _mm256_loadu_si256(s.as_ptr().add(i) as *const __m256i);
            let eq_q = _mm256_cmpeq_epi8(chunk, v_quote);
            let eq_b = _mm256_cmpeq_epi8(chunk, v_bslash);
            let biased = _mm256_add_epi8(chunk, v_ctrl_bias);
            let lt_ctrl = _mm256_cmpgt_epi8(v_ctrl_lim, biased);
            let hits = _mm256_or_si256(_mm256_or_si256(eq_q, eq_b), lt_ctrl);
            let stop_mask = _mm256_movemask_epi8(hits) as u32;
            let hi_mask = _mm256_movemask_epi8(chunk) as u32;
            if stop_mask == 0 {
                non_ascii_acc |= hi_mask;
                i += 32;
                continue;
            }
            let first = stop_mask.trailing_zeros() as usize;
            let prefix_mask = if first == 0 { 0 } else { (1u32 << first) - 1 };
            non_ascii_acc |= hi_mask & prefix_mask;
            return (i + first, non_ascii_acc != 0);
        }
    }

    #[cfg(target_arch = "x86_64")]
    unsafe {
        use std::arch::x86_64::*;
        let v_quote = _mm_set1_epi8(b'"' as i8);
        let v_bslash = _mm_set1_epi8(b'\\' as i8);
        let v_ctrl_bias = _mm_set1_epi8(-128i8);
        let v_ctrl_lim = _mm_set1_epi8((0x20i16 - 128) as i8);

        while i + 16 <= n {
            let chunk = _mm_loadu_si128(s.as_ptr().add(i) as *const __m128i);
            let eq_q = _mm_cmpeq_epi8(chunk, v_quote);
            let eq_b = _mm_cmpeq_epi8(chunk, v_bslash);
            let biased = _mm_add_epi8(chunk, v_ctrl_bias);
            let lt_ctrl = _mm_cmpgt_epi8(v_ctrl_lim, biased);
            let hits = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_ctrl);
            let stop_mask = _mm_movemask_epi8(hits) as u32;
            let hi_mask = _mm_movemask_epi8(chunk) as u32;
            if stop_mask == 0 {
                non_ascii_acc |= hi_mask;
                i += 16;
                continue;
            }
            let first = stop_mask.trailing_zeros() as usize;
            let prefix_mask = if first == 0 { 0 } else { (1u32 << first) - 1 };
            non_ascii_acc |= hi_mask & prefix_mask;
            return (i + first, non_ascii_acc != 0);
        }
    }

    while i < n {
        let b = s[i];
        if b < 0x20 || b == b'"' || b == b'\\' {
            break;
        }
        non_ascii_acc |= (b & 0x80) as u32;
        i += 1;
    }
    (i, non_ascii_acc != 0)
}

struct JsonScanner<'a> {
    s: &'a [u8],
    pos: usize,
}

impl<'a> JsonScanner<'a> {
    #[inline(always)]
    fn skip_ws(&mut self) {
        while self.pos < self.s.len() {
            match self.s[self.pos] {
                b' ' | b'\t' | b'\n' | b'\r' => self.pos += 1,
                _ => break,
            }
        }
    }

    #[inline(always)]
    fn parse_string_span(&mut self, mut out: Option<&mut Vec<u8>>) -> Option<(StrSpan, bool)> {
        if self.pos >= self.s.len() || self.s[self.pos] != b'"' {
            return None;
        }
        let start = self.pos + 1;
        let (stop, has_non_ascii) = scan_plain_string_prefix(self.s, start);
        if stop >= self.s.len() {
            return None;
        }
        let stop_byte = self.s[stop];
        if stop_byte == b'"' {
            if has_non_ascii && std::str::from_utf8(&self.s[start..stop]).is_err() {
                return None;
            }
            self.pos = stop + 1;
            let span = if out.is_some() {
                StrSpan::Raw(start, stop)
            } else {
                StrSpan::Omitted
            };
            return Some((span, !has_non_ascii));
        }
        if stop_byte < 0x20 {
            return None;
        }

        std::hint::cold_path();
        if has_non_ascii && std::str::from_utf8(&self.s[start..stop]).is_err() {
            return None;
        }
        if let Some(buf) = out.as_deref_mut() {
            buf.clear();
            buf.extend_from_slice(&self.s[start..stop]);
        }
        self.pos = stop;

        while self.pos < self.s.len() {
            let b = self.s[self.pos];
            if b == b'"' {
                self.pos += 1;
                let span = if out.is_some() {
                    StrSpan::Scratch
                } else {
                    StrSpan::Omitted
                };
                return Some((span, false));
            }
            if b < 0x20 {
                return None;
            }
            if b == b'\\' {
                self.pos += 1;
                if self.pos >= self.s.len() {
                    return None;
                }
                let esc = self.s[self.pos];
                self.pos += 1;
                let ch_byte = match esc {
                    b'"' => Some(b'"'),
                    b'\\' => Some(b'\\'),
                    b'/' => Some(b'/'),
                    b'b' => Some(0x08),
                    b'f' => Some(0x0c),
                    b'n' => Some(b'\n'),
                    b'r' => Some(b'\r'),
                    b't' => Some(b'\t'),
                    b'u' => None,
                    _ => return None,
                };
                if let Some(cb) = ch_byte {
                    if let Some(buf) = out.as_deref_mut() {
                        buf.push(cb);
                    }
                } else {
                    if self.pos + 4 > self.s.len() {
                        return None;
                    }
                    let u1 = match (
                        hex_val(self.s[self.pos]),
                        hex_val(self.s[self.pos + 1]),
                        hex_val(self.s[self.pos + 2]),
                        hex_val(self.s[self.pos + 3]),
                    ) {
                        (Some(a), Some(b), Some(c), Some(d)) => {
                            (a << 12) | (b << 8) | (c << 4) | d
                        }
                        _ => return None,
                    };
                    self.pos += 4;
                    let codepoint: u32 = if (0xD800..=0xDBFF).contains(&u1) {
                        if self.pos + 6 > self.s.len()
                            || self.s[self.pos] != b'\\'
                            || self.s[self.pos + 1] != b'u'
                        {
                            return None;
                        }
                        let u2 = match (
                            hex_val(self.s[self.pos + 2]),
                            hex_val(self.s[self.pos + 3]),
                            hex_val(self.s[self.pos + 4]),
                            hex_val(self.s[self.pos + 5]),
                        ) {
                            (Some(a), Some(b), Some(c), Some(d)) => {
                                (a << 12) | (b << 8) | (c << 4) | d
                            }
                            _ => return None,
                        };
                        if !(0xDC00..=0xDFFF).contains(&u2) {
                            return None;
                        }
                        self.pos += 6;
                        0x10000 + (((u1 as u32 - 0xD800) << 10) | (u2 as u32 - 0xDC00))
                    } else if (0xDC00..=0xDFFF).contains(&u1) {
                        return None;
                    } else {
                        u1 as u32
                    };
                    if let Some(ch) = char::from_u32(codepoint) {
                        if let Some(buf) = out.as_deref_mut() {
                            let mut tmp = [0u8; 4];
                            buf.extend_from_slice(ch.encode_utf8(&mut tmp).as_bytes());
                        }
                    } else {
                        return None;
                    }
                }
                continue;
            }
            let seq = utf8_seq_len(&self.s[self.pos..]);
            if seq == 0 {
                return None;
            }
            if let Some(buf) = out.as_deref_mut() {
                buf.extend_from_slice(&self.s[self.pos..self.pos + seq]);
            }
            self.pos += seq;
        }
        None
    }

    #[inline(always)]
    fn parse_number_seconds(&mut self) -> Result<Option<i64>, ()> {
        if self.pos >= self.s.len() {
            return Err(());
        }
        let b0 = self.s[self.pos];
        if b0 != b'-' && !b0.is_ascii_digit() {
            return if self.skip_value(1) {
                Ok(None)
            } else {
                Err(())
            };
        }
        let neg = if b0 == b'-' {
            self.pos += 1;
            true
        } else {
            false
        };
        if self.pos >= self.s.len() {
            return Err(());
        }
        let mut val: i64 = 0;
        let first_d = self.s[self.pos];
        if first_d == b'0' {
            self.pos += 1;
        } else if first_d.is_ascii_digit() {
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                val = val
                    .saturating_mul(10)
                    .saturating_add((self.s[self.pos] - b'0') as i64);
                self.pos += 1;
            }
        } else {
            return Err(());
        }

        let mut frac_nonzero = false;
        let mut has_exp = false;
        if self.pos < self.s.len() && self.s[self.pos] == b'.' {
            self.pos += 1;
            if self.pos >= self.s.len() || !self.s[self.pos].is_ascii_digit() {
                return Err(());
            }
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                if self.s[self.pos] != b'0' {
                    frac_nonzero = true;
                }
                self.pos += 1;
            }
        }
        if self.pos < self.s.len() && (self.s[self.pos] == b'e' || self.s[self.pos] == b'E') {
            has_exp = true;
            self.pos += 1;
            if self.pos < self.s.len() && (self.s[self.pos] == b'+' || self.s[self.pos] == b'-') {
                self.pos += 1;
            }
            if self.pos >= self.s.len() || !self.s[self.pos].is_ascii_digit() {
                return Err(());
            }
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        }
        if frac_nonzero || has_exp {
            Ok(None)
        } else {
            Ok(Some(if neg { -val } else { val }))
        }
    }

    fn skip_number(&mut self) -> bool {
        if self.pos < self.s.len() && self.s[self.pos] == b'-' {
            self.pos += 1;
        }
        if self.pos >= self.s.len() {
            return false;
        }
        if self.s[self.pos] == b'0' {
            self.pos += 1;
        } else if self.s[self.pos].is_ascii_digit() {
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        } else {
            return false;
        }
        if self.pos < self.s.len() && self.s[self.pos] == b'.' {
            self.pos += 1;
            if self.pos >= self.s.len() || !self.s[self.pos].is_ascii_digit() {
                return false;
            }
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        }
        if self.pos < self.s.len() && (self.s[self.pos] == b'e' || self.s[self.pos] == b'E') {
            self.pos += 1;
            if self.pos < self.s.len() && (self.s[self.pos] == b'+' || self.s[self.pos] == b'-') {
                self.pos += 1;
            }
            if self.pos >= self.s.len() || !self.s[self.pos].is_ascii_digit() {
                return false;
            }
            while self.pos < self.s.len() && self.s[self.pos].is_ascii_digit() {
                self.pos += 1;
            }
        }
        true
    }

    fn skip_value(&mut self, depth: u32) -> bool {
        if depth > 64 {
            return false;
        }
        self.skip_ws();
        if self.pos >= self.s.len() {
            return false;
        }
        match self.s[self.pos] {
            b'"' => self.parse_string_span(None).is_some(),
            b'{' => {
                self.pos += 1;
                self.skip_ws();
                if self.pos < self.s.len() && self.s[self.pos] == b'}' {
                    self.pos += 1;
                    return true;
                }
                loop {
                    self.skip_ws();
                    if self.parse_string_span(None).is_none() {
                        return false;
                    }
                    self.skip_ws();
                    if self.pos >= self.s.len() || self.s[self.pos] != b':' {
                        return false;
                    }
                    self.pos += 1;
                    if !self.skip_value(depth + 1) {
                        return false;
                    }
                    self.skip_ws();
                    if self.pos >= self.s.len() {
                        return false;
                    }
                    if self.s[self.pos] == b',' {
                        self.pos += 1;
                    } else if self.s[self.pos] == b'}' {
                        self.pos += 1;
                        return true;
                    } else {
                        return false;
                    }
                }
            }
            b'[' => {
                self.pos += 1;
                self.skip_ws();
                if self.pos < self.s.len() && self.s[self.pos] == b']' {
                    self.pos += 1;
                    return true;
                }
                loop {
                    if !self.skip_value(depth + 1) {
                        return false;
                    }
                    self.skip_ws();
                    if self.pos >= self.s.len() {
                        return false;
                    }
                    if self.s[self.pos] == b',' {
                        self.pos += 1;
                    } else if self.s[self.pos] == b']' {
                        self.pos += 1;
                        return true;
                    } else {
                        return false;
                    }
                }
            }
            b't' => {
                if self.s[self.pos..].starts_with(b"true") {
                    self.pos += 4;
                    true
                } else {
                    false
                }
            }
            b'f' => {
                if self.s[self.pos..].starts_with(b"false") {
                    self.pos += 5;
                    true
                } else {
                    false
                }
            }
            b'n' => {
                if self.s[self.pos..].starts_with(b"null") {
                    self.pos += 4;
                    true
                } else {
                    false
                }
            }
            b'-' | b'0'..=b'9' => self.skip_number(),
            _ => false,
        }
    }
}

pub enum PostBodyParse<'a> {
    Ok(&'a [u8]),
    MalformedJson,
    BodyRequired,
    BodyTooLong,
}

#[inline(always)]
fn js_ws_prefix_len(s: &[u8]) -> usize {
    if s.is_empty() {
        return 0;
    }
    let b0 = s[0];
    if b0 == b' ' || (0x09..=0x0D).contains(&b0) {
        return 1;
    }
    if s.len() >= 2 && b0 == 0xC2 && s[1] == 0xA0 {
        return 2;
    }
    if s.len() >= 3 {
        let tri = ((b0 as u32) << 16) | ((s[1] as u32) << 8) | (s[2] as u32);
        if tri == 0xE19A80
            || (0xE28080..=0xE2808A).contains(&tri)
            || tri == 0xE280A8
            || tri == 0xE280A9
            || tri == 0xE280AF
            || tri == 0xE2819F
            || tri == 0xE38080
            || tri == 0xEFBBBF
        {
            return 3;
        }
    }
    0
}

#[inline(always)]
fn js_ws_suffix_len(s: &[u8]) -> usize {
    let n = s.len();
    if n == 0 {
        return 0;
    }
    let last = s[n - 1];
    if last == b' ' || (0x09..=0x0D).contains(&last) {
        return 1;
    }
    if n >= 2 && s[n - 2] == 0xC2 && last == 0xA0 {
        return 2;
    }
    if n >= 3 && js_ws_prefix_len(&s[n - 3..]) == 3 {
        return 3;
    }
    0
}

pub fn parse_post_body<'a>(
    raw_json: &'a [u8],
    scratch: &'a mut Vec<u8>,
    key_buf: &mut Vec<u8>,
) -> PostBodyParse<'a> {
    let mut sc = JsonScanner { s: raw_json, pos: 0 };
    sc.skip_ws();
    if sc.pos >= sc.s.len() || sc.s[sc.pos] != b'{' {
        return PostBodyParse::MalformedJson;
    }
    sc.pos += 1;
    sc.skip_ws();

    if sc.pos < sc.s.len() && sc.s[sc.pos] == b'}' {
        sc.pos += 1;
        sc.skip_ws();
        return if sc.pos == sc.s.len() {
            PostBodyParse::BodyRequired
        } else {
            PostBodyParse::MalformedJson
        };
    }

    let mut body_span: Option<(StrSpan, bool)> = None;

    loop {
        sc.skip_ws();
        let Some((kspan, _)) = sc.parse_string_span(Some(key_buf)) else {
            return PostBodyParse::MalformedJson;
        };
        let is_body_key = match kspan {
            StrSpan::Raw(a, b) => &raw_json[a..b] == b"body",
            StrSpan::Scratch => key_buf.as_slice() == b"body",
            StrSpan::Omitted => false,
        };
        sc.skip_ws();
        if sc.pos >= sc.s.len() || sc.s[sc.pos] != b':' {
            return PostBodyParse::MalformedJson;
        }
        sc.pos += 1;
        sc.skip_ws();

        if is_body_key {
            if sc.pos < sc.s.len() && sc.s[sc.pos] == b'"' {
                let Some((vspan, pure_ascii)) = sc.parse_string_span(Some(scratch)) else {
                    return PostBodyParse::MalformedJson;
                };
                body_span = Some((vspan, pure_ascii));
            } else {
                if !sc.skip_value(1) {
                    return PostBodyParse::MalformedJson;
                }
                body_span = None;
            }
        } else if !sc.skip_value(1) {
            return PostBodyParse::MalformedJson;
        }

        sc.skip_ws();
        if sc.pos >= sc.s.len() {
            return PostBodyParse::MalformedJson;
        }
        if sc.s[sc.pos] == b',' {
            sc.pos += 1;
        } else if sc.s[sc.pos] == b'}' {
            sc.pos += 1;
            break;
        } else {
            return PostBodyParse::MalformedJson;
        }
    }

    sc.skip_ws();
    if sc.pos != sc.s.len() {
        return PostBodyParse::MalformedJson;
    }
    let Some((vspan, pure_ascii)) = body_span else {
        return PostBodyParse::BodyRequired;
    };

    let mut slice: &'a [u8] = match vspan {
        StrSpan::Raw(a, b) => &raw_json[a..b],
        StrSpan::Scratch => scratch.as_slice(),
        StrSpan::Omitted => return PostBodyParse::BodyRequired,
    };

    if pure_ascii {
        while let Some(&b) = slice.first() {
            if b == b' ' || (0x09..=0x0D).contains(&b) {
                slice = &slice[1..];
            } else {
                break;
            }
        }
        while let Some(&b) = slice.last() {
            if b == b' ' || (0x09..=0x0D).contains(&b) {
                slice = &slice[..slice.len() - 1];
            } else {
                break;
            }
        }
        if slice.is_empty() {
            return PostBodyParse::BodyRequired;
        }
        if slice.len() > 500 {
            return PostBodyParse::BodyTooLong;
        }
        return PostBodyParse::Ok(slice);
    }

    loop {
        let p = js_ws_prefix_len(slice);
        if p == 0 {
            break;
        }
        slice = &slice[p..];
    }
    loop {
        let sf = js_ws_suffix_len(slice);
        if sf == 0 {
            break;
        }
        slice = &slice[..slice.len() - sf];
    }
    if slice.is_empty() {
        return PostBodyParse::BodyRequired;
    }
    let char_count = slice.iter().filter(|&&b| (b & 0xC0) != 0x80).count();
    if char_count > 500 {
        return PostBodyParse::BodyTooLong;
    }
    PostBodyParse::Ok(slice)
}

pub fn jwt_header_is_hs256(raw: &[u8], key: &mut Vec<u8>, val: &mut Vec<u8>) -> bool {
    let mut sc = JsonScanner { s: raw, pos: 0 };
    sc.skip_ws();
    if sc.pos >= sc.s.len() || sc.s[sc.pos] != b'{' {
        return false;
    }
    sc.pos += 1;
    sc.skip_ws();
    if sc.pos < sc.s.len() && sc.s[sc.pos] == b'}' {
        return false;
    }
    let mut is_hs256 = false;
    loop {
        sc.skip_ws();
        let Some((kspan, _)) = sc.parse_string_span(Some(key)) else {
            return false;
        };
        let is_alg = match kspan {
            StrSpan::Raw(a, b) => &raw[a..b] == b"alg",
            StrSpan::Scratch => key.as_slice() == b"alg",
            StrSpan::Omitted => false,
        };
        sc.skip_ws();
        if sc.pos >= sc.s.len() || sc.s[sc.pos] != b':' {
            return false;
        }
        sc.pos += 1;
        sc.skip_ws();
        if is_alg {
            let Some((vspan, _)) = sc.parse_string_span(Some(val)) else {
                return false;
            };
            is_hs256 = match vspan {
                StrSpan::Raw(a, b) => &raw[a..b] == b"HS256",
                StrSpan::Scratch => val.as_slice() == b"HS256",
                StrSpan::Omitted => false,
            };
        } else if !sc.skip_value(1) {
            return false;
        }
        sc.skip_ws();
        if sc.pos >= sc.s.len() {
            return false;
        }
        if sc.s[sc.pos] == b',' {
            sc.pos += 1;
        } else if sc.s[sc.pos] == b'}' {
            sc.pos += 1;
            break;
        } else {
            return false;
        }
    }
    sc.skip_ws();
    sc.pos == sc.s.len() && is_hs256
}

#[derive(Clone, Copy)]
pub enum JwtUnameSpan {
    Raw(usize, usize),
    Scratch,
    Omitted,
}

pub enum JwtClaimsResult {
    Ok { user_id: i64, uname: JwtUnameSpan },
    InvalidOrExpired,
    InvalidPayload,
}

#[inline(always)]
pub fn parse_positive_id(bytes: &[u8]) -> Option<i64> {
    if bytes.is_empty() {
        return None;
    }
    let mut v: i64 = 0;
    for &b in bytes {
        if !b.is_ascii_digit() {
            return None;
        }
        if v < 100_000_000_000_000_000 {
            v = v * 10 + (b - b'0') as i64;
        } else {
            v = i64::MAX;
        }
    }
    if v > 0 { Some(v) } else { None }
}

pub fn parse_jwt_claims(
    raw: &[u8],
    now_sec: i64,
    need_username: bool,
    username_out: &mut Vec<u8>,
    key: &mut Vec<u8>,
    sub_buf: &mut Vec<u8>,
) -> JwtClaimsResult {
    let mut sc = JsonScanner { s: raw, pos: 0 };
    sc.skip_ws();
    if sc.pos >= sc.s.len() || sc.s[sc.pos] != b'{' {
        return JwtClaimsResult::InvalidOrExpired;
    }
    sc.pos += 1;
    sc.skip_ws();
    if sc.pos < sc.s.len() && sc.s[sc.pos] == b'}' {
        return JwtClaimsResult::InvalidOrExpired;
    }

    let mut exp_val: Option<i64> = None;
    let mut exp_bad = false;
    let mut nbf_val: Option<i64> = None;
    let mut nbf_bad = false;
    let mut sub_uid: Option<i64> = None;
    let mut uname_span: Option<JwtUnameSpan> = None;

    loop {
        sc.skip_ws();
        let Some((kspan, _)) = sc.parse_string_span(Some(key)) else {
            return JwtClaimsResult::InvalidOrExpired;
        };
        let key_bytes: &[u8] = match kspan {
            StrSpan::Raw(a, b) => &raw[a..b],
            StrSpan::Scratch => key.as_slice(),
            StrSpan::Omitted => &[],
        };
        sc.skip_ws();
        if sc.pos >= sc.s.len() || sc.s[sc.pos] != b':' {
            return JwtClaimsResult::InvalidOrExpired;
        }
        sc.pos += 1;
        sc.skip_ws();

        match key_bytes {
            b"exp" => match sc.parse_number_seconds() {
                Ok(Some(v)) => exp_val = Some(v),
                Ok(None) => exp_bad = true,
                Err(()) => return JwtClaimsResult::InvalidOrExpired,
            },
            b"nbf" => match sc.parse_number_seconds() {
                Ok(Some(v)) => nbf_val = Some(v),
                Ok(None) => nbf_bad = true,
                Err(()) => return JwtClaimsResult::InvalidOrExpired,
            },
            b"sub" => {
                if sc.pos < sc.s.len() && sc.s[sc.pos] == b'"' {
                    let Some((vspan, _)) = sc.parse_string_span(Some(sub_buf)) else {
                        return JwtClaimsResult::InvalidOrExpired;
                    };
                    let sub_slice = match vspan {
                        StrSpan::Raw(a, b) => &raw[a..b],
                        StrSpan::Scratch => sub_buf.as_slice(),
                        StrSpan::Omitted => &[],
                    };
                    sub_uid = parse_positive_id(sub_slice);
                } else {
                    if !sc.skip_value(1) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    sub_uid = None;
                }
            }
            b"username" => {
                if sc.pos < sc.s.len() && sc.s[sc.pos] == b'"' {
                    let out_opt = if need_username {
                        Some(&mut *username_out)
                    } else {
                        None
                    };
                    let Some((vspan, _)) = sc.parse_string_span(out_opt) else {
                        return JwtClaimsResult::InvalidOrExpired;
                    };
                    uname_span = Some(match vspan {
                        StrSpan::Raw(a, b) => JwtUnameSpan::Raw(a, b),
                        StrSpan::Scratch => JwtUnameSpan::Scratch,
                        StrSpan::Omitted => JwtUnameSpan::Omitted,
                    });
                } else {
                    if !sc.skip_value(1) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    uname_span = None;
                }
            }
            _ => {
                if !sc.skip_value(1) {
                    return JwtClaimsResult::InvalidOrExpired;
                }
            }
        }

        sc.skip_ws();
        if sc.pos >= sc.s.len() {
            return JwtClaimsResult::InvalidOrExpired;
        }
        if sc.s[sc.pos] == b',' {
            sc.pos += 1;
        } else if sc.s[sc.pos] == b'}' {
            sc.pos += 1;
            break;
        } else {
            return JwtClaimsResult::InvalidOrExpired;
        }
    }

    sc.skip_ws();
    if sc.pos != sc.s.len() {
        return JwtClaimsResult::InvalidOrExpired;
    }

    if exp_bad || nbf_bad {
        return JwtClaimsResult::InvalidOrExpired;
    }
    let Some(exp) = exp_val else {
        return JwtClaimsResult::InvalidOrExpired;
    };
    if exp <= now_sec {
        return JwtClaimsResult::InvalidOrExpired;
    }
    if let Some(nbf) = nbf_val {
        if nbf > now_sec {
            return JwtClaimsResult::InvalidOrExpired;
        }
    }
    match (sub_uid, uname_span) {
        (Some(uid), Some(uname)) => JwtClaimsResult::Ok {
            user_id: uid,
            uname,
        },
        _ => JwtClaimsResult::InvalidPayload,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ref_escape(s: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        for &c in s {
            match c {
                b'"' => out.extend_from_slice(b"\\\""),
                b'\\' => out.extend_from_slice(b"\\\\"),
                b'\n' => out.extend_from_slice(b"\\n"),
                b'\r' => out.extend_from_slice(b"\\r"),
                b'\t' => out.extend_from_slice(b"\\t"),
                b'\x08' => out.extend_from_slice(b"\\b"),
                b'\x0c' => out.extend_from_slice(b"\\f"),
                0x00..=0x1f => {
                    out.extend_from_slice(&[
                        b'\\',
                        b'u',
                        b'0',
                        b'0',
                        HEX[(c >> 4) as usize],
                        HEX[(c & 15) as usize],
                    ]);
                }
                _ => out.push(c),
            }
        }
        out
    }

    #[test]
    fn test_write_i64_and_json_escape_exhaustive_boundaries() {
        let mut out = Vec::new();
        write_i64(&mut out, 0);
        out.push(b',');
        write_i64(&mut out, -9876543210);
        out.push(b',');
        write_i64(&mut out, 1234567890123);
        assert_eq!(
            std::str::from_utf8(&out).unwrap(),
            "0,-9876543210,1234567890123"
        );

        let specials = [b'"', b'\\', b'\n', b'\r', b'\t', 0x08, 0x0c, 0x00, 0x1f];
        for len in 0..=140 {
            let base: Vec<u8> = (0..len).map(|k| b'a' + (k % 26) as u8).collect();
            let mut got = Vec::new();
            write_json_escaped(&mut got, &base);
            assert_eq!(got, ref_escape(&base));

            for pos in 0..len {
                let mut mutated = base.clone();
                mutated[pos] = specials[(len + pos) % specials.len()];
                got.clear();
                write_json_escaped(&mut got, &mutated);
                assert_eq!(got, ref_escape(&mutated), "mismatch len={len} pos={pos}");
            }
        }
    }

    #[test]
    fn test_zero_copy_and_escaped_post_body_and_jwt() {
        let mut scratch = Vec::new();
        let mut key_buf = Vec::new();
        let raw = br#"  {"body": "  hello zero copy world!  "} "#;
        match parse_post_body(raw, &mut scratch, &mut key_buf) {
            PostBodyParse::Ok(s) => {
                assert_eq!(s, b"hello zero copy world!");
                let r_range = raw.as_ptr_range();
                assert!(r_range.contains(&s.as_ptr()));
            }
            _ => panic!("expected Ok"),
        }

        let esc_raw = "{\"body\":\"  test: <b>&amp;</b> \\\"quoted\\\" / caf\u{00e9} \u{2713}  \"}".as_bytes();
        match parse_post_body(esc_raw, &mut scratch, &mut key_buf) {
            PostBodyParse::Ok(s) => {
                assert_eq!(
                    std::str::from_utf8(s).unwrap(),
                    "test: <b>&amp;</b> \"quoted\" / caf\u{00e9} \u{2713}"
                );
            }
            _ => panic!("expected Ok on escaped utf8"),
        }

        let mut uname = Vec::new();
        let mut sub_b = Vec::new();
        let claims = br#"{"sub":"42","username":"golden_ember_1","iat":1700000000,"exp":1900000000}"#;
        match parse_jwt_claims(claims, 1800000000, true, &mut uname, &mut key_buf, &mut sub_b) {
            JwtClaimsResult::Ok {
                user_id: 42,
                uname: JwtUnameSpan::Raw(a, b),
            } => assert_eq!(&claims[a..b], b"golden_ember_1"),
            _ => panic!("expected zero-copy JWT claims"),
        }
        assert!(uname.is_empty());

        match parse_jwt_claims(claims, 1800000000, false, &mut uname, &mut key_buf, &mut sub_b) {
            JwtClaimsResult::Ok {
                user_id: 42,
                uname: JwtUnameSpan::Omitted,
            } => {}
            _ => panic!("expected omitted username on like path"),
        }

        let mut ts_cache = DayPrefixCache::new();
        let cases = [
            (0i64, "2000-01-01T00:00:00.000Z"),
            (-1_000i64, "1999-12-31T23:59:59.999Z"),
            (820_540_800_123_000i64, "2026-01-01T00:00:00.123Z"),
            (762_566_399_987_000i64, "2024-02-29T23:59:59.987Z"),
        ];
        for (pg_us, expected) in cases {
            let mut buf = [0u8; 32];
            let end = unsafe { write_pg_timestamptz_ptr(buf.as_mut_ptr().add(3), pg_us, &mut ts_cache) };
            assert_eq!(end as usize - buf.as_ptr() as usize, 27);
            assert_eq!(std::str::from_utf8(&buf[3..27]).unwrap(), expected);
        }
        for extreme in [i64::MIN, i64::MAX, -100_000_000_000_000_000i64] {
            let mut buf = [0u8; 32];
            let end = unsafe { write_pg_timestamptz_ptr(buf.as_mut_ptr(), extreme, &mut ts_cache) };
            assert_eq!(end as usize - buf.as_ptr() as usize, 24);
            assert!(std::str::from_utf8(&buf[..24]).is_ok());
        }
    }
}
