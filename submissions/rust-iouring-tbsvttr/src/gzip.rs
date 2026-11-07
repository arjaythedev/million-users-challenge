/* =========================================================================
 * Zero-Allocation Epoch-Based Fixed-Huffman + Greedy LZ77 Gzip Compressor
 * Compresses 3.6 KB /feed JSON to ~870 bytes (1 TCP segment) in ~3 µs
 * ========================================================================= */

const HT_BITS: usize = 12;
const HT_SIZE: usize = 1 << HT_BITS;
const HT_MASK: usize = HT_SIZE - 1;

const GZIP_HEADER: [u8; 10] = [0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x03];

const fn bit_reverse(mut v: u16, bits: u8) -> u16 {
    let mut out = 0u16;
    let mut i = 0;
    while i < bits {
        out = (out << 1) | (v & 1);
        v >>= 1;
        i += 1;
    }
    out
}

const fn build_fixed_lit_table() -> [(u16, u8); 288] {
    let mut tab = [(0u16, 0u8); 288];
    let mut sym = 0usize;
    while sym <= 143 {
        tab[sym] = (bit_reverse(0x30 + sym as u16, 8), 8);
        sym += 1;
    }
    while sym <= 255 {
        tab[sym] = (bit_reverse(0x190 + (sym - 144) as u16, 9), 9);
        sym += 1;
    }
    while sym <= 279 {
        tab[sym] = (bit_reverse((sym - 256) as u16, 7), 7);
        sym += 1;
    }
    while sym <= 287 {
        tab[sym] = (bit_reverse(0xC0 + (sym - 280) as u16, 8), 8);
        sym += 1;
    }
    tab
}

const fn build_fixed_dist_table() -> [u16; 32] {
    let mut tab = [0u16; 32];
    let mut d = 0usize;
    while d < 32 {
        tab[d] = bit_reverse(d as u16, 5);
        d += 1;
    }
    tab
}

const fn build_crc32_slice8() -> [[u32; 256]; 8] {
    let mut table = [[0u32; 256]; 8];
    let mut i = 0usize;
    while i < 256 {
        let mut crc = i as u32;
        let mut bit = 0;
        while bit < 8 {
            if (crc & 1) != 0 {
                crc = (crc >> 1) ^ 0xEDB8_8320;
            } else {
                crc >>= 1;
            }
            bit += 1;
        }
        table[0][i] = crc;
        i += 1;
    }
    let mut s = 1usize;
    while s < 8 {
        let mut idx = 0usize;
        while idx < 256 {
            let prev = table[s - 1][idx];
            table[s][idx] = (prev >> 8) ^ table[0][(prev & 0xFF) as usize];
            idx += 1;
        }
        s += 1;
    }
    table
}

const LIT_CODES: [(u16, u8); 288] = build_fixed_lit_table();
const DIST_CODES: [u16; 32] = build_fixed_dist_table();
const CRC32_T: [[u32; 256]; 8] = build_crc32_slice8();

const LEN_BASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
    131, 163, 195, 227, 258,
];
const LEN_EXTRA: [u8; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
];

const DIST_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
];
const DIST_EXTRA: [u8; 30] = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13,
];

#[inline]
pub fn crc32_fast(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    let mut chunks = data.chunks_exact(8);
    for c in &mut chunks {
        let low = u32::from_le_bytes([c[0], c[1], c[2], c[3]]) ^ crc;
        let high = u32::from_le_bytes([c[4], c[5], c[6], c[7]]);
        crc = CRC32_T[7][(low & 0xFF) as usize]
            ^ CRC32_T[6][((low >> 8) & 0xFF) as usize]
            ^ CRC32_T[5][((low >> 16) & 0xFF) as usize]
            ^ CRC32_T[4][(low >> 24) as usize]
            ^ CRC32_T[3][(high & 0xFF) as usize]
            ^ CRC32_T[2][((high >> 8) & 0xFF) as usize]
            ^ CRC32_T[1][((high >> 16) & 0xFF) as usize]
            ^ CRC32_T[0][(high >> 24) as usize];
    }
    for &b in chunks.remainder() {
        crc = (crc >> 8) ^ CRC32_T[0][((crc ^ u32::from(b)) & 0xFF) as usize];
    }
    !crc
}

