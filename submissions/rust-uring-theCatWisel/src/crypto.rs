use std::sync::atomic::{AtomicU8, Ordering};

const IV: [u32; 8] = [
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
];

#[repr(align(32))]
struct AlignedK([u32; 64]);

const K_ALIGNED: AlignedK = AlignedK([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

const MODE_UNKNOWN: u8 = 0;
const MODE_SCALAR: u8 = 1;
const MODE_AVX2_BMI2: u8 = 2;
const MODE_SHANI: u8 = 3;

static SHA_MODE: AtomicU8 = AtomicU8::new(MODE_UNKNOWN);

#[inline(always)]
fn sha_mode() -> u8 {
    let m = SHA_MODE.load(Ordering::Relaxed);
    if m != MODE_UNKNOWN {
        return m;
    }
    detect_sha_mode()
}

#[cold]
fn detect_sha_mode() -> u8 {
    let disable_shani = std::env::var("FEED_NO_SHANI")
        .map(|v| v == "1")
        .unwrap_or(false);
    #[cfg(target_arch = "x86_64")]
    {
        let chosen = if !disable_shani
            && std::arch::is_x86_feature_detected!("sha")
            && std::arch::is_x86_feature_detected!("sse4.1")
            && std::arch::is_x86_feature_detected!("ssse3")
        {
            MODE_SHANI
        } else if std::arch::is_x86_feature_detected!("avx2")
            && std::arch::is_x86_feature_detected!("bmi1")
            && std::arch::is_x86_feature_detected!("bmi2")
        {
            MODE_AVX2_BMI2
        } else {
            MODE_SCALAR
        };
        SHA_MODE.store(chosen, Ordering::Relaxed);
        chosen
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        let _ = disable_shani;
        SHA_MODE.store(MODE_SCALAR, Ordering::Relaxed);
        MODE_SCALAR
    }
}

fn compress_block_scalar(state: &mut [u32; 8], block: &[u8; 64]) {
    let k = &K_ALIGNED.0;
    let mut w = [0u32; 64];
    for i in 0..16 {
        w[i] = u32::from_be_bytes([
            block[4 * i],
            block[4 * i + 1],
            block[4 * i + 2],
            block[4 * i + 3],
        ]);
    }
    for i in 16..64 {
        let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
        let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16]
            .wrapping_add(s0)
            .wrapping_add(w[i - 7])
            .wrapping_add(s1);
    }

    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = *state;
    for i in 0..64 {
        let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let ch = (e & f) ^ ((!e) & g);
        let t1 = h
            .wrapping_add(s1)
            .wrapping_add(ch)
            .wrapping_add(k[i])
            .wrapping_add(w[i]);
        let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
        let maj = (a & (b ^ c)) ^ (b & c);
        let t2 = s0.wrapping_add(maj);
        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(t1);
        d = c;
        c = b;
        b = a;
        a = t1.wrapping_add(t2);
    }
    state[0] = state[0].wrapping_add(a);
    state[1] = state[1].wrapping_add(b);
    state[2] = state[2].wrapping_add(c);
    state[3] = state[3].wrapping_add(d);
    state[4] = state[4].wrapping_add(e);
    state[5] = state[5].wrapping_add(f);
    state[6] = state[6].wrapping_add(g);
    state[7] = state[7].wrapping_add(h);
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2,bmi1,bmi2")]
unsafe fn compress_block_avx2_bmi2(state: &mut [u32; 8], block: &[u8; 64]) {
    use std::arch::x86_64::*;

    #[repr(align(32))]
    struct AlignedWk([u32; 64]);
    let mut wk = AlignedWk([0u32; 64]);

    let bswap = _mm_set_epi64x(
        0x0c0d_0e0f_0809_0a0b_u64 as i64,
        0x0405_0607_0001_0203_u64 as i64,
    );
    let bptr = block.as_ptr() as *const __m128i;
    let kptr = K_ALIGNED.0.as_ptr() as *const __m128i;
    let wkptr = wk.0.as_mut_ptr() as *mut __m128i;

    let mut w0 = _mm_shuffle_epi8(_mm_loadu_si128(bptr), bswap);
    let mut w1 = _mm_shuffle_epi8(_mm_loadu_si128(bptr.add(1)), bswap);
    let mut w2 = _mm_shuffle_epi8(_mm_loadu_si128(bptr.add(2)), bswap);
    let mut w3 = _mm_shuffle_epi8(_mm_loadu_si128(bptr.add(3)), bswap);

    _mm_store_si128(wkptr, _mm_add_epi32(w0, _mm_load_si128(kptr)));
    _mm_store_si128(wkptr.add(1), _mm_add_epi32(w1, _mm_load_si128(kptr.add(1))));
    _mm_store_si128(wkptr.add(2), _mm_add_epi32(w2, _mm_load_si128(kptr.add(2))));
    _mm_store_si128(wkptr.add(3), _mm_add_epi32(w3, _mm_load_si128(kptr.add(3))));

    macro_rules! sigma0_xmm {
        ($x:expr) => {{
            let r7 = _mm_or_si128(_mm_srli_epi32($x, 7), _mm_slli_epi32($x, 25));
            let r18 = _mm_or_si128(_mm_srli_epi32($x, 18), _mm_slli_epi32($x, 14));
            let s3 = _mm_srli_epi32($x, 3);
            _mm_xor_si128(_mm_xor_si128(r7, r18), s3)
        }};
    }

    macro_rules! sigma1_xmm {
        ($x:expr) => {{
            let r17 = _mm_or_si128(_mm_srli_epi32($x, 17), _mm_slli_epi32($x, 15));
            let r19 = _mm_or_si128(_mm_srli_epi32($x, 19), _mm_slli_epi32($x, 13));
            let s10 = _mm_srli_epi32($x, 10);
            _mm_xor_si128(_mm_xor_si128(r17, r19), s10)
        }};
    }

    macro_rules! sched4 {
        ($q:expr, $wa:ident, $wb:ident, $wc:ident, $wd:ident) => {{
            let v15 = _mm_alignr_epi8($wb, $wa, 4);
            let v7 = _mm_alignr_epi8($wd, $wc, 4);
            let s0 = sigma0_xmm!(v15);
            let base = _mm_add_epi32(_mm_add_epi32($wa, s0), v7);
            let s1_lo = sigma1_xmm!(_mm_srli_si128($wd, 8));
            let mid = _mm_add_epi32(base, s1_lo);
            let s1_hi = sigma1_xmm!(_mm_slli_si128(mid, 8));
            $wa = _mm_add_epi32(mid, s1_hi);
            _mm_store_si128(wkptr.add($q), _mm_add_epi32($wa, _mm_load_si128(kptr.add($q))));
        }};
    }

    sched4!(4, w0, w1, w2, w3);
    sched4!(5, w1, w2, w3, w0);
    sched4!(6, w2, w3, w0, w1);
    sched4!(7, w3, w0, w1, w2);
    sched4!(8, w0, w1, w2, w3);
    sched4!(9, w1, w2, w3, w0);
    sched4!(10, w2, w3, w0, w1);
    sched4!(11, w3, w0, w1, w2);
    sched4!(12, w0, w1, w2, w3);
    sched4!(13, w1, w2, w3, w0);
    sched4!(14, w2, w3, w0, w1);
    sched4!(15, w3, w0, w1, w2);

    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut h] = *state;

    macro_rules! rnd {
        ($a:ident, $b:ident, $c:ident, $d:ident, $e:ident, $f:ident, $g:ident, $h:ident, $idx:expr) => {{
            let s1 = $e.rotate_right(6) ^ $e.rotate_right(11) ^ $e.rotate_right(25);
            let ch = ($e & $f) ^ ((!$e) & $g);
            let t1 = $h.wrapping_add(s1).wrapping_add(ch).wrapping_add(wk.0[$idx]);
            let s0 = $a.rotate_right(2) ^ $a.rotate_right(13) ^ $a.rotate_right(22);
            let maj = ($a & ($b ^ $c)) ^ ($b & $c);
            $d = $d.wrapping_add(t1);
            $h = t1.wrapping_add(s0).wrapping_add(maj);
        }};
    }

    macro_rules! rnd8 {
        ($base:expr) => {{
            rnd!(a, b, c, d, e, f, g, h, $base);
            rnd!(h, a, b, c, d, e, f, g, $base + 1);
            rnd!(g, h, a, b, c, d, e, f, $base + 2);
            rnd!(f, g, h, a, b, c, d, e, $base + 3);
            rnd!(e, f, g, h, a, b, c, d, $base + 4);
            rnd!(d, e, f, g, h, a, b, c, $base + 5);
            rnd!(c, d, e, f, g, h, a, b, $base + 6);
            rnd!(b, c, d, e, f, g, h, a, $base + 7);
        }};
    }

    rnd8!(0);
    rnd8!(8);
    rnd8!(16);
    rnd8!(24);
    rnd8!(32);
    rnd8!(40);
    rnd8!(48);
    rnd8!(56);

    state[0] = state[0].wrapping_add(a);
    state[1] = state[1].wrapping_add(b);
    state[2] = state[2].wrapping_add(c);
    state[3] = state[3].wrapping_add(d);
    state[4] = state[4].wrapping_add(e);
    state[5] = state[5].wrapping_add(f);
    state[6] = state[6].wrapping_add(g);
    state[7] = state[7].wrapping_add(h);
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "sha,sse4.1,ssse3")]
unsafe fn compress_block_shani(state: &mut [u32; 8], block: &[u8; 64]) {
    use std::arch::x86_64::*;

    let mask = _mm_set_epi64x(
        0x0c0d_0e0f_0809_0a0b_u64 as i64,
        0x0405_0607_0001_0203_u64 as i64,
    );
    let s_abef_init = _mm_loadu_si128(state.as_ptr() as *const __m128i);
    let s_cdgh_init = _mm_loadu_si128(state.as_ptr().add(4) as *const __m128i);

    let tmp = _mm_shuffle_epi32(s_abef_init, 0xB1);
    let s_cdgh_shuf = _mm_shuffle_epi32(s_cdgh_init, 0x1B);
    let mut abef = _mm_alignr_epi8(tmp, s_cdgh_shuf, 8);
    let mut cdgh = _mm_blend_epi16(s_cdgh_shuf, tmp, 0xF0);
    let abef_save = abef;
    let cdgh_save = cdgh;

    let ptr = block.as_ptr() as *const __m128i;
    let kptr = K_ALIGNED.0.as_ptr();
    let mut msg0 = _mm_shuffle_epi8(_mm_loadu_si128(ptr), mask);
    let mut msg1 = _mm_shuffle_epi8(_mm_loadu_si128(ptr.add(1)), mask);
    let mut msg2 = _mm_shuffle_epi8(_mm_loadu_si128(ptr.add(2)), mask);
    let mut msg3 = _mm_shuffle_epi8(_mm_loadu_si128(ptr.add(3)), mask);

    macro_rules! round4_plain {
        ($kidx:expr, $m_cur:ident) => {{
            let kv = _mm_load_si128(kptr.add($kidx) as *const __m128i);
            let wk = _mm_add_epi32($m_cur, kv);
            cdgh = _mm_sha256rnds2_epu32(cdgh, abef, wk);
            let wk_hi = _mm_srli_si128(wk, 8);
            abef = _mm_sha256rnds2_epu32(abef, cdgh, wk_hi);
        }};
    }

    macro_rules! round4_sched {
        ($kidx:expr, $m_cur:ident, $m_prev3:ident, $m_prev2:ident, $m_prev1:ident) => {{
            round4_plain!($kidx, $m_cur);
            $m_prev3 = _mm_sha256msg1_epu32($m_prev3, $m_prev2);
            let t = _mm_alignr_epi8($m_cur, $m_prev1, 4);
            $m_prev3 = _mm_add_epi32($m_prev3, t);
            $m_prev3 = _mm_sha256msg2_epu32($m_prev3, $m_cur);
        }};
    }

    round4_plain!(0, msg0);
    round4_plain!(4, msg1);
    round4_plain!(8, msg2);
    round4_sched!(12, msg3, msg0, msg1, msg2);
    round4_sched!(16, msg0, msg1, msg2, msg3);
    round4_sched!(20, msg1, msg2, msg3, msg0);
    round4_sched!(24, msg2, msg3, msg0, msg1);
    round4_sched!(28, msg3, msg0, msg1, msg2);
    round4_sched!(32, msg0, msg1, msg2, msg3);
    round4_sched!(36, msg1, msg2, msg3, msg0);
    round4_sched!(40, msg2, msg3, msg0, msg1);
    round4_sched!(44, msg3, msg0, msg1, msg2);
    round4_sched!(48, msg0, msg1, msg2, msg3);
    round4_sched!(52, msg1, msg2, msg3, msg0);
    round4_sched!(56, msg2, msg3, msg0, msg1);
    round4_plain!(60, msg3);

    abef = _mm_add_epi32(abef, abef_save);
    cdgh = _mm_add_epi32(cdgh, cdgh_save);

    let t = _mm_shuffle_epi32(abef, 0x1B);
    let c = _mm_shuffle_epi32(cdgh, 0xB1);
    let out0 = _mm_blend_epi16(t, c, 0xF0);
    let out1 = _mm_alignr_epi8(c, t, 8);

    _mm_storeu_si128(state.as_mut_ptr() as *mut __m128i, out0);
    _mm_storeu_si128(state.as_mut_ptr().add(4) as *mut __m128i, out1);
}

