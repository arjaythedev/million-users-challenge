//! SHA-256, HMAC-SHA256, PBKDF2 and base64, as needed for HS256 tokens and the SCRAM-SHA-256 login.

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
];

/// The 64 words of a block's message schedule, each with its round constant already added.
fn schedule(p: &[u8]) -> [u32; 64] {
    let mut w = [0u32; 64];
    for i in 0..16 {
        w[i] = u32::from_be_bytes([p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]]);
    }
    for i in 16..64 {
        let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
        let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
    }
    for (w, k) in w.iter_mut().zip(K) {
        *w = w.wrapping_add(k);
    }
    w
}

/// One round, with the working variables named by the caller so that nothing moves between rounds.
macro_rules! round {
    ($wk:ident, $a:ident, $b:ident, $c:ident, $d:ident, $e:ident, $f:ident, $g:ident, $h:ident, $i:expr) => {{
        let t1 = $h
            .wrapping_add($e.rotate_right(6) ^ $e.rotate_right(11) ^ $e.rotate_right(25))
            .wrapping_add($g ^ ($e & ($f ^ $g)))
            .wrapping_add($wk[$i]);
        let t2 = ($a.rotate_right(2) ^ $a.rotate_right(13) ^ $a.rotate_right(22)).wrapping_add(($a & $b) | ($c & ($a | $b)));
        $d = $d.wrapping_add(t1);
        $h = t1.wrapping_add(t2);
    }};
}

fn block_plain(h: &mut [u32; 8], p: &[u8]) {
    let wk = schedule(p);
    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = *h;
    let mut i = 0;
    while i < 64 {
        round!(wk, a, b, c, d, e, f, g, hh, i);
        round!(wk, hh, a, b, c, d, e, f, g, i + 1);
        round!(wk, g, hh, a, b, c, d, e, f, i + 2);
        round!(wk, f, g, hh, a, b, c, d, e, i + 3);
        round!(wk, e, f, g, hh, a, b, c, d, i + 4);
        round!(wk, d, e, f, g, hh, a, b, c, i + 5);
        round!(wk, c, d, e, f, g, hh, a, b, i + 6);
        round!(wk, b, c, d, e, f, g, hh, a, i + 7);
        i += 8;
    }
    for (x, y) in h.iter_mut().zip([a, b, c, d, e, f, g, hh]) {
        *x = x.wrapping_add(y);
    }
}

/// A block with the schedule worked out four words per step, each step placed 16 rounds ahead of the rounds
/// that use it: the rounds are one long chain of additions, and the schedule runs beside it instead of before.
/// A word of the schedule needs the one two places before it, so a step is done in two halves, the lower two
/// words first and the upper two from those.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "ssse3")]
unsafe fn block_ssse3(h: &mut [u32; 8], p: &[u8]) {
    use core::arch::x86_64::*;
    debug_assert!(p.len() >= 64);
    let mut wk = [0u32; 64];
    let big_endian = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    let k = |i: usize| _mm_loadu_si128(K.as_ptr().add(i).cast());
    let mut x = [_mm_setzero_si128(); 4];
    for (i, x) in x.iter_mut().enumerate() {
        *x = _mm_shuffle_epi8(_mm_loadu_si128(p.as_ptr().add(16 * i).cast()), big_endian);
        _mm_storeu_si128(wk.as_mut_ptr().add(4 * i).cast(), _mm_add_epi32(*x, k(4 * i)));
    }
    let small1 = |v: __m128i| {
        let r17 = _mm_or_si128(_mm_srli_epi32(v, 17), _mm_slli_epi32(v, 15));
        let r19 = _mm_or_si128(_mm_srli_epi32(v, 19), _mm_slli_epi32(v, 13));
        _mm_xor_si128(_mm_xor_si128(r17, r19), _mm_srli_epi32(v, 10))
    };
    macro_rules! four_words {
        ($at:expr) => {{
            let [x0, x1, x2, x3] = x;
            let behind15 = _mm_alignr_epi8(x1, x0, 4);
            let r7 = _mm_or_si128(_mm_srli_epi32(behind15, 7), _mm_slli_epi32(behind15, 25));
            let r18 = _mm_or_si128(_mm_srli_epi32(behind15, 18), _mm_slli_epi32(behind15, 14));
            let small0 = _mm_xor_si128(_mm_xor_si128(r7, r18), _mm_srli_epi32(behind15, 3));
            let partial = _mm_add_epi32(_mm_add_epi32(x0, small0), _mm_alignr_epi8(x3, x2, 4));
            let lower = _mm_add_epi32(partial, _mm_srli_si128(small1(x3), 8));
            let new = _mm_add_epi32(lower, _mm_slli_si128(small1(lower), 8));
            _mm_storeu_si128(wk.as_mut_ptr().add($at).cast(), _mm_add_epi32(new, k($at)));
            x = [x1, x2, x3, new];
        }};
    }
    let [mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh] = *h;
    let mut i = 0;
    while i < 64 {
        if i < 48 {
            four_words!(i + 16);
        }
        round!(wk, a, b, c, d, e, f, g, hh, i);
        round!(wk, hh, a, b, c, d, e, f, g, i + 1);
        round!(wk, g, hh, a, b, c, d, e, f, i + 2);
        round!(wk, f, g, hh, a, b, c, d, e, i + 3);
        if i < 48 {
            four_words!(i + 20);
        }
        round!(wk, e, f, g, hh, a, b, c, d, i + 4);
        round!(wk, d, e, f, g, hh, a, b, c, i + 5);
        round!(wk, c, d, e, f, g, hh, a, b, i + 6);
        round!(wk, b, c, d, e, f, g, hh, a, i + 7);
        i += 8;
    }
    for (x, y) in h.iter_mut().zip([a, b, c, d, e, f, g, hh]) {
        *x = x.wrapping_add(y);
    }
}