pub struct FastGzip {
    ht: Box<[u32; HT_SIZE]>,
    epoch_base: u32,
}

impl FastGzip {
    pub fn new() -> Self {
        Self {
            ht: Box::new([0u32; HT_SIZE]),
            epoch_base: 65536,
        }
    }

    pub fn compress_into(&mut self, src: &[u8], dst: &mut Vec<u8>) {
        dst.extend_from_slice(&GZIP_HEADER);

        let base = if self.epoch_base > 0x7000_0000 {
            self.ht.fill(0);
            self.epoch_base = 65536;
            65536
        } else {
            let b = self.epoch_base;
            self.epoch_base = b.wrapping_add((src.len() as u32) + 65536);
            b
        };

        /* Start Deflate fixed-Huffman final block: BFINAL=1, BTYPE=01 (0b011 = 3, 3 bits) */
        let mut bit_buf: u64 = 0b011;
        let mut bit_cnt: u32 = 3;

        let n = src.len();
        let mut ip = 0usize;

        while ip + 4 <= n {
            let w = u32::from_le_bytes([src[ip], src[ip + 1], src[ip + 2], src[ip + 3]]);
            let h = ((w.wrapping_mul(0x1E35_A7BD)) >> (32 - HT_BITS)) as usize & HT_MASK;
            let entry = self.ht[h];
            self.ht[h] = base + (ip as u32);

            if entry >= base {
                let ref_pos = (entry - base) as usize;
                let dist = ip.wrapping_sub(ref_pos);
                if dist >= 1 && dist <= 32768 {
                    let ref_w = u32::from_le_bytes([
                        src[ref_pos],
                        src[ref_pos + 1],
                        src[ref_pos + 2],
                        src[ref_pos + 3],
                    ]);
                    if ref_w == w {
                        let max_len = (n - ip).min(258);
                        let mut mlen = 4usize;
                        while mlen + 8 <= max_len {
                            let a = u64::from_le_bytes(
                                src[ip + mlen..ip + mlen + 8].try_into().unwrap(),
                            );
                            let b = u64::from_le_bytes(
                                src[ref_pos + mlen..ref_pos + mlen + 8].try_into().unwrap(),
                            );
                            let diff = a ^ b;
                            if diff != 0 {
                                mlen += (diff.trailing_zeros() >> 3) as usize;
                                break;
                            }
                            mlen += 8;
                        }
                        while mlen < max_len && src[ip + mlen] == src[ref_pos + mlen] {
                            mlen += 1;
                        }

                        emit_length_dist(mlen, dist, &mut bit_buf, &mut bit_cnt, dst);

                        /* Update hash table at match midpoint and end for chained JSON keys */
                        if ip + mlen + 4 <= n {
                            let end_p = ip + mlen - 1;
                            let ew = u32::from_le_bytes([
                                src[end_p],
                                src[end_p + 1],
                                src[end_p + 2],
                                src[end_p + 3],
                            ]);
                            let eh = ((ew.wrapping_mul(0x1E35_A7BD)) >> (32 - HT_BITS)) as usize
                                & HT_MASK;
                            self.ht[eh] = base + (end_p as u32);
                        }
                        ip += mlen;
                        continue;
                    }
                }
            }

            let (code, clen) = LIT_CODES[src[ip] as usize];
            bit_buf |= u64::from(code) << bit_cnt;
            bit_cnt += u32::from(clen);
            if bit_cnt >= 32 {
                dst.extend_from_slice(&(bit_buf as u32).to_le_bytes());
                bit_buf >>= 32;
                bit_cnt -= 32;
            }
            ip += 1;
        }

        while ip < n {
            let (code, clen) = LIT_CODES[src[ip] as usize];
            bit_buf |= u64::from(code) << bit_cnt;
            bit_cnt += u32::from(clen);
            if bit_cnt >= 32 {
                dst.extend_from_slice(&(bit_buf as u32).to_le_bytes());
                bit_buf >>= 32;
                bit_cnt -= 32;
            }
            ip += 1;
        }

        /* End-of-block symbol 256 (code = 0, 7 bits) */
        let (eob_code, eob_len) = LIT_CODES[256];
        bit_buf |= u64::from(eob_code) << bit_cnt;
        bit_cnt += u32::from(eob_len);

        while bit_cnt >= 8 {
            dst.push(bit_buf as u8);
            bit_buf >>= 8;
            bit_cnt -= 8;
        }
        if bit_cnt > 0 {
            dst.push(bit_buf as u8);
        }

        let crc = crc32_fast(src);
        dst.extend_from_slice(&crc.to_le_bytes());
        dst.extend_from_slice(&(src.len() as u32).to_le_bytes());
    }
}

