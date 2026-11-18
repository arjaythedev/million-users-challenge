use std::sync::LazyLock;

const HASH_BITS: u32 = 12;
const MIN_MATCH: usize = 4;
const MAX_MATCH: usize = 258;
const WINDOW: usize = 32768;
const INSERT_MAX: usize = 4;

const LEN_BASE: [u16; 29] = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEN_EXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DIST_BASE: [u16; 30] = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DIST_EXTRA: [u8; 30] = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];

struct Tables {
    lit: [u32; 256],
    len: [u32; MAX_MATCH + 1],
    dist_sym: [u8; 512],
    eob: u32,
}

fn reverse(code: u32, bits: u32) -> u32 {
    code.reverse_bits() >> (32 - bits)
}

fn fixed(sym: u32) -> (u32, u32) {
    match sym {
        0..=143 => (0x30 + sym, 8),
        144..=255 => (0x190 + sym - 144, 9),
        256..=279 => (sym - 256, 7),
        _ => (0xC0 + sym - 280, 8),
    }
}

static TABLES: LazyLock<Tables> = LazyLock::new(|| {
    let mut t = Tables { lit: [0; 256], len: [0; MAX_MATCH + 1], dist_sym: [0; 512], eob: 0 };
    for s in 0..256u32 {
        let (c, b) = fixed(s);
        t.lit[s as usize] = reverse(c, b) | b << 24;
    }
    for c in 0..29 {
        let to = if c + 1 < 29 { LEN_BASE[c + 1] as usize } else { MAX_MATCH + 1 };
        for l in LEN_BASE[c] as usize..to {
            let (code, b) = fixed(257 + c as u32);
            let extra = (l - LEN_BASE[c] as usize) as u32;
            let bits = reverse(code, b) | extra << b;
            t.len[l] = bits | (b + LEN_EXTRA[c] as u32) << 24;
        }
    }
    let (code, b) = fixed(285);
    t.len[258] = reverse(code, b) | b << 24;
    for c in 0..30 {
        let from = DIST_BASE[c] as usize - 1;
        let to = from + (1 << DIST_EXTRA[c]);
        for d in from..to {
            let k = if d < 256 { d } else { 256 + (d >> 7) };
            t.dist_sym[k] = c as u8;
        }
    }
    let (c, b) = fixed(256);
    t.eob = reverse(c, b) | b << 24;
    t
});

pub struct Gzip {
    head: Vec<i32>,
    base: i32,
}

pub fn bound(n: usize) -> usize {
    n + (n >> 3) + 64
}

#[inline(always)]
unsafe fn rd32(p: *const u8) -> u32 {
    unsafe { (p as *const u32).read_unaligned() }
}

#[inline(always)]
unsafe fn rd64(p: *const u8) -> u64 {
    unsafe { (p as *const u64).read_unaligned() }
}

impl Gzip {
    pub fn new() -> Self {
        LazyLock::force(&TABLES);
        Gzip { head: vec![0; 1 << HASH_BITS], base: WINDOW as i32 + 1 }
    }