#[derive(Clone)]
pub struct Sha256 {
    h: [u32; 8],
    buf: [u8; 64],
    fill: usize,
    total: u64,
}

impl Sha256 {
    pub fn new() -> Sha256 {
        Sha256 {
            h: [
                0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
            ],
            buf: [0; 64],
            fill: 0,
            total: 0,
        }
    }

    /// One block. Without the SHA instructions (the benchmark box has none) this is most of the cost of
    /// checking a token: four blocks each.
    fn block(h: &mut [u32; 8], p: &[u8]) {
        #[cfg(target_arch = "x86_64")]
        if std::is_x86_feature_detected!("ssse3") {
            return unsafe { block_ssse3(h, p) };
        }
        block_plain(h, p)
    }

    pub fn update(&mut self, mut data: &[u8]) {
        self.total += data.len() as u64;
        if self.fill > 0 {
            let n = (64 - self.fill).min(data.len());
            self.buf[self.fill..self.fill + n].copy_from_slice(&data[..n]);
            self.fill += n;
            data = &data[n..];
            if self.fill < 64 {
                return;
            }
            let b = self.buf;
            Self::block(&mut self.h, &b);
            self.fill = 0;
        }
        while data.len() >= 64 {
            Self::block(&mut self.h, &data[..64]);
            data = &data[64..];
        }
        self.buf[..data.len()].copy_from_slice(data);
        self.fill = data.len();
    }

    pub fn finish(mut self) -> [u8; 32] {
        let bits = self.total.wrapping_mul(8);
        let mut pad = [0u8; 72];
        pad[0] = 0x80;
        let n = if self.fill < 56 { 56 - self.fill } else { 120 - self.fill };
        pad[n..n + 8].copy_from_slice(&bits.to_be_bytes());
        self.update(&pad[..n + 8]);
        let mut out = [0u8; 32];
        for (i, v) in self.h.iter().enumerate() {
            out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
        }
        out
    }
}

pub fn sha256(data: &[u8]) -> [u8; 32] {
    let mut s = Sha256::new();
    s.update(data);
    s.finish()
}

/// HMAC-SHA256 with the two key blocks already absorbed, so each MAC costs only the message.
#[derive(Clone)]
pub struct Hmac {
    inner: Sha256,
    outer: Sha256,
}

impl Hmac {
    pub fn new(key: &[u8]) -> Hmac {
        let mut k = [0u8; 64];
        if key.len() > 64 {
            k[..32].copy_from_slice(&sha256(key));
        } else {
            k[..key.len()].copy_from_slice(key);
        }
        let mut inner = Sha256::new();
        let mut outer = Sha256::new();
        inner.update(&k.map(|b| b ^ 0x36));
        outer.update(&k.map(|b| b ^ 0x5c));
        Hmac { inner, outer }
    }

