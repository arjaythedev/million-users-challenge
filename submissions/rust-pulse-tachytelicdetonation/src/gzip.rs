use crate::crc32::crc32;
use std::cmp::Reverse;
use std::collections::BinaryHeap;

const LBASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
];
const LEXTRA: [u8; 29] = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DBASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577,
];
const DEXTRA: [u8; 30] = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const CL_ORDER: [usize; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

const HASH_BITS: u32 = 10;
const WINDOW: usize = 32768;

trait Sink {
    fn lits(&mut self, s: &[u8]);
    fn mat(&mut self, len: usize, dist: usize);
}

#[inline(always)]
fn rd32(s: &[u8], i: usize) -> u32 {
    unsafe { (s.as_ptr().add(i) as *const u32).read_unaligned() }
}

#[inline(always)]
fn rd64(s: &[u8], i: usize) -> u64 {
    unsafe { (s.as_ptr().add(i) as *const u64).read_unaligned() }
}

#[inline(always)]
fn hash(v: u32) -> usize {
    (v.wrapping_mul(0x9E37_79B1) >> (32 - HASH_BITS)) as usize
}

fn lz77<S: Sink>(table: &mut [u32; 1 << HASH_BITS], base: u32, inp: &[u8], sink: &mut S) {
    let len = inp.len();
    let mut lit_start = 0;
    if len >= 12 {
        let limit = len - 8;
        let mut i = 0;
        while i <= limit {
            let v = rd32(inp, i);
            let h = hash(v);
            let cand = table[h];
            table[h] = base + i as u32;
            if cand >= base {
                let c = (cand - base) as usize;
                if i - c <= WINDOW && rd32(inp, c) == v {
                    let max = (len - i).min(258);
                    let mut m = 4;
                    loop {
                        if m + 8 > max {
                            while m < max && inp[c + m] == inp[i + m] {
                                m += 1;
                            }
                            break;
                        }
                        let x = rd64(inp, c + m) ^ rd64(inp, i + m);
                        if x != 0 {
                            m += (x.trailing_zeros() >> 3) as usize;
                            break;
                        }
                        m += 8;
                    }
                    if lit_start < i {
                        sink.lits(&inp[lit_start..i]);
                    }
                    sink.mat(m, i - c);
                    let end = i + m;
                    let mut p = if m <= 16 { i + 1 } else { end - 3 };
                    while p < end && p <= limit {
                        table[hash(rd32(inp, p))] = base + p as u32;
                        p += 1;
                    }
                    i = end;
                    lit_start = i;
                    continue;
                }
            }
            i += 1 + ((i - lit_start) >> 7);
        }
    }
    if lit_start < len {
        sink.lits(&inp[lit_start..]);
    }
}

struct Counter {
    lit: [u64; 286],
    dist: [u64; 30],
    lsym: [u16; 259],
    dsym: [u8; 512],
}

#[inline(always)]
fn dist_sym(t: &[u8; 512], d: usize) -> usize {
    if d <= 256 {
        t[d - 1] as usize
    } else {
        t[256 + ((d - 1) >> 7)] as usize
    }
}

impl Sink for Counter {
    fn lits(&mut self, s: &[u8]) {
        for &b in s {
            self.lit[b as usize] += 1;
        }
    }
    fn mat(&mut self, len: usize, dist: usize) {
        self.lit[self.lsym[len] as usize] += 1;
        self.dist[dist_sym(&self.dsym, dist)] += 1;
    }
}

struct Enc<'a> {
    p: *mut u8,
    acc: u64,
    n: u32,
    t: &'a Codes,
}

impl<'a> Enc<'a> {
    #[inline(always)]
    fn put(&mut self, bits: u64, nb: u32) {
        self.acc |= bits << self.n;
        self.n += nb;
        if self.n >= 32 {
            unsafe {
                (self.p as *mut u32).write_unaligned((self.acc as u32).to_le());
                self.p = self.p.add(4);
            }
            self.acc >>= 32;
            self.n -= 32;
        }
    }
}