#[inline(always)]
fn compress_block(state: &mut [u32; 8], block: &[u8; 64]) {
    #[cfg(target_arch = "x86_64")]
    {
        match sha_mode() {
            MODE_SHANI => unsafe {
                compress_block_shani(state, block);
                return;
            },
            MODE_AVX2_BMI2 => unsafe {
                compress_block_avx2_bmi2(state, block);
                return;
            },
            _ => {}
        }
    }
    compress_block_scalar(state, block);
}

#[inline(always)]
fn finalize_from_midstate(mut state: [u32; 8], prefix_bytes: u64, msg: &[u8]) -> [u8; 32] {
    let bit_len = (prefix_bytes + msg.len() as u64) * 8;
    let mut rem = msg;
    while rem.len() >= 64 {
        let blk: &[u8; 64] = rem[..64].try_into().unwrap();
        compress_block(&mut state, blk);
        rem = &rem[64..];
    }
    let mut tail = [0u8; 64];
    let rlen = rem.len();
    if rlen > 0 {
        tail[..rlen].copy_from_slice(rem);
    }
    tail[rlen] = 0x80;
    if rlen >= 56 {
        compress_block(&mut state, &tail);
        tail = [0u8; 64];
    }
    tail[56..64].copy_from_slice(&bit_len.to_be_bytes());
    compress_block(&mut state, &tail);

    let mut out = [0u8; 32];
    for (i, &w) in state.iter().enumerate() {
        out[4 * i..4 * i + 4].copy_from_slice(&w.to_be_bytes());
    }
    out
}

