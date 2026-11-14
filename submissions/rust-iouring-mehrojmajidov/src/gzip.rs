//! gzip for response bodies: one deflate block with the fixed Huffman codes and greedy matches from a hash table.
//! A feed is a few KB, where the setup cost of a general-purpose library is larger than the work itself.

/// 16 KB of table, half of a 32 KB first-level cache. Inside the server the size makes no measurable difference
/// between 4 KB and 64 KB (a feed's 3.6 KB spread over the table either way); above that it gets slower.
const HASH_BITS: u32 = 12;
const WINDOW: usize = 32_768;
const MIN_MATCH: usize = 4;
const MAX_MATCH: usize = 258;

const LEN_BASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
];
const LEN_EXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const fn reverse(code: u32, bits: u32) -> u32 {
    code.reverse_bits() >> (32 - bits)
}

/// (bits, bit count) of a literal/length symbol in the fixed code, ready to write low bit first.
const fn fixed_symbol(sym: u32) -> (u32, u32) {
    match sym {
        0..=143 => (reverse(0x30 + sym, 8), 8),
        144..=255 => (reverse(0x190 + sym - 144, 9), 9),
        256..=279 => (reverse(sym - 256, 7), 7),
        _ => (reverse(0xc0 + sym - 280, 8), 8),
    }
}

const LITERAL: [(u16, u8); 256] = {
    let mut t = [(0u16, 0u8); 256];
    let mut i = 0;
    while i < 256 {
        let (b, n) = fixed_symbol(i as u32);
        t[i] = (b as u16, n as u8);
        i += 1;
    }
    t
};

/// For each match length: the symbol and its extra bits as one unit.
const LENGTH: [(u32, u8); MAX_MATCH + 1] = {
    let mut t = [(0u32, 0u8); MAX_MATCH + 1];
    let mut len = 3;
    while len <= MAX_MATCH {
        let mut s = 28;
        while LEN_BASE[s] as usize > len {
            s -= 1;
        }
        let (b, n) = fixed_symbol(257 + s as u32);
        let extra = (len - LEN_BASE[s] as usize) as u32;
        t[len] = (b | extra << n, (n + LEN_EXTRA[s] as u32) as u8);
        len += 1;
    }
    t
};

/// The fixed 5-bit distance codes, ready to write low bit first.
const DIST_CODE: [u32; 30] = {
    let mut t = [0u32; 30];
    let mut s = 0;
    while s < 30 {
        t[s] = reverse(s as u32, 5);
        s += 1;
    }
    t
};

const CRC: [[u32; 256]; 8] = {
    let mut t = [[0u32; 256]; 8];
    let mut i = 0;
    while i < 256 {
        let mut c = i as u32;
        let mut k = 0;
        while k < 8 {
            c = if c & 1 != 0 { 0xedb8_8320 ^ (c >> 1) } else { c >> 1 };
            k += 1;
        }
        t[0][i] = c;
        i += 1;
    }
    let mut i = 0;
    while i < 256 {
        let mut s = 1;
        while s < 8 {
            t[s][i] = (t[s - 1][i] >> 8) ^ t[0][(t[s - 1][i] & 0xff) as usize];
            s += 1;
        }
        i += 1;
    }
    t
};

/// CRC-32 by table, eight bytes per step. `state` is the running value before the final inversion.
fn crc32_table(mut c: u32, data: &[u8]) -> u32 {
    let mut chunks = data.chunks_exact(8);
    for p in &mut chunks {
        let lo = u32::from_le_bytes([p[0], p[1], p[2], p[3]]) ^ c;
        let hi = u32::from_le_bytes([p[4], p[5], p[6], p[7]]);
        c = CRC[7][(lo & 0xff) as usize]
            ^ CRC[6][(lo >> 8 & 0xff) as usize]
            ^ CRC[5][(lo >> 16 & 0xff) as usize]
            ^ CRC[4][(lo >> 24) as usize]
            ^ CRC[3][(hi & 0xff) as usize]
            ^ CRC[2][(hi >> 8 & 0xff) as usize]
            ^ CRC[1][(hi >> 16 & 0xff) as usize]
            ^ CRC[0][(hi >> 24) as usize];
    }
    for &b in chunks.remainder() {
        c = CRC[0][((c ^ b as u32) & 0xff) as usize] ^ (c >> 8);
    }
    c
}

