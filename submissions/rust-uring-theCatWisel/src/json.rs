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
    prefix: [u8; 11],
}

impl DayPrefixCache {
    #[inline(always)]
    pub const fn new() -> Self {
        Self {
            day: i64::MIN,
            prefix: [0u8; 11],
        }
    }
}

#[inline(always)]
fn format_civil_day(days_since_epoch: i64, dst: &mut [u8; 11]) {
    let z = days_since_epoch + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u32;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146_096) / 365;
    let y_base = (yoe as i64) + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as usize;
    let m = (if mp < 10 { mp + 3 } else { mp - 9 }) as usize;
    let y = (y_base + if m <= 2 { 1 } else { 0 }) as usize;

    let yh = (y / 100) * 2;
    let yl = (y % 100) * 2;
    let mm = m * 2;
    let dd = d * 2;

    dst[0] = DIGITS_200[yh];
    dst[1] = DIGITS_200[yh + 1];
    dst[2] = DIGITS_200[yl];
    dst[3] = DIGITS_200[yl + 1];
    dst[4] = b'-';
    dst[5] = DIGITS_200[mm];
    dst[6] = DIGITS_200[mm + 1];
    dst[7] = b'-';
    dst[8] = DIGITS_200[dd];
    dst[9] = DIGITS_200[dd + 1];
    dst[10] = b'T';
}

#[inline(always)]
pub fn write_pg_timestamptz(out: &mut Vec<u8>, pg_us: i64, cache: &mut DayPrefixCache) {
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
        format_civil_day(days, &mut cache.prefix);
        cache.day = days;
    }

    let sod_u = sod as u32;
    let hh = ((sod_u / 3600) as usize) * 2;
    let rem_s = sod_u % 3600;
    let mi = ((rem_s / 60) as usize) * 2;
    let ss = ((rem_s % 60) as usize) * 2;
    let ms = (rem_us as u32) / 1000;
    let ms_h = (ms / 100) as u8 + b'0';
    let ms_l = ((ms % 100) as usize) * 2;

    let mut tail = [0u8; 24];
    tail[..11].copy_from_slice(&cache.prefix);
    tail[11] = DIGITS_200[hh];
    tail[12] = DIGITS_200[hh + 1];
    tail[13] = b':';
    tail[14] = DIGITS_200[mi];
    tail[15] = DIGITS_200[mi + 1];
    tail[16] = b':';
    tail[17] = DIGITS_200[ss];
    tail[18] = DIGITS_200[ss + 1];
    tail[19] = b'.';
    tail[20] = ms_h;
    tail[21] = DIGITS_200[ms_l];
    tail[22] = DIGITS_200[ms_l + 1];
    tail[23] = b'Z';
    out.extend_from_slice(&tail);
}

