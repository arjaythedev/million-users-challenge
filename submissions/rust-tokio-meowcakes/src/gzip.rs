const HASH_BITS: u32 = 13;
const MIN_MATCH: usize = 4;
const MAX_MATCH: usize = 258;
const WINDOW: u32 = 32768;
const TABLE: usize = 1 << HASH_BITS;

pub struct Gzip {
    table: Box<[u32; TABLE]>,
    base: u32,
    lit: [(u32, u32); 288],
    len: [(u32, u32); MAX_MATCH + 1],
}

impl Gzip {
    pub fn new() -> Self {
        let mut lit = [(0, 0); 288];
        for (s, e) in lit.iter_mut().enumerate() {
            let s = s as u32;
            let (code, n) = match s {
                0..=143 => (0x30 + s, 8),
                144..=255 => (0x190 + s - 144, 9),
                256..=279 => (s - 256, 7),
                _ => (0xC0 + s - 280, 8),
            };
            *e = (reverse(code, n), n);
        }
        const BASE: [u32; 29] = [
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
            115, 131, 163, 195, 227, 258,
        ];
        const EXTRA: [u32; 29] = [
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
        ];
        let mut len = [(0, 0); MAX_MATCH + 1];
        for (l, e) in len.iter_mut().enumerate().skip(3) {
            let i = if l == MAX_MATCH {
                28
            } else {
                (0..28).rev().find(|&i| BASE[i] as usize <= l).unwrap()
            };
            let (code, n) = lit[257 + i];
            *e = (code | (l as u32 - BASE[i]) << n, n + EXTRA[i]);
        }
        Gzip {
            table: vec![0; TABLE].into_boxed_slice().try_into().unwrap(),
            base: 1,
            lit,
            len,
        }
    }

    pub fn compress(&mut self, input: &[u8], out: &mut Vec<u8>) {
        let n = input.len();
        if self.base as u64 + n as u64 + 1 >= u32::MAX as u64 {
            self.table.fill(0);
            self.base = 1;
        }
        out.clear();
        out.reserve(n + n / 8 + 40);
        out.extend_from_slice(&[0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3]);
        let mut w = BitWriter {
            ptr: out.as_mut_ptr(),
            pos: out.len(),
            bits: 0b011,
            n: 3,
        };
        let src = input.as_ptr();

        let mut i = 0;
        while i + MIN_MATCH <= n {
            let v = unsafe { read32(src, i) };
            let h = (v.wrapping_mul(0x9E37_79B1) >> (32 - HASH_BITS)) as usize;
            let pos = self.base + i as u32;
            let cand = std::mem::replace(&mut self.table[h], pos);
            if cand >= self.base && pos - cand <= WINDOW {
                let c = (cand - self.base) as usize;
                if unsafe { read32(src, c) } == v {
                    let max = (n - i).min(MAX_MATCH) - MIN_MATCH;
                    let l =
                        MIN_MATCH + unsafe { match_len(src, c + MIN_MATCH, i + MIN_MATCH, max) };
                    let (lb, ln) = self.len[l];
                    let (db, dn) = distance((i - c) as u32);
                    w.put(lb | db << ln, ln + dn);
                    i += l;
                    continue;
                }
            }
            let (bits, nb) = self.lit[unsafe { *src.add(i) } as usize];
            w.put(bits, nb);
            i += 1;
        }
        for &b in &input[i..] {
            let (bits, nb) = self.lit[b as usize];
            w.put(bits, nb);
        }
        let (bits, nb) = self.lit[256];
        w.put(bits, nb);
        let end = w.pos + (w.n as usize).div_ceil(8);
        unsafe { out.set_len(end) };

        out.extend_from_slice(&crc32fast::hash(input).to_le_bytes());
        out.extend_from_slice(&(n as u32).to_le_bytes());
        self.base += n as u32 + 1;
    }
}

struct BitWriter {
    ptr: *mut u8,
    pos: usize,
    bits: u64,
    n: u32,
}

impl BitWriter {
    #[inline(always)]
    fn put(&mut self, bits: u32, n: u32) {
        self.bits |= (bits as u64) << self.n;
        self.n += n;
        unsafe { (self.ptr.add(self.pos) as *mut u64).write_unaligned(self.bits.to_le()) };
        let bytes = self.n >> 3;
        self.pos += bytes as usize;
        self.bits >>= bytes * 8;
        self.n &= 7;
    }
}