pub fn sha256_once(data: &[u8]) -> [u8; 32] {
    finalize_from_midstate(IV, 0, data)
}

#[derive(Clone, Copy)]
pub struct HmacSha256Key {
    inner_midstate: [u32; 8],
    outer_midstate: [u32; 8],
}

impl HmacSha256Key {
    pub fn new(key: &[u8]) -> Self {
        let _ = sha_mode();
        let mut k = [0u8; 64];
        if key.len() > 64 {
            let digest = sha256_once(key);
            k[..32].copy_from_slice(&digest);
        } else {
            k[..key.len()].copy_from_slice(key);
        }

        let mut ipad = [0x36u8; 64];
        let mut opad = [0x5cu8; 64];
        for i in 0..64 {
            ipad[i] ^= k[i];
            opad[i] ^= k[i];
        }

        let mut inner_midstate = IV;
        compress_block(&mut inner_midstate, &ipad);

        let mut outer_midstate = IV;
        compress_block(&mut outer_midstate, &opad);

        Self {
            inner_midstate,
            outer_midstate,
        }
    }

    #[inline(always)]
    pub fn sign(&self, msg: &[u8]) -> [u8; 32] {
        let inner_digest = finalize_from_midstate(self.inner_midstate, 64, msg);

        let mut outer_blk = [0u8; 64];
        outer_blk[..32].copy_from_slice(&inner_digest);
        outer_blk[32] = 0x80;
        outer_blk[56..64].copy_from_slice(&(96u64 * 8).to_be_bytes());

        let mut out_state = self.outer_midstate;
        compress_block(&mut out_state, &outer_blk);

        let mut out = [0u8; 32];
        for (i, &w) in out_state.iter().enumerate() {
            out[4 * i..4 * i + 4].copy_from_slice(&w.to_be_bytes());
        }
        out
    }