#[inline(always)]
pub fn write_json_escaped(out: &mut Vec<u8>, s: &[u8]) {
    let n = s.len();
    let mut i = 0usize;
    let mut run_start = 0usize;

    #[cfg(all(
        target_arch = "x86_64",
        target_feature = "avx512bw",
        target_feature = "vaes"
    ))]
    unsafe {
        use std::arch::x86_64::*;
        let v_quote = _mm512_set1_epi8(b'"' as i8);
        let v_bslash = _mm512_set1_epi8(b'\\' as i8);
        let v_space = _mm512_set1_epi8(0x20);

        while i + 64 <= n {
            let chunk = _mm512_loadu_si512(s.as_ptr().add(i) as *const _);
            let m_q = _mm512_cmpeq_epi8_mask(chunk, v_quote);
            let m_b = _mm512_cmpeq_epi8_mask(chunk, v_bslash);
            let m_c = _mm512_cmplt_epu8_mask(chunk, v_space);
            let mask = m_q | m_b | m_c;
            if mask == 0 {
                i += 64;
                continue;
            }
            let first = mask.trailing_zeros() as usize;
            let pos = i + first;
            out.extend_from_slice(&s[run_start..pos]);
            emit_escaped_byte(out, s[pos]);
            i = pos + 1;
            run_start = i;
        }
    }

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
            let mask = _mm256_movemask_epi8(hits) as u32;
            if mask == 0 {
                i += 32;
                continue;
            }
            let first = mask.trailing_zeros() as usize;
            let pos = i + first;
            out.extend_from_slice(&s[run_start..pos]);
            emit_escaped_byte(out, s[pos]);
            i = pos + 1;
            run_start = i;
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
            let mask = _mm_movemask_epi8(hits) as u32;
            if mask == 0 {
                i += 16;
                continue;
            }
            let first = mask.trailing_zeros() as usize;
            let pos = i + first;
            out.extend_from_slice(&s[run_start..pos]);
            emit_escaped_byte(out, s[pos]);
            i = pos + 1;
            run_start = i;
        }
    }

    while i < n {
        let c = s[i];
        if c >= 0x20 && c != b'"' && c != b'\\' {
            i += 1;
            continue;
        }
        if run_start < i {
            out.extend_from_slice(&s[run_start..i]);
        }
        emit_escaped_byte(out, c);
        i += 1;
        run_start = i;
    }
    if run_start < n {
        out.extend_from_slice(&s[run_start..n]);
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
        format_civil_day(days, &mut cache.prefix);
        cache.day = days;
    }

    let sod_u = sod as u32;
    let hh = ((sod_u / 3600) as usize) * 2;
    let rem_s = sod_u % 3600;
    let mi = ((rem_s / 60) as usize) * 2;
    let ss = ((rem_s % 60) as usize) * 2;
    let ms = (rem_us as u32) / 1000;
    let ms_h = (ms / 100) as u8 + b'0';
    let ms_l = ((ms % 100) as usize) * 2;

    std::ptr::copy_nonoverlapping(cache.prefix.as_ptr(), ptr, 11);
    *ptr.add(11) = DIGITS_200[hh];
    *ptr.add(12) = DIGITS_200[hh + 1];
    *ptr.add(13) = b':';
    *ptr.add(14) = DIGITS_200[mi];
    *ptr.add(15) = DIGITS_200[mi + 1];
    *ptr.add(16) = b':';
    *ptr.add(17) = DIGITS_200[ss];
    *ptr.add(18) = DIGITS_200[ss + 1];
    *ptr.add(19) = b'.';
    *ptr.add(20) = ms_h;
    *ptr.add(21) = DIGITS_200[ms_l];
    *ptr.add(22) = DIGITS_200[ms_l + 1];
    *ptr.add(23) = b'Z';
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
    let mut run_start = 0usize;

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
            let m_q = _mm512_cmpeq_epi8_mask(chunk, v_quote);
            let m_b = _mm512_cmpeq_epi8_mask(chunk, v_bslash);
            let m_c = _mm512_cmplt_epu8_mask(chunk, v_space);
            let mask = m_q | m_b | m_c;
            if mask == 0 {
                i += 64;
                continue;
            }
            std::hint::cold_path();
            let pos = i + mask.trailing_zeros() as usize;
            let span = pos - run_start;
            if span > 0 {
                std::ptr::copy_nonoverlapping(s.as_ptr().add(run_start), ptr, span);
                ptr = ptr.add(span);
            }
            ptr = emit_escaped_byte_ptr(ptr, s[pos]);
            i = pos + 1;
            run_start = i;
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
            let eq_q = _mm256_cmpeq_epi8(chunk, v_quote);
            let eq_b = _mm256_cmpeq_epi8(chunk, v_bslash);
            let biased = _mm256_add_epi8(chunk, v_ctrl_bias);
            let lt_ctrl = _mm256_cmpgt_epi8(v_ctrl_lim, biased);
            let hits = _mm256_or_si256(_mm256_or_si256(eq_q, eq_b), lt_ctrl);
            let mask = _mm256_movemask_epi8(hits) as u32;
            if mask == 0 {
                i += 32;
                continue;
            }
            let pos = i + mask.trailing_zeros() as usize;
            let span = pos - run_start;
            if span > 0 {
                std::ptr::copy_nonoverlapping(s.as_ptr().add(run_start), ptr, span);
                ptr = ptr.add(span);
            }
            ptr = emit_escaped_byte_ptr(ptr, s[pos]);
            i = pos + 1;
            run_start = i;
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
            let eq_q = _mm_cmpeq_epi8(chunk, v_quote);
            let eq_b = _mm_cmpeq_epi8(chunk, v_bslash);
            let biased = _mm_add_epi8(chunk, v_ctrl_bias);
            let lt_ctrl = _mm_cmpgt_epi8(v_ctrl_lim, biased);
            let hits = _mm_or_si128(_mm_or_si128(eq_q, eq_b), lt_ctrl);
            let mask = _mm_movemask_epi8(hits) as u32;
            if mask == 0 {
                i += 16;
                continue;
            }
            let pos = i + mask.trailing_zeros() as usize;
            let span = pos - run_start;
            if span > 0 {
                std::ptr::copy_nonoverlapping(s.as_ptr().add(run_start), ptr, span);
                ptr = ptr.add(span);
            }
            ptr = emit_escaped_byte_ptr(ptr, s[pos]);
            i = pos + 1;
            run_start = i;
        }
    }

    while i < n {
        let c = s[i];
        if c >= 0x20 && c != b'"' && c != b'\\' {
            i += 1;
            continue;
        }
        let span = i - run_start;
        if span > 0 {
            std::ptr::copy_nonoverlapping(s.as_ptr().add(run_start), ptr, span);
            ptr = ptr.add(span);
        }
        ptr = emit_escaped_byte_ptr(ptr, c);
        i += 1;
        run_start = i;
    }
    let tail = n - run_start;
    if tail > 0 {
        std::ptr::copy_nonoverlapping(s.as_ptr().add(run_start), ptr, tail);
        ptr = ptr.add(tail);
    }
    ptr
}