impl<'a> Sink for Enc<'a> {
    #[inline(always)]
    fn lits(&mut self, s: &[u8]) {
        let t = self.t;
        let mut it = s.chunks_exact(2);
        for pr in &mut it {
            let a = pr[0] as usize;
            let b = pr[1] as usize;
            let bits = t.lit[a] as u64 | ((t.lit[b] as u64) << t.litn[a]);
            self.put(bits, (t.litn[a] + t.litn[b]) as u32);
        }
        for &b in it.remainder() {
            self.put(t.lit[b as usize] as u64, t.litn[b as usize] as u32);
        }
    }
    #[inline(always)]
    fn mat(&mut self, len: usize, dist: usize) {
        let t = self.t;
        self.put(t.lcode[len] as u64, t.lbits[len] as u32);
        let s = dist_sym(&t.dsym, dist);
        let bits = t.dcode[s] as u64 | (((dist - DBASE[s] as usize) as u64) << t.dn[s]);
        self.put(bits, (t.dn[s] + DEXTRA[s]) as u32);
    }
}

struct Codes {
    lit: [u32; 286],
    litn: [u8; 286],
    lcode: [u32; 259],
    lbits: [u8; 259],
    dcode: [u32; 30],
    dn: [u8; 30],
    dsym: [u8; 512],
    hdr: Vec<u8>,
    hdr_bits: u64,
    hdr_n: u32,
}

fn lsym_table() -> [u16; 259] {
    let mut t = [0u16; 259];
    for k in 0..28 {
        let lo = LBASE[k] as usize;
        for l in lo..(lo + (1 << LEXTRA[k])).min(258) {
            t[l] = 257 + k as u16;
        }
    }
    t[258] = 285;
    t
}

fn dsym_table() -> [u8; 512] {
    let mut t = [0u8; 512];
    for k in 0..30 {
        let lo = DBASE[k] as usize;
        let hi = lo + (1 << DEXTRA[k]);
        for d in lo..hi {
            if d <= 256 {
                t[d - 1] = k as u8;
            } else {
                t[256 + ((d - 1) >> 7)] = k as u8;
            }
        }
    }
    t
}

fn canonical(lens: &[u8]) -> Vec<u32> {
    let mut count = [0u32; 16];
    for &l in lens {
        count[l as usize] += 1;
    }
    count[0] = 0;
    let mut next = [0u32; 16];
    let mut code = 0;
    for b in 1..16 {
        code = (code + count[b - 1]) << 1;
        next[b] = code;
    }
    lens.iter()
        .map(|&l| {
            if l == 0 {
                return 0;
            }
            let c = next[l as usize];
            next[l as usize] += 1;
            let mut r = 0;
            for k in 0..l {
                r |= ((c >> k) & 1) << (l - 1 - k);
            }
            r
        })
        .collect()
}

fn huff_lengths(freq: &[u64], limit: u8) -> Vec<u8> {
    let mut f: Vec<u64> = freq.to_vec();
    loop {
        let n = f.len();
        let mut lens = vec![0u8; n];
        let live: Vec<usize> = (0..n).filter(|&i| f[i] > 0).collect();
        if live.len() == 1 {
            lens[live[0]] = 1;
            return lens;
        }
        let mut parent = vec![usize::MAX; n + live.len()];
        let mut heap = BinaryHeap::new();
        for &i in &live {
            heap.push(Reverse((f[i], i)));
        }
        let mut next = n;
        while heap.len() > 1 {
            let Reverse((fa, a)) = heap.pop().unwrap();
            let Reverse((fb, b)) = heap.pop().unwrap();
            parent[a] = next;
            parent[b] = next;
            heap.push(Reverse((fa + fb, next)));
            next += 1;
        }
        let mut maxl = 0;
        for &i in &live {
            let mut d = 0u8;
            let mut x = i;
            while parent[x] != usize::MAX {
                x = parent[x];
                d += 1;
            }
            lens[i] = d;
            maxl = maxl.max(d);
        }
        if maxl <= limit {
            return lens;
        }
        for v in f.iter_mut() {
            if *v > 0 {
                *v = (*v >> 1).max(1);
            }
        }
    }
}

struct BitVec {
    out: Vec<u8>,
    acc: u64,
    n: u32,
}

impl BitVec {
    fn put(&mut self, bits: u64, nb: u32) {
        self.acc |= bits << self.n;
        self.n += nb;
        while self.n >= 8 {
            self.out.push(self.acc as u8);
            self.acc >>= 8;
            self.n -= 8;
        }
    }
}

