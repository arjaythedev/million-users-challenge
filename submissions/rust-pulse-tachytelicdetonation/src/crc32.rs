const fn make_tables() -> [[u32; 256]; 8] {
    let mut t = [[0u32; 256]; 8];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { 0xEDB88320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[0][i] = c;
        i += 1;
    }
    let mut i = 0;
    while i < 256 {
        let mut s = 1;
        while s < 8 {
            let p = t[s - 1][i];
            t[s][i] = (p >> 8) ^ t[0][(p & 0xff) as usize];
            s += 1;
        }
        i += 1;
    }
    t
}

static T: [[u32; 256]; 8] = make_tables();

pub fn update_table(mut c: u32, mut d: &[u8]) -> u32 {
    while d.len() >= 8 {
        let a = c ^ u32::from_le_bytes([d[0], d[1], d[2], d[3]]);
        c = T[7][(a & 0xff) as usize]
            ^ T[6][((a >> 8) & 0xff) as usize]
            ^ T[5][((a >> 16) & 0xff) as usize]
            ^ T[4][(a >> 24) as usize]
            ^ T[3][d[4] as usize]
            ^ T[2][d[5] as usize]
            ^ T[1][d[6] as usize]
            ^ T[0][d[7] as usize];
        d = &d[8..];
    }
    for &b in d {
        c = T[0][((c ^ b as u32) & 0xff) as usize] ^ (c >> 8);
    }
    c
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "crc")]
unsafe fn update_hw(mut c: u32, d: &[u8]) -> u32 {
    use std::arch::aarch64::*;
    let mut chunks = d.chunks_exact(8);
    for ch in &mut chunks {
        c = __crc32d(c, u64::from_le_bytes(ch.try_into().unwrap()));
    }
    for &b in chunks.remainder() {
        c = __crc32b(c, b);
    }
    c
}

#[cfg(all(target_arch = "x86_64", target_feature = "pclmulqdq", target_feature = "sse4.1"))]
unsafe fn update_hw(crc: u32, d: &[u8]) -> u32 {
    use std::arch::x86_64::*;
    if d.len() < 64 {
        return update_table(crc, d);
    }
    let n = d.len() & !15;
    let p = d.as_ptr();
    let ld = |o: usize| _mm_loadu_si128(p.add(o) as *const __m128i);
    let k1k2 = _mm_set_epi64x(0x1c6e41596, 0x154442bd4);
    let k3k4 = _mm_set_epi64x(0x0ccaa009e, 0x1751997d0);
    let k5 = _mm_set_epi64x(0, 0x163cd6124);
    let poly = _mm_set_epi64x(0x1F7011641, 0x1DB710641);
    let mask32 = _mm_set_epi32(0, 0, 0, -1);
    let mut x1 = _mm_xor_si128(ld(0), _mm_cvtsi32_si128(crc as i32));
    let mut x2 = ld(16);
    let mut x3 = ld(32);
    let mut x4 = ld(48);
    let mut o = 64;
    while o + 64 <= n {
        let f = |x: __m128i, y: __m128i| {
            _mm_xor_si128(
                _mm_xor_si128(_mm_clmulepi64_si128::<0x00>(x, k1k2), _mm_clmulepi64_si128::<0x11>(x, k1k2)),
                y,
            )
        };
        x1 = f(x1, ld(o));
        x2 = f(x2, ld(o + 16));
        x3 = f(x3, ld(o + 32));
        x4 = f(x4, ld(o + 48));
        o += 64;
    }
    let g = |x: __m128i, y: __m128i| {
        _mm_xor_si128(
            _mm_xor_si128(_mm_clmulepi64_si128::<0x00>(x, k3k4), _mm_clmulepi64_si128::<0x11>(x, k3k4)),
            y,
        )
    };
    x1 = g(x1, x2);
    x1 = g(x1, x3);
    x1 = g(x1, x4);
    while o + 16 <= n {
        x1 = g(x1, ld(o));
        o += 16;
    }
    let x = _mm_xor_si128(_mm_clmulepi64_si128::<0x10>(x1, k3k4), _mm_srli_si128::<8>(x1));
    let x = _mm_xor_si128(_mm_clmulepi64_si128::<0x00>(_mm_and_si128(x, mask32), k5), _mm_srli_si128::<4>(x));
    let t = _mm_clmulepi64_si128::<0x10>(_mm_and_si128(x, mask32), poly);
    let t = _mm_clmulepi64_si128::<0x00>(_mm_and_si128(t, mask32), poly);
    let r = _mm_extract_epi32::<1>(_mm_xor_si128(t, x)) as u32;
    update_table(r, &d[n..])
}

#[cfg(target_arch = "aarch64")]
fn hw_ok() -> bool {
    std::arch::is_aarch64_feature_detected!("crc")
}

#[cfg(all(target_arch = "x86_64", target_feature = "pclmulqdq", target_feature = "sse4.1"))]
fn hw_ok() -> bool {
    true
}

pub fn crc32(d: &[u8]) -> u32 {
    #[cfg(any(target_arch = "aarch64", all(target_arch = "x86_64", target_feature = "pclmulqdq", target_feature = "sse4.1")))]
    {
        if hw_ok() {
            return !unsafe { update_hw(!0, d) };
        }
    }
    !update_table(!0, d)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_table() {
        assert_eq!(crc32(b"123456789"), 0xCBF43926);
        assert_eq!(crc32(b""), 0);
        let mut x = 0x12345678u32;
        let data: Vec<u8> = (0..5000)
            .map(|_| {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                x as u8
            })
            .collect();
        for len in 0..600 {
            for start in 0..3 {
                let s = &data[start..start + len];
                assert_eq!(crc32(s), !update_table(!0, s), "len {} start {}", len, start);
            }
        }
        assert_eq!(crc32(&data), !update_table(!0, &data));
    }
}
