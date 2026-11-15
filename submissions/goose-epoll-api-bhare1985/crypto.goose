// SHA-256, HMAC-SHA256 and base64url, ported from server.c. No bounds check is left in any of these.
namespace crypto;
import std;

let K: u32[64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2];

let IV: u32[8] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];

// A running hash: the chaining value, the bytes hashed so far, and the unfinished block.
struct Sha { h: u32[8], n: u64, b: u8[64], bl: u32 }

checkfree inline fn ror(x: u32, n: u32) -> u32 { (x >> n) | (x << (32 - n)) }

checkfree fn compress(h: u32[8]&, p: u8[:]) {
    assert(p.len >= 64);
    var w: u32[64];
    for i in 16 {
        w[i] = (p[4 * i] as u32) << 24 | (p[4 * i + 1] as u32) << 16 | (p[4 * i + 2] as u32) << 8 | p[4 * i + 3];
    }
    for i in 16..64 {
        let a = w[i - 15];
        let b = w[i - 2];
        let s0 = ror(a, 7) ^ ror(a, 18) ^ (a >> 3);
        let s1 = ror(b, 17) ^ ror(b, 19) ^ (b >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    var a = h[0]; var b = h[1]; var c = h[2]; var d = h[3];
    var e = h[4]; var f = h[5]; var g = h[6]; var k = h[7];
    for i in 64 {
        let t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        let t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

fn init() -> Sha { Sha { h: IV, n: 0, b: [0; 64], bl: 0 } }

fn update(s: Sha&, p: u8[:]) {
    var q = p;
    s.n += q.len as u64;
    if s.bl > 0 {
        let have = s.bl as i64;
        let t = min(64 - have, q.len);
        copy_into(s.b[have..(have + t)], q[..t]);
        s.bl += t as u32;
        q = q[t..];
        if s.bl == 64 { compress(s.h, s.b[..]); s.bl = 0; }
    }
    while q.len >= 64 { compress(s.h, q); q = q[64..]; }
    if q.len > 0 {
        copy_into(s.b[..q.len], q);
        s.bl = q.len as u32;
    }
}

fn finish(s: Sha&, out: u8[32]&) {
    let bits = s.n * 8;
    var bl = s.bl as i64;
    s.b[bl] = 0x80;
    bl++;
    if bl > 56 {   // the length does not fit: finish this block and use another
        for i in bl..64 { s.b[i] = 0; }
        compress(s.h, s.b[..]);
        bl = 0;
    }
    for i in bl..56 { s.b[i] = 0; }
    for i in 8 { s.b[56 + i] = (bits >> (56 - 8 * i as u64)) as! u8; }
    compress(s.h, s.b[..]);
    for i in 8 {
        out[4 * i] = (s.h[i] >> 24) as! u8; out[4 * i + 1] = (s.h[i] >> 16) as! u8;
        out[4 * i + 2] = (s.h[i] >> 8) as! u8; out[4 * i + 3] = s.h[i] as! u8;
    }
}

// HMAC key schedule: hash state after absorbing key^ipad / key^opad, computed once for the secret.
struct Hmac { inner: Sha, outer: Sha }

fn hmac_key(key: u8[:]) -> Hmac {
    var k: u8[64] = [0; 64];
    if key.len > 64 {
        var s = init();
        update(s, key);
        var d: u8[32] = [0; 32];
        finish(s, d);
        for i in 32 { k[i] = d[i]; }
    } else {
        for i in key.len { k[i] = key[i]; }
    }
    var padv: u8[64];
    var r = Hmac { inner: init(), outer: init() };
    for i in 64 { padv[i] = k[i] ^ 0x36; }
    update(r.inner, padv[..]);
    for i in 64 { padv[i] = k[i] ^ 0x5c; }
    update(r.outer, padv[..]);
    return r;
}

fn hmac(key: Hmac&, m: u8[:], out: u8[32]&) {
    var s = key.inner;
    var ih: u8[32] = [0; 32];
    update(s, m);
    finish(s, ih);
    s = key.outer;
    update(s, ih[..]);
    finish(s, out);
}

// base64url without padding, as JWT uses it. -1 marks a bad character.
let B64: i8[256] = [-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1];

// Appends the decoded bytes to out; false on bad input (out is then unspecified).
fn b64url_decode(s: u8[:], out: u8[>..]&) -> bool {
    if s.len % 4 == 1 { return false; }
    var acc: u32 = 0;
    var bits: u32 = 0;
    for c in s {
        let v = B64[c];
        if v < 0 { return false; }
        acc = acc << 6 | v as u32;
        bits += 6;
        if bits >= 8 { bits -= 8; out.push((acc >> bits) as! u8); }
    }
    return true;
}