    pub fn mac(&self, parts: &[&[u8]]) -> [u8; 32] {
        let mut i = self.inner.clone();
        for p in parts {
            i.update(p);
        }
        let mut o = self.outer.clone();
        o.update(&i.finish());
        o.finish()
    }
}

pub fn pbkdf2(password: &[u8], salt: &[u8], iterations: u32) -> [u8; 32] {
    let h = Hmac::new(password);
    let mut u = h.mac(&[salt, &[0, 0, 0, 1]]);
    let mut out = u;
    for _ in 1..iterations {
        u = h.mac(&[&u]);
        for (o, x) in out.iter_mut().zip(u) {
            *o ^= x;
        }
    }
    out
}

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn b64_encode(input: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(input.len().div_ceil(3) * 4);
    for chunk in input.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let n = (b[0] as u32) << 16 | (b[1] as u32) << 8 | b[2] as u32;
        out.push(B64[(n >> 18) as usize & 63]);
        out.push(B64[(n >> 12) as usize & 63]);
        out.push(if chunk.len() > 1 { B64[(n >> 6) as usize & 63] } else { b'=' });
        out.push(if chunk.len() > 2 { B64[n as usize & 63] } else { b'=' });
    }
    out
}

/// The six bits of each character of either base64 alphabet; 0xff for every other byte.
const B64_VALUE: [u8; 256] = {
    let mut t = [0xffu8; 256];
    let mut i = 0;
    while i < 64 {
        t[B64[i] as usize] = i as u8;
        i += 1;
    }
    t[b'-' as usize] = 62;
    t[b'_' as usize] = 63;
    t
};

/// Decodes base64 in either alphabet, with or without padding, into `out` and returns the decoded length.
/// `None` on any other character, an impossible length, or an `out` that is too short.
pub fn b64_decode_into(input: &[u8], out: &mut [u8]) -> Option<usize> {
    let mut end = input.len();
    while end > 0 && input[end - 1] == b'=' {
        end -= 1;
    }
    let input = &input[..end];
    let n = input.len() * 3 / 4;
    if input.len() % 4 == 1 || n > out.len() {
        return None;
    }
    let value = |c: u8| B64_VALUE[c as usize] as u32;
    let mut seen = 0;
    let whole = input.len() / 4;
    for (q, o) in input.chunks_exact(4).zip(out.chunks_exact_mut(3)) {
        let (a, b, c, d) = (value(q[0]), value(q[1]), value(q[2]), value(q[3]));
        seen |= a | b | c | d;
        let x = a << 18 | b << 12 | c << 6 | d;
        o.copy_from_slice(&[(x >> 16) as u8, (x >> 8) as u8, x as u8]);
    }
    match input[whole * 4..] {
        [p, q] => {
            let (a, b) = (value(p), value(q));
            seen |= a | b;
            out[whole * 3] = (a << 2 | b >> 4) as u8;
        }
        [p, q, r] => {
            let (a, b, c) = (value(p), value(q), value(r));
            seen |= a | b | c;
            out[whole * 3] = (a << 2 | b >> 4) as u8;
            out[whole * 3 + 1] = (b << 4 | c >> 2) as u8;
        }
        _ => {}
    }
    (seen < 64).then_some(n)
}