impl Codes {
    fn build(litn: &[u8], dn: &[u8], dynamic: bool) -> Codes {
        let lit_c = canonical(litn);
        let d_c = canonical(dn);
        let mut c = Codes {
            lit: [0; 286],
            litn: [0; 286],
            lcode: [0; 259],
            lbits: [0; 259],
            dcode: [0; 30],
            dn: [0; 30],
            dsym: dsym_table(),
            hdr: Vec::new(),
            hdr_bits: 0,
            hdr_n: 0,
        };
        c.lit.copy_from_slice(&lit_c[..286]);
        c.litn.copy_from_slice(&litn[..286]);
        c.dcode.copy_from_slice(&d_c[..30]);
        c.dn.copy_from_slice(&dn[..30]);
        let ls = lsym_table();
        for l in 3..=258 {
            let s = ls[l] as usize;
            let k = s - 257;
            let extra = (l - LBASE[k] as usize) as u32;
            c.lcode[l] = c.lit[s] | (extra << c.litn[s]);
            c.lbits[l] = c.litn[s] + LEXTRA[k];
        }
        let mut bv = BitVec { out: Vec::new(), acc: 0, n: 0 };
        if !dynamic {
            bv.put(3, 3);
        } else {
            bv.put(5, 3);
            let mut seq: Vec<u8> = litn[..286].to_vec();
            seq.extend_from_slice(&dn[..30]);
            let mut syms: Vec<(u8, u8, u32)> = Vec::new();
            let mut i = 0;
            while i < seq.len() {
                let l = seq[i];
                let mut run = 1;
                while i + run < seq.len() && seq[i + run] == l {
                    run += 1;
                }
                if l == 0 {
                    let mut r = run;
                    while r >= 11 {
                        let k = r.min(138);
                        syms.push((18, 7, (k - 11) as u32));
                        r -= k;
                    }
                    if r >= 3 {
                        syms.push((17, 3, (r - 3) as u32));
                        r = 0;
                    }
                    for _ in 0..r {
                        syms.push((0, 0, 0));
                    }
                } else {
                    syms.push((l, 0, 0));
                    let mut r = run - 1;
                    while r >= 3 {
                        let k = r.min(6);
                        syms.push((16, 2, (k - 3) as u32));
                        r -= k;
                    }
                    for _ in 0..r {
                        syms.push((l, 0, 0));
                    }
                }
                i += run;
            }
            let mut clf = [0u64; 19];
            for s in &syms {
                clf[s.0 as usize] += 1;
            }
            let cll = huff_lengths(&clf, 7);
            let clc = canonical(&cll);
            let mut hclen = 19;
            while hclen > 4 && cll[CL_ORDER[hclen - 1]] == 0 {
                hclen -= 1;
            }
            bv.put(286 - 257, 5);
            bv.put(30 - 1, 5);
            bv.put((hclen - 4) as u64, 4);
            for k in 0..hclen {
                bv.put(cll[CL_ORDER[k]] as u64, 3);
            }
            for (s, eb, ev) in syms {
                bv.put(clc[s as usize] as u64, cll[s as usize] as u32);
                if eb > 0 {
                    bv.put(ev as u64, eb as u32);
                }
            }
        }
        c.hdr = bv.out;
        c.hdr_bits = bv.acc;
        c.hdr_n = bv.n;
        c
    }
}

const WORDS: &[&str] = &[
    "the", "a", "and", "of", "to", "in", "is", "it", "that", "for", "on", "with", "as", "was", "at", "by", "this",
    "be", "from", "or", "have", "not", "but", "are", "we", "you", "they", "my", "our", "just", "so", "when", "what",
    "about", "more", "time", "today", "really", "never", "always", "maybe", "still", "after", "before", "server",
    "request", "latency", "cache", "database", "query", "index", "pool", "connection", "deploy", "build", "test",
    "linux", "kernel", "network", "packet", "socket", "thread", "core", "memory", "disk", "load", "users", "traffic",
    "coffee", "morning", "night", "weekend", "project", "feature", "bug", "fix", "release", "learning", "writing",
    "reading", "running", "waiting", "watching", "building", "measuring", "shipping", "debugging", "fast", "slow",
    "big", "small", "cheap", "lucky", "weird", "boring", "careful", "broken", "simple", "surprising", "everything",
    "nothing", "someone", "everyone", "everywhere", "nowhere", "here", "there", "those", "these", "because", "while",
    "usually", "rarely", "tomorrow", "yesterday", "rewrite", "benchmark", "throughput", "graph", "metric", "error",
    "retry", "timeout", "queue", "worker", "process", "ubuntu", "droplet", "cloud", "cluster", "keepalive", "proxy",
];
const FIRST: &[&str] = &["TIL", "Hot take:", "Today", "Finally", "Note to self:", "Honestly", "So", "Why"];
const ADJ: &[&str] = &["happy", "swift", "golden", "quiet", "brave", "lazy", "clever", "bright", "calm", "wild"];
const NOUN: &[&str] = &["ember", "nebula", "otter", "falcon", "river", "comet", "pixel", "cedar", "harbor", "maple"];