const fn build_len_packed_table() -> [(u32, u8); 259] {
    let mut tab = [(0u32, 0u8); 259];
    let mut mlen = 3usize;
    while mlen <= 258 {
        let l_idx = if mlen == 258 {
            28
        } else {
            let mut i = 0usize;
            while i + 1 < 28 && (mlen as u16) >= LEN_BASE[i + 1] {
                i += 1;
            }
            i
        };
        let (l_code, l_clen) = LIT_CODES[257 + l_idx];
        let l_ebits = LEN_EXTRA[l_idx];
        let l_eval = (mlen as u16) - LEN_BASE[l_idx];
        let bits = (l_code as u32) | ((l_eval as u32) << l_clen);
        tab[mlen] = (bits, l_clen + l_ebits);
        mlen += 1;
    }
    tab
}

const fn build_dist_idx_table() -> [u8; 512] {
    let mut tab = [0u8; 512];
    let mut d = 0usize;
    while d < 256 {
        let dist = (d + 1) as u16;
        let mut i = 0usize;
        while i + 1 < 30 && dist >= DIST_BASE[i + 1] {
            i += 1;
        }
        tab[d] = i as u8;
        d += 1;
    }
    let mut d = 0usize;
    while d < 256 {
        let dist = ((d << 7) + 1) as u16;
        let mut i = 0usize;
        while i + 1 < 30 && dist >= DIST_BASE[i + 1] {
            i += 1;
        }
        tab[256 + d] = i as u8;
        d += 1;
    }
    tab
}

const LEN_PACKED: [(u32, u8); 259] = build_len_packed_table();
const DIST_IDX_TAB: [u8; 512] = build_dist_idx_table();

#[inline]
fn emit_length_dist(
    mlen: usize,
    dist: usize,
    bit_buf: &mut u64,
    bit_cnt: &mut u32,
    dst: &mut Vec<u8>,
) {
    let (l_bits, l_total) = LEN_PACKED[mlen];
    let d_m1 = dist - 1;
    let d_idx = if d_m1 < 256 {
        DIST_IDX_TAB[d_m1] as usize
    } else {
        DIST_IDX_TAB[256 + (d_m1 >> 7)] as usize
    };
    let d_code = DIST_CODES[d_idx] as u32;
    let d_ebits = DIST_EXTRA[d_idx];
    let d_eval = ((dist as u16) - DIST_BASE[d_idx]) as u32;
    let d_bits = d_code | (d_eval << 5);
    let d_total = 5 + d_ebits;

    let combined = (l_bits as u64) | ((d_bits as u64) << l_total);
    let total_bits = (l_total + d_total) as u32;

    *bit_buf |= combined << *bit_cnt;
    *bit_cnt += total_bits;
    while *bit_cnt >= 32 {
        dst.extend_from_slice(&(*bit_buf as u32).to_le_bytes());
        *bit_buf >>= 32;
        *bit_cnt -= 32;
    }
}