/// CRC-32 by carry-less multiplication (Gopal et al., "Fast CRC Computation for Generic Polynomials Using
/// PCLMULQDQ Instruction"): four 128-bit lanes folded 64 bytes at a time, then reduced to 32 bits.
/// `data` must be at least 64 bytes and a multiple of 16.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "pclmulqdq,sse4.1")]
unsafe fn crc32_clmul(state: u32, data: &[u8]) -> u32 {
    use core::arch::x86_64::*;
    debug_assert!(data.len() >= 64 && data.len() % 16 == 0);
    let load = |at: usize| _mm_loadu_si128(data.as_ptr().add(at).cast());
    let fold = |x: __m128i, k: __m128i, next: __m128i| {
        _mm_xor_si128(_mm_xor_si128(_mm_clmulepi64_si128(x, k, 0x00), _mm_clmulepi64_si128(x, k, 0x11)), next)
    };

    let k1k2 = _mm_set_epi64x(0x01_c6e4_1596, 0x01_5444_2bd4);
    let (mut x1, mut x2, mut x3, mut x4) = (load(0), load(16), load(32), load(48));
    x1 = _mm_xor_si128(x1, _mm_cvtsi32_si128(state as i32));
    let mut at = 64;
    while at + 64 <= data.len() {
        x1 = fold(x1, k1k2, load(at));
        x2 = fold(x2, k1k2, load(at + 16));
        x3 = fold(x3, k1k2, load(at + 32));
        x4 = fold(x4, k1k2, load(at + 48));
        at += 64;
    }

    let k3k4 = _mm_set_epi64x(0x00_ccaa_009e, 0x01_7519_97d0);
    x1 = fold(x1, k3k4, x2);
    x1 = fold(x1, k3k4, x3);
    x1 = fold(x1, k3k4, x4);
    while at + 16 <= data.len() {
        x1 = fold(x1, k3k4, load(at));
        at += 16;
    }

    // 128 bits to 64
    let low32 = _mm_setr_epi32(!0, 0, !0, 0);
    let x2 = _mm_clmulepi64_si128(x1, k3k4, 0x10);
    x1 = _mm_xor_si128(_mm_srli_si128(x1, 8), x2);
    let k5 = _mm_set_epi64x(0, 0x01_63cd_6124);
    let x2 = _mm_srli_si128(x1, 4);
    x1 = _mm_xor_si128(_mm_clmulepi64_si128(_mm_and_si128(x1, low32), k5, 0x00), x2);

    // Barrett reduction to 32
    let poly = _mm_set_epi64x(0x01_f701_1641, 0x01_db71_0641);
    let mut t = _mm_and_si128(x1, low32);
    t = _mm_clmulepi64_si128(t, poly, 0x10);
    t = _mm_and_si128(t, low32);
    t = _mm_clmulepi64_si128(t, poly, 0x00);
    _mm_extract_epi32(_mm_xor_si128(x1, t), 1) as u32
}

pub fn crc32(data: &[u8]) -> u32 {
    let mut state = !0u32;
    let mut rest = data;
    #[cfg(target_arch = "x86_64")]
    if data.len() >= 64 && std::is_x86_feature_detected!("pclmulqdq") && std::is_x86_feature_detected!("sse4.1") {
        let whole = data.len() & !15;
        state = unsafe { crc32_clmul(state, &data[..whole]) };
        rest = &data[whole..];
    }
    !crc32_table(state, rest)
}

pub struct Gzip {
    /// Position of the last occurrence of each 4-byte hash, offset by `base`. It is never cleared:
    /// every call's positions lie above the previous call's, so older entries read as "none".
    table: Box<[u32; 1 << HASH_BITS]>,
    base: u32,
}