    #[inline(always)]
    pub fn verify_ct(&self, msg: &[u8], expected_sig: &[u8; 32]) -> bool {
        let actual = self.sign(msg);
        let mut diff = 0u8;
        for i in 0..32 {
            diff |= actual[i] ^ expected_sig[i];
        }
        diff == 0
    }
}

pub fn hmac_sha256_once(key: &[u8], msg: &[u8]) -> [u8; 32] {
    HmacSha256Key::new(key).sign(msg)
}

pub fn pbkdf2_hmac_sha256(password: &[u8], salt: &[u8], iterations: u32) -> [u8; 32] {
    let prf = HmacSha256Key::new(password);
    let mut salt_blk = Vec::with_capacity(salt.len() + 4);
    salt_blk.extend_from_slice(salt);
    salt_blk.extend_from_slice(&1u32.to_be_bytes());

    let mut u = prf.sign(&salt_blk);
    let mut out = u;
    for _ in 1..iterations {
        u = prf.sign(&u);
        for i in 0..32 {
            out[i] ^= u[i];
        }
    }
    out
}

const B64URL_DEC: [i8; 256] = {
    let mut t = [-1i8; 256];
    let alpha = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut i = 0;
    while i < 64 {
        t[alpha[i] as usize] = i as i8;
        i += 1;
    }
    t
};