    pub fn compress(&mut self, input: &[u8], out: &mut Vec<u8>) {
        let n = input.len();
        out.reserve(bound(n));
        if self.base > i32::MAX - n as i32 - WINDOW as i32 - 1 {
            self.head.iter_mut().for_each(|h| *h = 0);
            self.base = WINDOW as i32 + 1;
        }
        let base = self.base;
        self.base += n as i32 + WINDOW as i32 + 1;
        let t = &*TABLES;
        let crc = crc32(0, input);
        unsafe {
            let start = out.len();
            let dst = out.as_mut_ptr().add(start);
            std::ptr::copy_nonoverlapping([0x1fu8, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff].as_ptr(), dst, 10);
            let mut op = dst.add(10);
            let mut bits: u64 = 0b011;
            let mut nb: u32 = 3;
            let src = input.as_ptr();
            let head = self.head.as_mut_ptr();
            macro_rules! put {
                ($v:expr) => {{
                    let v: u32 = $v;
                    bits |= ((v & 0xFF_FFFF) as u64) << nb;
                    nb += v >> 24;
                }};
            }
            macro_rules! flush {
                () => {{
                    (op as *mut u64).write_unaligned(bits);
                    let bytes = nb >> 3;
                    op = op.add(bytes as usize);
                    bits = if bytes == 8 { 0 } else { bits >> (bytes * 8) };
                    nb &= 7;
                }};
            }
            let mut i = 0usize;
            if n >= MIN_MATCH {
                let last = n - MIN_MATCH;
                while i <= last {
                    let v = rd32(src.add(i));
                    let h = (v.wrapping_mul(0x9E37_79B1) >> (32 - HASH_BITS)) as usize;
                    let cand = *head.add(h) - base;
                    *head.add(h) = i as i32 + base;
                    let dist = (i as i32).wrapping_sub(cand);
                    if (dist - 1) as u32 >= WINDOW as u32 || rd32(src.add(cand as usize)) != v {
                        put!(*t.lit.get_unchecked(*src.add(i) as usize));
                        flush!();
                        i += 1;
                        continue;
                    }
                    let c = cand as usize;
                    let limit = (n - i).min(MAX_MATCH);
                    let mut m = MIN_MATCH;
                    loop {
                        if m + 8 <= limit {
                            let x = rd64(src.add(i + m)) ^ rd64(src.add(c + m));
                            if x != 0 {
                                m += (x.trailing_zeros() >> 3) as usize;
                                break;
                            }
                            m += 8;
                        } else {
                            while m < limit && *src.add(i + m) == *src.add(c + m) {
                                m += 1;
                            }
                            break;
                        }
                    }
                    put!(*t.len.get_unchecked(m));
                    let d = dist as usize - 1;
                    let sym = *t.dist_sym.get_unchecked(if d < 256 { d } else { 256 + (d >> 7) }) as usize;
                    let dbits = reverse(sym as u32, 5) | ((d + 1 - DIST_BASE[sym] as usize) as u32) << 5;
                    flush!();
                    put!(dbits | (5 + DIST_EXTRA[sym] as u32) << 24);
                    flush!();
                    let end = i + m;
                    let to = end.min(last + 1).min(i + 1 + INSERT_MAX);
                    let mut p = i + 1;
                    while p < to {
                        let hv = rd32(src.add(p));
                        *head.add((hv.wrapping_mul(0x9E37_79B1) >> (32 - HASH_BITS)) as usize) = p as i32 + base;
                        p += 1;
                    }
                    i = end;
                }
            }
            while i < n {
                put!(*t.lit.get_unchecked(*src.add(i) as usize));
                flush!();
                i += 1;
            }
            put!(t.eob);
            flush!();
            if nb > 0 {
                *op = bits as u8;
                op = op.add(1);
            }
            (op as *mut u32).write_unaligned(crc.to_le());
            (op.add(4) as *mut u32).write_unaligned((n as u32).to_le());
            op = op.add(8);
            out.set_len(start + op.offset_from(dst) as usize);
        }
    }
}

static CRC_TABLE: [[u32; 256]; 8] = {
    let mut t = [[0u32; 256]; 8];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[0][i] = c;
        i += 1;
    }
    let mut i = 0;
    while i < 256 {
        let mut s = 1;
        while s < 8 {
            let prev = t[s - 1][i];
            t[s][i] = (prev >> 8) ^ t[0][(prev & 0xff) as usize];
            s += 1;
        }
        i += 1;
    }
    t
};

pub fn crc32_table(crc: u32, data: &[u8]) -> u32 {
    let mut c = !crc;
    let mut chunks = data.chunks_exact(8);
    for b in &mut chunks {
        let lo = u32::from_le_bytes([b[0], b[1], b[2], b[3]]) ^ c;
        let hi = u32::from_le_bytes([b[4], b[5], b[6], b[7]]);
        c = CRC_TABLE[7][(lo & 0xff) as usize]
            ^ CRC_TABLE[6][(lo >> 8 & 0xff) as usize]
            ^ CRC_TABLE[5][(lo >> 16 & 0xff) as usize]
            ^ CRC_TABLE[4][(lo >> 24) as usize]
            ^ CRC_TABLE[3][(hi & 0xff) as usize]
            ^ CRC_TABLE[2][(hi >> 8 & 0xff) as usize]
            ^ CRC_TABLE[1][(hi >> 16 & 0xff) as usize]
            ^ CRC_TABLE[0][(hi >> 24) as usize];
    }
    for &b in chunks.remainder() {
        c = CRC_TABLE[0][((c ^ b as u32) & 0xff) as usize] ^ (c >> 8);
    }
    !c
}