#[inline(always)]
unsafe fn read32(src: *const u8, i: usize) -> u32 {
    u32::from_le(unsafe { (src.add(i) as *const u32).read_unaligned() })
}

#[inline(always)]
unsafe fn match_len(src: *const u8, a: usize, b: usize, max: usize) -> usize {
    let mut l = 0;
    while l + 8 <= max {
        let x = unsafe {
            (src.add(a + l) as *const u64).read_unaligned()
                ^ (src.add(b + l) as *const u64).read_unaligned()
        };
        if x != 0 {
            return l + (u64::from_le(x).trailing_zeros() / 8) as usize;
        }
        l += 8;
    }
    while l < max && unsafe { *src.add(a + l) == *src.add(b + l) } {
        l += 1;
    }
    l
}

#[inline(always)]
fn distance(d: u32) -> (u32, u32) {
    if d <= 4 {
        return (reverse(d - 1, 5), 5);
    }
    let x = d - 1;
    let b = 31 - x.leading_zeros();
    let extra = b - 1;
    let code = 2 * b + (x >> extra & 1);
    (reverse(code, 5) | (x & ((1 << extra) - 1)) << 5, 5 + extra)
}

const fn reverse(code: u32, n: u32) -> u32 {
    code.reverse_bits() >> (32 - n)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip(g: &mut Gzip, input: &[u8]) {
        let mut out = Vec::new();
        g.compress(input, &mut out);
        let mut back = vec![0; input.len() + 1];
        let n = libdeflater::Decompressor::new()
            .gzip_decompress(&out, &mut back)
            .expect("valid gzip");
        assert_eq!(&back[..n], input, "round trip of {} bytes", input.len());
    }

    fn rng(seed: &mut u64) -> u64 {
        *seed ^= *seed << 13;
        *seed ^= *seed >> 7;
        *seed ^= *seed << 17;
        *seed
    }

    #[test]
    fn round_trips() {
        let mut g = Gzip::new();
        let feed = std::fs::read(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../test/golden/feed.json"
        ))
        .unwrap();
        roundtrip(&mut g, &feed);
        roundtrip(&mut g, &feed);
        roundtrip(&mut g, b"");
        roundtrip(&mut g, b"abc");
        roundtrip(&mut g, &[b'a'; 100_000]);
        let mut seed = 0x2545_F491_4F6C_DD1D;
        for size in [1, 7, 100, 1000, 4096, 40_000, 70_000, 300_000] {
            let random: Vec<u8> = (0..size).map(|_| rng(&mut seed) as u8).collect();
            roundtrip(&mut g, &random);
            let words: [&[u8]; 6] = [
                b"feed ",
                b"post ",
                b"\"like_count\":",
                b"0123",
                b"\xc3\xa9 ",
                b"\n",
            ];
            let mut text = Vec::new();
            while text.len() < size {
                text.extend_from_slice(words[(rng(&mut seed) % 6) as usize]);
                if rng(&mut seed).is_multiple_of(4) {
                    text.push(rng(&mut seed) as u8);
                }
            }
            roundtrip(&mut g, &text);
        }
    }

    #[test]
    fn base_wraps() {
        let mut g = Gzip::new();
        let feed = std::fs::read(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../test/golden/feed.json"
        ))
        .unwrap();
        roundtrip(&mut g, &feed);
        g.base = u32::MAX - 3000;
        roundtrip(&mut g, &feed);
        assert!(g.base < 10_000);
        roundtrip(&mut g, &feed);
    }

    #[test]
    fn distances() {
        let mut base = vec![1u32, 2, 3, 4];
        let mut extra = vec![0u32; 4];
        for code in 4..30u32 {
            let e = code / 2 - 1;
            base.push(base[code as usize - 1] + (1 << extra[code as usize - 1]));
            extra.push(e);
        }
        for d in 1..=32768u32 {
            let code = (0..30).rev().find(|&c| base[c] <= d).unwrap();
            let want = (
                reverse(code as u32, 5) | (d - base[code]) << 5,
                5 + extra[code],
            );
            assert_eq!(distance(d), want, "distance {d}");
        }
    }
}