const B64STD_DEC: [i8; 256] = {
    let mut t = [-1i8; 256];
    let alpha = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut i = 0;
    while i < 64 {
        t[alpha[i] as usize] = i as i8;
        i += 1;
    }
    t
};

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "ssse3")]
unsafe fn base64url_decode_ssse3<'a>(src: &[u8], dst: &'a mut [u8]) -> Option<&'a [u8]> {
    use std::arch::x86_64::*;

    let n = src.len();
    if n % 4 == 1 {
        return None;
    }
    let needed = (n * 3) / 4;
    if dst.len() < needed {
        return None;
    }

    let mut out_idx = 0usize;
    let mut i = 0usize;

    let zero = _mm_setzero_si128();
    let c_a_up = _mm_set1_epi8(b'A' as i8);
    let c_a_lo = _mm_set1_epi8(b'a' as i8);
    let c_0 = _mm_set1_epi8(b'0' as i8);
    let span_25 = _mm_set1_epi8(25);
    let span_9 = _mm_set1_epi8(9);
    let c_dash = _mm_set1_epi8(b'-' as i8);
    let c_under = _mm_set1_epi8(b'_' as i8);
    let off_up = _mm_set1_epi8(65);
    let off_lo = _mm_set1_epi8(71);
    let off_09 = _mm_set1_epi8(4);
    let val_62 = _mm_set1_epi8(62);
    let val_63 = _mm_set1_epi8(63);
    let merge_ab = _mm_set1_epi32(0x0140_0140);
    let merge_abcd = _mm_set1_epi32(0x0001_1000);
    let pack_12 = _mm_setr_epi8(2, 1, 0, 6, 5, 4, 10, 9, 8, 14, 13, 12, -1, -1, -1, -1);

    let src_ptr = src.as_ptr();
    let dst_ptr = dst.as_mut_ptr();

    while i + 16 <= n && out_idx + 16 <= dst.len() {
        let v = _mm_loadu_si128(src_ptr.add(i) as *const __m128i);

        let d_up = _mm_sub_epi8(v, c_a_up);
        let m_up = _mm_cmpeq_epi8(_mm_subs_epu8(d_up, span_25), zero);

        let d_lo = _mm_sub_epi8(v, c_a_lo);
        let m_lo = _mm_cmpeq_epi8(_mm_subs_epu8(d_lo, span_25), zero);

        let d_09 = _mm_sub_epi8(v, c_0);
        let m_09 = _mm_cmpeq_epi8(_mm_subs_epu8(d_09, span_9), zero);

        let m_dash = _mm_cmpeq_epi8(v, c_dash);
        let m_under = _mm_cmpeq_epi8(v, c_under);

        let valid = _mm_or_si128(
            _mm_or_si128(_mm_or_si128(m_up, m_lo), m_09),
            _mm_or_si128(m_dash, m_under),
        );
        if _mm_movemask_epi8(valid) != 0xFFFF {
            return None;
        }

        let sextets = _mm_or_si128(
            _mm_or_si128(
                _mm_and_si128(m_up, _mm_sub_epi8(v, off_up)),
                _mm_and_si128(m_lo, _mm_sub_epi8(v, off_lo)),
            ),
            _mm_or_si128(
                _mm_and_si128(m_09, _mm_add_epi8(v, off_09)),
                _mm_or_si128(_mm_and_si128(m_dash, val_62), _mm_and_si128(m_under, val_63)),
            ),
        );

        let t16 = _mm_maddubs_epi16(sextets, merge_ab);
        let t32 = _mm_madd_epi16(t16, merge_abcd);
        let packed = _mm_shuffle_epi8(t32, pack_12);
        _mm_storeu_si128(dst_ptr.add(out_idx) as *mut __m128i, packed);

        i += 16;
        out_idx += 12;
    }

    base64url_decode_tail(src, dst, i, out_idx)
}