#[cfg(target_arch = "x86_64")]
static HW: LazyLock<bool> =
    LazyLock::new(|| is_x86_feature_detected!("pclmulqdq") && is_x86_feature_detected!("sse4.1"));

#[cfg(target_arch = "aarch64")]
static HW: LazyLock<bool> = LazyLock::new(|| std::arch::is_aarch64_feature_detected!("crc"));

pub fn crc32(crc: u32, data: &[u8]) -> u32 {
    #[cfg(all(target_arch = "x86_64", target_feature = "pclmulqdq", target_feature = "sse4.1"))]
    return unsafe { crc32_hw(crc, data) };
    #[allow(unreachable_code)]
    #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
    if *HW {
        return unsafe { crc32_hw(crc, data) };
    }
    crc32_table(crc, data)
}

#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "crc")]
unsafe fn crc32_hw(crc: u32, data: &[u8]) -> u32 {
    use std::arch::aarch64::{__crc32b, __crc32d};
    let mut c = !crc;
    let mut chunks = data.chunks_exact(8);
    for b in &mut chunks {
        c = __crc32d(c, u64::from_le_bytes(b.try_into().unwrap()));
    }
    for &b in chunks.remainder() {
        c = __crc32b(c, b);
    }
    !c
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "pclmulqdq", enable = "sse2", enable = "sse4.1")]
unsafe fn crc32_hw(crc: u32, mut data: &[u8]) -> u32 {
    use std::arch::x86_64::*;
    const K1: i64 = 0x154442bd4;
    const K2: i64 = 0x1c6e41596;
    const K3: i64 = 0x1751997d0;
    const K4: i64 = 0x0ccaa009e;
    const K5: i64 = 0x163cd6124;
    const P_X: i64 = 0x1DB710641;
    const U_PRIME: i64 = 0x1F7011641;

    if data.len() < 128 {
        return crc32_table(crc, data);
    }
    #[inline(always)]
    unsafe fn get(d: &mut &[u8]) -> __m128i {
        let r = unsafe { _mm_loadu_si128(d.as_ptr() as *const __m128i) };
        *d = &d[16..];
        r
    }
    #[inline(always)]
    unsafe fn fold(a: __m128i, b: __m128i, k: __m128i) -> __m128i {
        unsafe {
            let t1 = _mm_clmulepi64_si128(a, k, 0x00);
            let t2 = _mm_clmulepi64_si128(a, k, 0x11);
            _mm_xor_si128(_mm_xor_si128(b, t1), t2)
        }
    }
    unsafe {
        let mut x3 = get(&mut data);
        let mut x2 = get(&mut data);
        let mut x1 = get(&mut data);
        let mut x0 = get(&mut data);
        x3 = _mm_xor_si128(x3, _mm_cvtsi32_si128(!crc as i32));
        let k1k2 = _mm_set_epi64x(K2, K1);
        while data.len() >= 64 {
            x3 = fold(x3, get(&mut data), k1k2);
            x2 = fold(x2, get(&mut data), k1k2);
            x1 = fold(x1, get(&mut data), k1k2);
            x0 = fold(x0, get(&mut data), k1k2);
        }
        let k3k4 = _mm_set_epi64x(K4, K3);
        let mut x = fold(x3, x2, k3k4);
        x = fold(x, x1, k3k4);
        x = fold(x, x0, k3k4);
        while data.len() >= 16 {
            x = fold(x, get(&mut data), k3k4);
        }
        let x = _mm_xor_si128(_mm_clmulepi64_si128(x, k3k4, 0x10), _mm_srli_si128(x, 8));
        let low32 = _mm_set_epi32(0, 0, 0, !0);
        let x = _mm_xor_si128(
            _mm_clmulepi64_si128(_mm_and_si128(x, low32), _mm_set_epi64x(0, K5), 0x00),
            _mm_srli_si128(x, 4),
        );
        let pu = _mm_set_epi64x(U_PRIME, P_X);
        let t1 = _mm_clmulepi64_si128(_mm_and_si128(x, low32), pu, 0x10);
        let t2 = _mm_clmulepi64_si128(_mm_and_si128(t1, low32), pu, 0x00);
        let c = _mm_extract_epi32(_mm_xor_si128(x, t2), 1) as u32;
        if data.is_empty() { !c } else { crc32_table(!c, data) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn inflate_fixed(d: &[u8]) -> Vec<u8> {
        let mut pos = 0usize;
        let bit = |pos: &mut usize| -> u32 {
            let b = (d[*pos >> 3] >> (*pos & 7)) & 1;
            *pos += 1;
            b as u32
        };
        let bits = |pos: &mut usize, n: u32| -> u32 {
            let mut v = 0;
            for k in 0..n {
                v |= bit(pos) << k;
            }
            v
        };
        assert_eq!(bits(&mut pos, 3), 0b011);
        let mut out: Vec<u8> = Vec::new();
        loop {
            let mut code = 0u32;
            let mut len = 0;
            let sym;
            loop {
                code = code << 1 | bit(&mut pos);
                len += 1;
                if len == 7 && code <= 0x17 {
                    sym = code + 256;
                    break;
                }
                if len == 8 && (0x30..=0xBF).contains(&code) {
                    sym = code - 0x30;
                    break;
                }
                if len == 8 && (0xC0..=0xC7).contains(&code) {
                    sym = code - 0xC0 + 280;
                    break;
                }
                if len == 9 {
                    sym = code - 0x190 + 144;
                    break;
                }
            }
            if sym < 256 {
                out.push(sym as u8);
                continue;
            }
            if sym == 256 {
                break;
            }
            let c = (sym - 257) as usize;
            let l = LEN_BASE[c] as usize + bits(&mut pos, LEN_EXTRA[c] as u32) as usize;
            let mut ds = 0u32;
            for _ in 0..5 {
                ds = ds << 1 | bit(&mut pos);
            }
            let ds = ds as usize;
            let dist = DIST_BASE[ds] as usize + bits(&mut pos, DIST_EXTRA[ds] as u32) as usize;
            for _ in 0..l {
                let b = out[out.len() - dist];
                out.push(b);
            }
        }
        out
    }

    #[test]
    fn crc() {
        #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
        eprintln!("hardware crc: {}", *HW);
        assert_eq!(crc32_table(0, b"123456789"), 0xCBF43926);
        assert_eq!(crc32(0, &[b'x'; 300]), crc32_table(0, &[b'x'; 300]));
        let data: Vec<u8> = (0..5000u32).map(|i| (i * 2654435761u32 >> 13) as u8).collect();
        for n in [0, 1, 15, 16, 63, 64, 127, 128, 129, 200, 1000, 4999, 5000] {
            assert_eq!(crc32(0, &data[..n]), crc32_table(0, &data[..n]), "n={n}");
        }
    }

    #[test]
    fn round_trip() {
        let mut g = Gzip::new();
        let mut feed = Vec::new();
        for i in 0..20 {
            feed.extend_from_slice(format!(r#"{{"id":{},"body":"post number {} says hello","created_at":"2025-12-31T23:59:19.409Z","author":"user_{}","like_count":{}}},"#, 1000 + i, i * 7, i % 3, i).as_bytes());
        }
        for input in [&b""[..], b"a", b"abcd", b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &feed] {
            for _ in 0..3 {
                let mut out = Vec::new();
                g.compress(input, &mut out);
                assert_eq!(&out[..3], &[0x1f, 0x8b, 8]);
                let n = out.len();
                let got = inflate_fixed(&out[10..n - 8]);
                assert_eq!(got, input);
                assert_eq!(u32::from_le_bytes(out[n - 8..n - 4].try_into().unwrap()), crc32_table(0, input));
                assert_eq!(u32::from_le_bytes(out[n - 4..].try_into().unwrap()), input.len() as u32);
                assert!(out.len() <= bound(input.len()));
            }
        }
        let mut out = Vec::new();
        g.compress(&feed, &mut out);
        eprintln!("feed {} -> {}", feed.len(), out.len());
    }
}