#[inline(always)]
fn emit_escaped_byte(out: &mut Vec<u8>, c: u8) {
    match c {
        b'"' => out.extend_from_slice(b"\\\""),
        b'\\' => out.extend_from_slice(b"\\\\"),
        b'\n' => out.extend_from_slice(b"\\n"),
        b'\r' => out.extend_from_slice(b"\\r"),
        b'\t' => out.extend_from_slice(b"\\t"),
        b'\x08' => out.extend_from_slice(b"\\b"),
        b'\x0c' => out.extend_from_slice(b"\\f"),
        _ => {
            out.extend_from_slice(&[
                b'\\',
                b'u',
                b'0',
                b'0',
                HEX[(c >> 4) as usize],
                HEX[(c & 15) as usize],
            ]);
        }
    }
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

    fn parse_string(&mut self, mut out: Option<&mut Vec<u8>>) -> bool {
        if self.pos >= self.s.len() || self.s[self.pos] != b'"' {
            return false;
        }
        self.pos += 1;
        while self.pos < self.s.len() {
            let b = self.s[self.pos];
            if b == b'"' {
                self.pos += 1;
                return true;
            }
            if b < 0x20 {
                return false;
            }
            if b == b'\\' {
                self.pos += 1;
                if self.pos >= self.s.len() {
                    return false;
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
                    _ => return false,
                };
                if let Some(cb) = ch_byte {
                    if let Some(buf) = out.as_deref_mut() {
                        buf.push(cb);
                    }
                } else {
                    if self.pos + 4 > self.s.len() {
                        return false;
                    }
                    let u1 = match (
                        hex_val(self.s[self.pos]),
                        hex_val(self.s[self.pos + 1]),
                        hex_val(self.s[self.pos + 2]),
                        hex_val(self.s[self.pos + 3]),
                    ) {
                        (Some(a), Some(b), Some(c), Some(d)) => (a << 12) | (b << 8) | (c << 4) | d,
                        _ => return false,
                    };
                    self.pos += 4;
                    let codepoint: u32 = if (0xD800..=0xDBFF).contains(&u1) {
                        if self.pos + 6 > self.s.len()
                            || self.s[self.pos] != b'\\'
                            || self.s[self.pos + 1] != b'u'
                        {
                            return false;
                        }
                        let u2 = match (
                            hex_val(self.s[self.pos + 2]),
                            hex_val(self.s[self.pos + 3]),
                            hex_val(self.s[self.pos + 4]),
                            hex_val(self.s[self.pos + 5]),
                        ) {
                            (Some(a), Some(b), Some(c), Some(d)) => (a << 12) | (b << 8) | (c << 4) | d,
                            _ => return false,
                        };
                        if !(0xDC00..=0xDFFF).contains(&u2) {
                            return false;
                        }
                        self.pos += 6;
                        0x10000 + (((u1 as u32 - 0xD800) << 10) | (u2 as u32 - 0xDC00))
                    } else if (0xDC00..=0xDFFF).contains(&u1) {
                        return false;
                    } else {
                        u1 as u32
                    };
                    if let Some(ch) = char::from_u32(codepoint) {
                        if let Some(buf) = out.as_deref_mut() {
                            let mut tmp = [0u8; 4];
                            buf.extend_from_slice(ch.encode_utf8(&mut tmp).as_bytes());
                        }
                    } else {
                        return false;
                    }
                }
                continue;
            }
            let seq = utf8_seq_len(&self.s[self.pos..]);
            if seq == 0 {
                return false;
            }
            if let Some(buf) = out.as_deref_mut() {
                buf.extend_from_slice(&self.s[self.pos..self.pos + seq]);
            }
            self.pos += seq;
        }
        false
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
            b'"' => self.parse_string(None),
            b'{' => {
                self.pos += 1;
                self.skip_ws();
                if self.pos < self.s.len() && self.s[self.pos] == b'}' {
                    self.pos += 1;
                    return true;
                }
                loop {
                    self.skip_ws();
                    if !self.parse_string(None) {
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
    raw_json: &[u8],
    scratch: &'a mut Vec<u8>,
    key_buf: &mut Vec<u8>,
) -> PostBodyParse<'a> {
    scratch.clear();
    let mut sc = JsonScanner { s: raw_json, pos: 0 };
    sc.skip_ws();
    if sc.pos >= sc.s.len() || sc.s[sc.pos] != b'{' {
        return PostBodyParse::MalformedJson;
    }
    sc.pos += 1;
    sc.skip_ws();

    let mut found_body_str = false;

    if sc.pos < sc.s.len() && sc.s[sc.pos] == b'}' {
        sc.pos += 1;
        sc.skip_ws();
        return if sc.pos == sc.s.len() {
            PostBodyParse::BodyRequired
        } else {
            PostBodyParse::MalformedJson
        };
    }

    loop {
        sc.skip_ws();
        key_buf.clear();
        if !sc.parse_string(Some(key_buf)) {
            return PostBodyParse::MalformedJson;
        }
        sc.skip_ws();
        if sc.pos >= sc.s.len() || sc.s[sc.pos] != b':' {
            return PostBodyParse::MalformedJson;
        }
        sc.pos += 1;
        sc.skip_ws();

        if key_buf.as_slice() == b"body" {
            if sc.pos < sc.s.len() && sc.s[sc.pos] == b'"' {
                scratch.clear();
                if !sc.parse_string(Some(scratch)) {
                    return PostBodyParse::MalformedJson;
                }
                found_body_str = true;
            } else {
                if !sc.skip_value(1) {
                    return PostBodyParse::MalformedJson;
                }
                found_body_str = false;
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
    if !found_body_str {
        return PostBodyParse::BodyRequired;
    }

    let mut slice: &[u8] = scratch.as_slice();
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
        key.clear();
        if !sc.parse_string(Some(key)) {
            return false;
        }
        sc.skip_ws();
        if sc.pos >= sc.s.len() || sc.s[sc.pos] != b':' {
            return false;
        }
        sc.pos += 1;
        sc.skip_ws();
        if key.as_slice() == b"alg" {
            val.clear();
            if !sc.parse_string(Some(val)) {
                return false;
            }
            is_hs256 = val.as_slice() == b"HS256";
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

pub enum JwtClaimsResult {
    Ok { user_id: i64 },
    InvalidOrExpired,
    InvalidPayload,
}

fn parse_json_int_seconds(num_bytes: &[u8]) -> Option<i64> {
    if num_bytes.is_empty() {
        return None;
    }
    let mut i = 0;
    let neg = if num_bytes[0] == b'-' {
        i = 1;
        true
    } else {
        false
    };
    if i >= num_bytes.len() || !num_bytes[i].is_ascii_digit() {
        return None;
    }
    let mut val: i64 = 0;
    while i < num_bytes.len() && num_bytes[i].is_ascii_digit() {
        val = val.saturating_mul(10).saturating_add((num_bytes[i] - b'0') as i64);
        i += 1;
    }
    if i < num_bytes.len() {
        if num_bytes[i] == b'.' {
            i += 1;
            if i >= num_bytes.len() {
                return None;
            }
            while i < num_bytes.len() && num_bytes[i] == b'0' {
                i += 1;
            }
            if i != num_bytes.len() {
                return None;
            }
        } else {
            return None;
        }
    }
    Some(if neg { -val } else { val })
}

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
    username_out: &mut Vec<u8>,
    key: &mut Vec<u8>,
    sub_buf: &mut Vec<u8>,
) -> JwtClaimsResult {
    username_out.clear();
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
    let mut has_username = false;

    loop {
        sc.skip_ws();
        key.clear();
        if !sc.parse_string(Some(key)) {
            return JwtClaimsResult::InvalidOrExpired;
        }
        sc.skip_ws();
        if sc.pos >= sc.s.len() || sc.s[sc.pos] != b':' {
            return JwtClaimsResult::InvalidOrExpired;
        }
        sc.pos += 1;
        sc.skip_ws();

        match key.as_slice() {
            b"exp" => {
                let start = sc.pos;
                if !sc.skip_number() {
                    if !sc.skip_value(1) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    exp_bad = true;
                } else {
                    match parse_json_int_seconds(&sc.s[start..sc.pos]) {
                        Some(v) => exp_val = Some(v),
                        None => exp_bad = true,
                    }
                }
            }
            b"nbf" => {
                let start = sc.pos;
                if !sc.skip_number() {
                    if !sc.skip_value(1) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    nbf_bad = true;
                } else {
                    match parse_json_int_seconds(&sc.s[start..sc.pos]) {
                        Some(v) => nbf_val = Some(v),
                        None => nbf_bad = true,
                    }
                }
            }
            b"sub" => {
                if sc.pos < sc.s.len() && sc.s[sc.pos] == b'"' {
                    sub_buf.clear();
                    if !sc.parse_string(Some(sub_buf)) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    sub_uid = parse_positive_id(sub_buf);
                } else {
                    if !sc.skip_value(1) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    sub_uid = None;
                }
            }
            b"username" => {
                if sc.pos < sc.s.len() && sc.s[sc.pos] == b'"' {
                    username_out.clear();
                    if !sc.parse_string(Some(username_out)) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    has_username = true;
                } else {
                    if !sc.skip_value(1) {
                        return JwtClaimsResult::InvalidOrExpired;
                    }
                    has_username = false;
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
    match (sub_uid, has_username) {
        (Some(uid), true) => JwtClaimsResult::Ok { user_id: uid },
        _ => JwtClaimsResult::InvalidPayload,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_write_i64_and_json_escape_long_runs() {
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

        let mut raw = Vec::new();
        raw.extend_from_slice(&[b'a'; 75]);
        raw.push(b'"');
        raw.extend_from_slice(&[b'b'; 37]);
        raw.push(b'\\');
        raw.push(b'\n');
        raw.push(0x01);
        raw.extend_from_slice(b"tail");

        let mut esc = Vec::new();
        write_json_escaped(&mut esc, &raw);
        let expected = format!(
            "{}\\\"{}\\\\\\n\\u0001tail",
            "a".repeat(75),
            "b".repeat(37)
        );
        assert_eq!(std::str::from_utf8(&esc).unwrap(), expected);
    }
}