#[inline(always)]
fn base64url_decode_tail<'a>(
    src: &[u8],
    dst: &'a mut [u8],
    mut i: usize,
    mut out_idx: usize,
) -> Option<&'a [u8]> {
    let n = src.len();
    while i + 4 <= n {
        let a = B64URL_DEC[src[i] as usize];
        let b = B64URL_DEC[src[i + 1] as usize];
        let c = B64URL_DEC[src[i + 2] as usize];
        let d = B64URL_DEC[src[i + 3] as usize];
        if (a | b | c | d) < 0 {
            return None;
        }
        let v = ((a as u32) << 18) | ((b as u32) << 12) | ((c as u32) << 6) | (d as u32);
        dst[out_idx] = (v >> 16) as u8;
        dst[out_idx + 1] = (v >> 8) as u8;
        dst[out_idx + 2] = v as u8;
        out_idx += 3;
        i += 4;
    }
    let rem = n - i;
    if rem == 2 {
        let a = B64URL_DEC[src[i] as usize];
        let b = B64URL_DEC[src[i + 1] as usize];
        if (a | b) < 0 {
            return None;
        }
        let v = ((a as u32) << 12) | ((b as u32) << 6);
        dst[out_idx] = (v >> 10) as u8;
        out_idx += 1;
    } else if rem == 3 {
        let a = B64URL_DEC[src[i] as usize];
        let b = B64URL_DEC[src[i + 1] as usize];
        let c = B64URL_DEC[src[i + 2] as usize];
        if (a | b | c) < 0 {
            return None;
        }
        let v = ((a as u32) << 12) | ((b as u32) << 6) | (c as u32);
        dst[out_idx] = (v >> 10) as u8;
        dst[out_idx + 1] = (v >> 2) as u8;
        out_idx += 2;
    }
    Some(&dst[..out_idx])
}

#[inline(always)]
pub fn base64url_decode<'a>(src: &[u8], dst: &'a mut [u8]) -> Option<&'a [u8]> {
    #[cfg(target_arch = "x86_64")]
    {
        if sha_mode() >= MODE_AVX2_BMI2 || std::arch::is_x86_feature_detected!("ssse3") {
            return unsafe { base64url_decode_ssse3(src, dst) };
        }
    }
    let n = src.len();
    if n % 4 == 1 {
        return None;
    }
    let needed = (n * 3) / 4;
    if dst.len() < needed {
        return None;
    }
    base64url_decode_tail(src, dst, 0, 0)
}

pub fn base64_std_encode(src: &[u8]) -> String {
    const ALPHA: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = Vec::with_capacity(((src.len() + 2) / 3) * 4);
    let mut i = 0;
    while i < src.len() {
        let b0 = src[i] as u32;
        let b1 = if i + 1 < src.len() { src[i + 1] as u32 } else { 0 };
        let b2 = if i + 2 < src.len() { src[i + 2] as u32 } else { 0 };
        let v = (b0 << 16) | (b1 << 8) | b2;
        out.push(ALPHA[((v >> 18) & 63) as usize]);
        out.push(ALPHA[((v >> 12) & 63) as usize]);
        out.push(if i + 1 < src.len() { ALPHA[((v >> 6) & 63) as usize] } else { b'=' });
        out.push(if i + 2 < src.len() { ALPHA[(v & 63) as usize] } else { b'=' });
        i += 3;
    }
    unsafe { String::from_utf8_unchecked(out) }
}