pub fn b64_decode(input: &[u8]) -> Option<Vec<u8>> {
    let mut out = vec![0; input.len() * 3 / 4];
    let n = b64_decode_into(input, &mut out)?;
    out.truncate(n);
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    #[test]
    fn sha256_vectors() {
        assert_eq!(hex(&sha256(b"")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        assert_eq!(hex(&sha256(b"abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert_eq!(
            hex(&sha256(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
        let million = vec![b'a'; 1_000_000];
        assert_eq!(hex(&sha256(&million)), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
        // every split point of a 200-byte message gives the same digest
        let msg: Vec<u8> = (0..200u8).collect();
        let want = sha256(&msg);
        for cut in 0..msg.len() {
            let mut s = Sha256::new();
            s.update(&msg[..cut]);
            s.update(&msg[cut..]);
            assert_eq!(s.finish(), want);
        }
    }

    #[test]
    fn both_blocks_agree() {
        let mut x = 0x2545_f491u32;
        for _ in 0..200 {
            let block: [u8; 64] = core::array::from_fn(|_| {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                (x >> 11) as u8
            });
            let (mut plain, mut fast) = ([x; 8], [x; 8]);
            block_plain(&mut plain, &block);
            Sha256::block(&mut fast, &block);
            assert_eq!(plain, fast);
        }
    }

    #[test]
    fn hmac_vectors() {
        // RFC 4231 cases 1, 2 and 6 (key longer than a block)
        assert_eq!(
            hex(&Hmac::new(&[0x0b; 20]).mac(&[b"Hi There"])),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
        );
        assert_eq!(
            hex(&Hmac::new(b"Jefe").mac(&[b"what do ya want ", b"for nothing?"])),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
        );
        assert_eq!(
            hex(&Hmac::new(&[0xaa; 131]).mac(&[b"Test Using Larger Than Block-Size Key - Hash Key First"])),
            "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
        );
    }

    #[test]
    fn pbkdf2_vectors() {
        assert_eq!(
            hex(&pbkdf2(b"password", b"salt", 1)),
            "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"
        );
        assert_eq!(
            hex(&pbkdf2(b"password", b"salt", 4096)),
            "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"
        );
    }

    #[test]
    #[ignore = "a timing, not a check: cargo test --release -- --ignored --nocapture timing"]
    fn timing() {
        let block = [0x5au8; 64];
        let best = |f: &dyn Fn() -> u32| {
            (0..7)
                .map(|_| {
                    let t = std::time::Instant::now();
                    let mut sum = 0u32;
                    for _ in 0..300_000 {
                        sum = sum.wrapping_add(f());
                    }
                    std::hint::black_box(sum);
                    t.elapsed().as_nanos() as f64 / 300_000.0
                })
                .fold(f64::MAX, f64::min)
        };
        let plain = best(&|| {
            let mut h = [1u32; 8];
            block_plain(&mut h, std::hint::black_box(&block));
            h[0]
        });
        let block = best(&|| {
            let mut h = [1u32; 8];
            Sha256::block(&mut h, std::hint::black_box(&block));
            h[0]
        });
        println!("one block: {block:.0} ns, with the schedule first and word by word: {plain:.0} ns");
    }

    #[test]
    fn base64_into_a_buffer() {
        let mut out = [0u8; 8];
        assert_eq!(b64_decode_into(b"Zm9vYmFy", &mut out), Some(6));
        assert_eq!(&out[..6], b"foobar");
        assert_eq!(b64_decode_into(b"Zm9vYmE=", &mut out), Some(5));
        assert_eq!(b64_decode_into(b"Zm9vYg", &mut out), Some(4));
        assert_eq!(&out[..4], b"foob");
        assert_eq!(b64_decode_into(b"-_-_", &mut out), b64_decode_into(b"+/+/", &mut [0u8; 3]));
        assert_eq!(b64_decode_into(b"", &mut out), Some(0));
        assert_eq!(b64_decode_into(b"Zm9vYmFyZm9v", &mut out), None, "too long for the buffer");
        for bad in [&b"Z"[..], b"Zm9vY", b"Zm9v!mFy", b"Zm 9", b"Zm9\xff", b"Zm=9", b"Z.9v"] {
            assert_eq!(b64_decode_into(bad, &mut out), None, "{bad:?}");
        }
    }

    #[test]
    fn base64_round_trip() {
        assert_eq!(b64_encode(b""), b"");
        assert_eq!(b64_encode(b"f"), b"Zg==");
        assert_eq!(b64_encode(b"fo"), b"Zm8=");
        assert_eq!(b64_encode(b"foobar"), b"Zm9vYmFy");
        assert_eq!(b64_decode(b"Zm9vYmFy").unwrap(), b"foobar");
        assert_eq!(b64_decode(b"Zg").unwrap(), b"f");
        assert_eq!(b64_decode(b"Zg==").unwrap(), b"f");
        assert_eq!(b64_decode(b"-_-_").unwrap(), b64_decode(b"+/+/").unwrap());
        assert!(b64_decode(b"Z").is_none());
        assert!(b64_decode(b"Zm9v!").is_none());
    }
}