fn training_corpus() -> Vec<Vec<u8>> {
    let mut x: u64 = 0x9E3779B97F4A7C15;
    let mut rnd = move |n: usize| {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        (x % n as u64) as usize
    };
    let mut docs = Vec::new();
    let mut id = 499_990usize;
    for _ in 0..48 {
        let mut s = String::from("{\"posts\":[");
        for p in 0..20 {
            if p > 0 {
                s.push(',');
            }
            let mut body = String::new();
            if rnd(3) == 0 {
                body.push_str(FIRST[rnd(FIRST.len())]);
                body.push(' ');
            }
            let sentences = 1 + rnd(3);
            for si in 0..sentences {
                let words = 5 + rnd(10);
                for w in 0..words {
                    let word = WORDS[rnd(WORDS.len())];
                    if w == 0 && (si > 0 || body.is_empty()) {
                        let mut cs = word.chars();
                        let f = cs.next().unwrap().to_ascii_uppercase();
                        body.push(f);
                        body.push_str(cs.as_str());
                    } else {
                        body.push_str(word);
                    }
                    if w + 1 < words {
                        body.push(if rnd(12) == 0 { ',' } else { ' ' });
                        if body.ends_with(',') {
                            body.push(' ');
                        }
                    }
                }
                body.push_str([".", "!", "?", "...", "."][rnd(5)]);
                if si + 1 < sentences {
                    body.push(' ');
                }
            }
            id -= 1 + rnd(3);
            s.push_str(&format!(
                "{{\"id\":{},\"body\":\"{}\",\"created_at\":\"2025-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z\",\"author\":\"{}_{}_{}\",\"like_count\":{}}}",
                id,
                body,
                1 + rnd(12),
                1 + rnd(28),
                rnd(24),
                rnd(60),
                rnd(60),
                rnd(1000),
                ADJ[rnd(ADJ.len())],
                NOUN[rnd(NOUN.len())],
                1 + rnd(50000),
                rnd(30)
            ));
        }
        s.push_str("]}");
        docs.push(s.into_bytes());
    }
    docs
}

pub struct Gz {
    table: Box<[u32; 1 << HASH_BITS]>,
    base: u32,
    codes: Codes,
}

impl Gz {
    pub fn new(fixed: bool) -> Gz {
        let codes = if fixed {
            let mut litn = [0u8; 288];
            for (i, l) in litn.iter_mut().enumerate() {
                *l = match i {
                    0..=143 => 8,
                    144..=255 => 9,
                    256..=279 => 7,
                    _ => 8,
                };
            }
            Codes::build(&litn, &[5u8; 30], false)
        } else {
            let mut cnt = Counter { lit: [0; 286], dist: [0; 30], lsym: lsym_table(), dsym: dsym_table() };
            let mut table = Box::new([0u32; 1 << HASH_BITS]);
            let mut base = 1u32;
            for doc in training_corpus() {
                lz77(&mut table, base, &doc, &mut cnt);
                base += doc.len() as u32 + WINDOW as u32 + 1;
            }
            cnt.lit[256] += 48;
            let lit: Vec<u64> = cnt.lit.iter().map(|&f| f * 4 + 1).collect();
            let dist: Vec<u64> = cnt.dist.iter().map(|&f| f * 4 + 1).collect();
            let litn = huff_lengths(&lit, 15);
            let dn = huff_lengths(&dist, 15);
            Codes::build(&litn, &dn, true)
        };
        Gz { table: Box::new([0; 1 << HASH_BITS]), base: 1, codes }
    }