pub fn base64_std_decode(src: &[u8]) -> Option<Vec<u8>> {
    let trimmed = match src.iter().position(|&b| b == b'=') {
        Some(pos) => &src[..pos],
        None => src,
    };
    if trimmed.len() % 4 == 1 {
        return None;
    }
    let mut out = Vec::with_capacity((trimmed.len() * 3) / 4);
    let mut acc = 0u32;
    let mut bits = 0u32;
    for &b in trimmed {
        let v = B64STD_DEC[b as usize];
        if v < 0 {
            return None;
        }
        acc = (acc << 6) | (v as u32);
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_and_hmac_vectors() {
        let d = sha256_once(b"abc");
        let expected = [
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
        ];
        assert_eq!(d, expected);

        let key = HmacSha256Key::new(b"key");
        let mac = key.sign(b"The quick brown fox jumps over the lazy dog");
        let expected_mac = [
            0xf7, 0xbc, 0x83, 0xf4, 0x30, 0x53, 0x84, 0x24,
            0xb1, 0x32, 0x98, 0xe6, 0xaa, 0x6f, 0xb1, 0x43,
            0xef, 0x4d, 0x59, 0xa1, 0x49, 0x46, 0x17, 0x59,
            0x97, 0x47, 0x9d, 0xbc, 0x2d, 0x1a, 0x3c, 0xd8,
        ];
        assert_eq!(mac, expected_mac);
        assert!(key.verify_ct(b"The quick brown fox jumps over the lazy dog", &expected_mac));
    }

    #[test]
    fn sha256_all_backends_match_across_lengths() {
        for len in 0..=512 {
            let msg: Vec<u8> = (0..len).map(|k| ((k * 41 + 19) & 0xFF) as u8).collect();
            SHA_MODE.store(MODE_SCALAR, Ordering::Relaxed);
            let d_scalar = hmac_sha256_once(b"million-users-challenge", &msg);

            #[cfg(target_arch = "x86_64")]
            {
                if std::arch::is_x86_feature_detected!("avx2")
                    && std::arch::is_x86_feature_detected!("bmi1")
                    && std::arch::is_x86_feature_detected!("bmi2")
                {
                    SHA_MODE.store(MODE_AVX2_BMI2, Ordering::Relaxed);
                    let d_avx2 = hmac_sha256_once(b"million-users-challenge", &msg);
                    assert_eq!(d_scalar, d_avx2, "avx2_bmi2 mismatch at len={}", len);
                }
                if std::arch::is_x86_feature_detected!("sha")
                    && std::arch::is_x86_feature_detected!("sse4.1")
                    && std::arch::is_x86_feature_detected!("ssse3")
                {
                    SHA_MODE.store(MODE_SHANI, Ordering::Relaxed);
                    let d_shani = hmac_sha256_once(b"million-users-challenge", &msg);
                    assert_eq!(d_scalar, d_shani, "shani mismatch at len={}", len);
                }
            }
        }
        SHA_MODE.store(MODE_UNKNOWN, Ordering::Relaxed);
        let _ = sha_mode();
    }

    #[test]
    fn base64url_simd_matches_scalar_all_lengths_and_bytes() {
        const URL_ALPHA: &[u8; 64] =
            b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for len in 0..=180 {
            if len % 4 == 1 {
                let bad = vec![b'A'; len];
                let mut d1 = [0u8; 256];
                assert!(base64url_decode(&bad, &mut d1).is_none());
                continue;
            }
            let mut enc = Vec::with_capacity(len);
            for k in 0..len {
                enc.push(URL_ALPHA[(k * 17 + len * 7) & 63]);
            }
            let mut d_simd = [0u8; 256];
            let mut d_scal = [0u8; 256];
            let r_simd = base64url_decode(&enc, &mut d_simd).unwrap().to_vec();
            let r_scal = base64url_decode_tail(&enc, &mut d_scal, 0, 0)
                .unwrap()
                .to_vec();
            assert_eq!(r_simd, r_scal, "mismatch at len={}", len);
        }

        for pos in 0..24 {
            for b in 0u8..=255 {
                let mut enc = *b"eyJhbGciOiJIUzI1NiIsInR5";
                enc[pos] = b;
                let mut d_simd = [0u8; 64];
                let mut d_scal = [0u8; 64];
                let r1 = base64url_decode(&enc, &mut d_simd).map(|s| s.to_vec());
                let r2 = base64url_decode_tail(&enc, &mut d_scal, 0, 0).map(|s| s.to_vec());
                assert_eq!(r1, r2, "byte={} at pos={}", b, pos);
            }
        }
    }
}