impl Gzip {
    pub fn new() -> Gzip {
        Gzip { table: Box::new([0; 1 << HASH_BITS]), base: 1 }
    }

    /// Appends `input` to `out` as one gzip member.
    pub fn compress(&mut self, input: &[u8], out: &mut Vec<u8>) {
        let n = input.len();
        if self.base as usize + n >= u32::MAX as usize {
            self.table.fill(0);
            self.base = 1;
        }
        let base = self.base;
        self.base += n as u32 + 1;

        // Worst case: nine bits per byte. The slack covers the header, the trailer and the 8-byte stores below.
        let start = out.len();
        out.reserve(n + n / 8 + 64);

        // SAFETY: every read is inside `input` (each load is guarded by a length check), every table index is
        // below 2^HASH_BITS by construction of the hash, and the writes stay inside the capacity reserved
        // above: a symbol adds at most 31 bits, and `flush` stores 8 bytes at a position that is always more
        // than 8 bytes short of the reserved end.
        unsafe {
            let src = input.as_ptr();
            let table = self.table.as_mut_ptr();
            let mut dst = out.as_mut_ptr().add(start);
            core::ptr::copy_nonoverlapping([0x1fu8, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3].as_ptr(), dst, 10);
            dst = dst.add(10);

            // bits wait in `acc` (low bit first) until `flush` moves the whole bytes out
            let mut acc: u64 = 0b011; // final block, fixed Huffman codes
            let mut used: u32 = 3;
            macro_rules! put {
                ($bits:expr, $count:expr) => {{
                    acc |= ($bits as u64) << used;
                    used += $count as u32;
                }};
            }
            macro_rules! flush {
                () => {{
                    (dst as *mut u64).write_unaligned(acc.to_le());
                    dst = dst.add((used >> 3) as usize);
                    acc >>= used & !7;
                    used &= 7;
                }};
            }
            let load32 = |at: usize| u32::from_le((src.add(at) as *const u32).read_unaligned());
            let load64 = |at: usize| u64::from_le((src.add(at) as *const u64).read_unaligned());
            let hash = |v: u32| (v.wrapping_mul(2_654_435_761) >> (32 - HASH_BITS)) as usize;

            let mut i = 0;
            while i + MIN_MATCH <= n {
                let seq = load32(i);
                let slot = table.add(hash(seq));
                let seen = *slot;
                *slot = base + i as u32;
                if seen >= base {
                    let j = (seen - base) as usize;
                    let dist = i - j;
                    if dist <= WINDOW && load32(j) == seq {
                        let limit = MAX_MATCH.min(n - i);
                        let mut len = MIN_MATCH;
                        'extend: {
                            while len + 8 <= limit {
                                let diff = load64(i + len) ^ load64(j + len);
                                if diff != 0 {
                                    len += (diff.trailing_zeros() >> 3) as usize;
                                    break 'extend;
                                }
                                len += 8;
                            }
                            while len < limit && *src.add(i + len) == *src.add(j + len) {
                                len += 1;
                            }
                        }
                        // The distance code by arithmetic: above 4 it is twice the position of the highest
                        // bit of dist - 1 plus the bit below it, and the bits below that follow as extra bits.
                        let x = dist as u32 - 1;
                        let (sym, extra) = if x < 4 {
                            (x, 0)
                        } else {
                            let high = 31 - x.leading_zeros();
                            (2 * high + ((x >> (high - 1)) & 1), high - 1)
                        };
                        let (bits, count) = LENGTH[len];
                        let far = DIST_CODE[sym as usize] as u64 | ((x & ((1 << extra) - 1)) as u64) << 5;
                        put!(bits as u64 | far << count, count as u32 + 5 + extra);
                        flush!();
                        // Positions inside a match are not remembered: most of a feed is matches, and
                        // indexing them costs more time than it saves bytes (1% of the output).
                        i += len;
                        continue;
                    }
                }
                let (bits, count) = LITERAL[*src.add(i) as usize];
                put!(bits, count);
                // The byte after an unmatched one goes out as a literal too, remembered but not searched: half
                // the searches in a run of literals, for a match that now and then starts a byte late (6% more
                // output, 13% less time).
                if i + 1 + MIN_MATCH <= n {
                    *table.add(hash(load32(i + 1))) = base + i as u32 + 1;
                    let (bits, count) = LITERAL[*src.add(i + 1) as usize];
                    put!(bits, count);
                    i += 1;
                }
                flush!();
                i += 1;
            }
            while i < n {
                let (bits, count) = LITERAL[*src.add(i) as usize];
                put!(bits, count);
                flush!();
                i += 1;
            }
            let (eob, eob_bits) = fixed_symbol(256);
            put!(eob, eob_bits);
            // the last bits, padded to a whole byte
            (dst as *mut u64).write_unaligned(acc.to_le());
            dst = dst.add(used.div_ceil(8) as usize);

            core::ptr::copy_nonoverlapping(crc32(input).to_le_bytes().as_ptr(), dst, 4);
            core::ptr::copy_nonoverlapping((n as u32).to_le_bytes().as_ptr(), dst.add(4), 4);
            out.set_len(dst.add(8).offset_from(out.as_ptr()) as usize);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::process::{Command, Stdio};

    fn gunzip(data: &[u8]) -> Vec<u8> {
        let mut child = Command::new("gzip")
            .arg("-dc")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .expect("the gzip tool is needed for this test");
        let mut stdin = child.stdin.take().unwrap();
        let owned = data.to_vec();
        let feeder = std::thread::spawn(move || stdin.write_all(&owned));
        let out = child.wait_with_output().unwrap();
        feeder.join().unwrap().ok();
        assert!(out.status.success(), "gzip -d failed: {}", String::from_utf8_lossy(&out.stderr));
        out.stdout
    }

    fn round_trip(g: &mut Gzip, input: &[u8]) -> usize {
        let mut out = Vec::new();
        g.compress(input, &mut out);
        assert_eq!(gunzip(&out), input, "round trip of {} bytes", input.len());
        out.len()
    }

    #[test]
    fn crc_vectors() {
        assert_eq!(crc32(b""), 0);
        assert_eq!(crc32(b"123456789"), 0xcbf4_3926);
        assert_eq!(crc32(b"The quick brown fox jumps over the lazy dog"), 0x414f_a339);
    }

    #[test]
    fn crc_paths_agree() {
        // every length around the 64-byte and 16-byte boundaries of the carry-less path, against the table
        let mut x = 0x2545_f491u32;
        let data: Vec<u8> = (0..5_000)
            .map(|_| {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                x as u8
            })
            .collect();
        for len in (0..400).chain([1023, 1024, 1025, 3678, 4096, 4999, 5000]) {
            assert_eq!(crc32(&data[..len]), !crc32_table(!0, &data[..len]), "length {len}");
        }
        assert_eq!(crc32(&[0u8; 64]), 0x758d_6336);
        assert_eq!(crc32(&[0xffu8; 80]), !crc32_table(!0, &[0xffu8; 80]));
    }

    #[test]
    fn round_trips() {
        let mut g = Gzip::new();
        round_trip(&mut g, b"");
        round_trip(&mut g, b"a");
        round_trip(&mut g, b"abc");
        round_trip(&mut g, b"abcabcabcabcabcabcabcabcabcabc");
        round_trip(&mut g, &[0u8; 100_000]);

        let every_byte: Vec<u8> = (0..=255u8).cycle().take(4_000).collect();
        round_trip(&mut g, &every_byte);

        // pseudo-random bytes: no matches, the worst case for size
        let mut x = 0x9e37_79b9u32;
        let noise: Vec<u8> = (0..70_000)
            .map(|_| {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                (x >> 24) as u8
            })
            .collect();
        let size = round_trip(&mut g, &noise);
        assert!(size <= noise.len() + noise.len() / 8 + 32);

        // repeats farther apart than the 32 KB window must not be used as matches
        let mut far = noise[..40_000].to_vec();
        far.extend_from_slice(&noise[..40_000]);
        round_trip(&mut g, &far);

        // matches of every length, at short and long distances
        let mut varied = Vec::new();
        for len in 3..=300usize {
            varied.extend_from_slice(&noise[len..len + 7]);
            varied.extend_from_slice(&noise[..len]);
        }
        round_trip(&mut g, &varied);
    }

    /// A feed as it looks during a load test: twenty short posts written by the load generator.
    pub fn live_feed() -> Vec<u8> {
        live_feed_from(12345)
    }

    pub fn live_feed_from(seed: u32) -> Vec<u8> {
        let mut x = seed;
        let mut next = move |n: u32| {
            x = x.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            (x >> 8) % n
        };
        let adjectives = ["quiet", "brave", "clever", "sunny", "rusty", "swift", "golden", "curious"];
        let nouns = ["otter", "falcon", "ember", "glacier", "nebula", "thistle", "willow", "pebble"];
        let mut out = String::from("{\"posts\":[");
        for k in 0..20 {
            let name = format!("{}_{}_{}", adjectives[next(8) as usize], nouns[next(8) as usize], 1 + next(20_000));
            let ms = 100 + next(800);
            out += &format!(
                "{}{{\"id\":{},\"body\":\"{name} says hi at 2026-11-13T21:20:{:02}.{ms}Z (VU {}, iter {})\",\"created_at\":\"2026-11-13T21:20:{:02}.{}Z\",\"author\":\"{name}\",\"like_count\":{}}}",
                if k == 0 { "" } else { "," }, 700_000 - k, 59 - k / 12, 1 + next(1_480_000), next(14), 59 - k / 12, ms + 4, next(3) / 2
            );
        }
        out += "]}";
        out.into_bytes()
    }

    #[test]
    #[ignore = "a timing, not a check: cargo test --release -- --ignored --nocapture timing"]
    fn timing() {
        // 3,000 different feeds in turn, as in the server. One feed over and over lets the processor learn its
        // branches by heart, and the encoder then looks almost twice as fast as it is.
        let mut feeds: Vec<Vec<u8>> = (0..3000u32).map(|k| live_feed_from(1 + k * 7919)).collect();
        if let Ok(path) = std::env::var("FEED_SAMPLE") {
            feeds.push(std::fs::read(path).unwrap());
        }
        let mut g = Gzip::new();
        let mut out = Vec::new();
        let (rounds, mut bytes) = (40, 0);
        let t = std::time::Instant::now();
        for _ in 0..rounds {
            for f in &feeds {
                out.clear();
                g.compress(std::hint::black_box(f), &mut out);
                bytes += out.len();
            }
        }
        let calls = rounds * feeds.len();
        println!("feeds of {} bytes: {} bytes and {:.0} ns each", feeds[0].len(), bytes / calls, t.elapsed().as_nanos() as f64 / calls as f64);
        for f in feeds.iter().rev().take(50) {
            out.clear();
            g.compress(f, &mut out);
            assert_eq!(&gunzip(&out), f);
        }
        let feed = live_feed();
        let t = std::time::Instant::now();
        let mut sum = 0u32;
        for _ in 0..200_000 {
            sum = sum.wrapping_add(crc32(std::hint::black_box(&feed)));
        }
        println!("  of it the checksum: {:.0} ns ({sum:x})", t.elapsed().as_nanos() as f64 / 200_000.0);
    }

    #[test]
    fn feed_sized_body_shrinks() {
        let golden = concat!(env!("CARGO_MANIFEST_DIR"), "/../../test/golden/feed.json");
        let feed = std::fs::read(golden).expect("test/golden/feed.json");
        let mut g = Gzip::new();
        let first = round_trip(&mut g, &feed);
        // the seed's posts are plain prose, which this encoder shrinks least: about half
        assert!(first < feed.len() * 11 / 20, "{} -> {first}", feed.len());
        // a second call must not match against the first call's positions
        assert_eq!(round_trip(&mut g, &feed), first);
    }
}