    pub fn compress(&mut self, inp: &[u8], out: &mut Vec<u8>) {
        let need = inp.len() as u64 + WINDOW as u64 + 1;
        if self.base as u64 + need >= u32::MAX as u64 {
            self.table.fill(0);
            self.base = 1;
        }
        let c = &self.codes;
        out.reserve(inp.len() * 2 + c.hdr.len() + 64);
        let start = out.len();
        unsafe {
            let p0 = out.as_mut_ptr().add(start);
            let hdr: [u8; 10] = [0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3];
            std::ptr::copy_nonoverlapping(hdr.as_ptr(), p0, 10);
            std::ptr::copy_nonoverlapping(c.hdr.as_ptr(), p0.add(10), c.hdr.len());
            let mut e = Enc { p: p0.add(10 + c.hdr.len()), acc: c.hdr_bits, n: c.hdr_n, t: c };
            lz77(&mut self.table, self.base, inp, &mut e);
            e.put(c.lit[256] as u64, c.litn[256] as u32);
            let mut p = e.p;
            let mut acc = e.acc;
            let mut n = e.n as i32;
            while n > 0 {
                *p = acc as u8;
                p = p.add(1);
                acc >>= 8;
                n -= 8;
            }
            let crc = crc32(inp);
            std::ptr::copy_nonoverlapping(crc.to_le_bytes().as_ptr(), p, 4);
            std::ptr::copy_nonoverlapping((inp.len() as u32).to_le_bytes().as_ptr(), p.add(4), 4);
            p = p.add(8);
            out.set_len(p.offset_from(out.as_ptr()) as usize);
        }
        self.base += need as u32;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::process::{Command, Stdio};

    fn gunzip(data: &[u8]) -> Option<Vec<u8>> {
        let mut ch = Command::new("gzip").arg("-dc").stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap();
        let mut sin = ch.stdin.take().unwrap();
        let d = data.to_vec();
        let t = std::thread::spawn(move || {
            sin.write_all(&d).unwrap();
        });
        let out = ch.wait_with_output().unwrap();
        t.join().unwrap();
        out.status.success().then_some(out.stdout)
    }

    fn inputs() -> Vec<Vec<u8>> {
        let mut v: Vec<Vec<u8>> = vec![b"".to_vec(), b"a".to_vec(), b"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa".to_vec()];
        let mut x = 0xdeadbeefu64;
        let mut r = move || {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            x
        };
        let mixed = "é ✓ café json \"x\"".as_bytes();
        for n in [1usize, 2, 3, 7, 11, 12, 13, 64, 100, 257, 258, 259, 1000, 4096, 40000, 70000] {
            v.push((0..n).map(|_| r() as u8).collect());
            v.push((0..n).map(|_| b"ab"[(r() % 2) as usize]).collect());
            v.push((0..n).map(|_| mixed[(r() % mixed.len() as u64) as usize]).collect());
        }
        let mut long = String::new();
        for i in 0..20 {
            long.push_str(&format!("{{\"id\":{},\"body\":\"{}\"}}", i, "x".repeat(500)));
        }
        v.push(long.into_bytes());
        v.extend(training_corpus());
        if let Ok(g) = std::fs::read("../../test/golden/feed.json") {
            v.push(g);
        }
        v
    }

    #[test]
    fn roundtrip_gzip_cli() {
        for fixed in [false, true] {
            let mut gz = Gz::new(fixed);
            let mut all = Vec::new();
            for inp in inputs() {
                let mut out = Vec::new();
                gz.compress(&inp, &mut out);
                all.extend_from_slice(&out);
                assert_eq!(gunzip(&out).as_deref(), Some(&inp[..]), "fixed={} len={}", fixed, inp.len());
            }
            let mut joined = Vec::new();
            for inp in inputs() {
                joined.extend_from_slice(&inp);
            }
            assert_eq!(gunzip(&all), Some(joined));
        }
    }

    #[test]
    fn fuzz_many() {
        let mut gz = Gz::new(false);
        let mut x = 0x1234567u64;
        let mut r = move || {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            x
        };
        let alphabet = b"{}\":,abcdefghij 0123456789\\\n";
        let mut batch_in = Vec::new();
        let mut batch_out = Vec::new();
        for k in 0..10_000 {
            let n = (r() % 3000) as usize;
            let mode = r() % 3;
            let inp: Vec<u8> = (0..n)
                .map(|_| match mode {
                    0 => r() as u8,
                    1 => alphabet[(r() % alphabet.len() as u64) as usize],
                    _ => b"abc"[(r() % 3) as usize],
                })
                .collect();
            gz.compress(&inp, &mut batch_out);
            batch_in.extend_from_slice(&inp);
            if k % 1000 == 999 {
                assert_eq!(gunzip(&batch_out).as_deref(), Some(&batch_in[..]));
                batch_in.clear();
                batch_out.clear();
            }
        }
    }
}
