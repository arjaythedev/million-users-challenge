// HS256 bearer tokens, checked on every authenticated request: shape, alg, signature (HMAC-SHA256 written here
// over typed arrays, compared in constant time), exp (and nbf when present), then the claims. Nothing about a
// token is remembered; kept between requests are only the HMAC key pads' compressed states.
const K = new Int32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
  0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
  0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
  0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
  0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
  0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
  0xc67178f2,
]);
const IV = new Int32Array([
  0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
]);
const W = new Int32Array(64);

// One SHA-256 block (64 bytes of src at off) into state.
function compress(state: Int32Array, src: Uint8Array, off: number): void {
  for (let i = 0; i < 16; i++) {
    const j = off + i * 4;
    W[i] = (src[j]! << 24) | (src[j + 1]! << 16) | (src[j + 2]! << 8) | src[j + 3]!;
  }
  for (let i = 16; i < 64; i++) {
    const w15 = W[i - 15]!, w2 = W[i - 2]!;
    const s0 = ((w15 >>> 7) | (w15 << 25)) ^ ((w15 >>> 18) | (w15 << 14)) ^ (w15 >>> 3);
    const s1 = ((w2 >>> 17) | (w2 << 15)) ^ ((w2 >>> 19) | (w2 << 13)) ^ (w2 >>> 10);
    W[i] = (W[i - 16]! + s0 + W[i - 7]! + s1) | 0;
  }
  let a = state[0]!, b = state[1]!, c = state[2]!, d = state[3]!;
  let e = state[4]!, f = state[5]!, g = state[6]!, h = state[7]!;
  for (let i = 0; i < 64; i++) {
    const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
    const ch = (e & f) ^ (~e & g);
    const t1 = (h + S1 + ch + K[i]! + W[i]!) | 0;
    const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
    const maj = (a & b) ^ (a & c) ^ (b & c);
    const t2 = (S0 + maj) | 0;
    h = g;
    g = f;
    f = e;
    e = (d + t1) | 0;
    d = c;
    c = b;
    b = a;
    a = (t1 + t2) | 0;
  }
  state[0] = (state[0]! + a) | 0;
  state[1] = (state[1]! + b) | 0;
  state[2] = (state[2]! + c) | 0;
  state[3] = (state[3]! + d) | 0;
  state[4] = (state[4]! + e) | 0;
  state[5] = (state[5]! + f) | 0;
  state[6] = (state[6]! + g) | 0;
  state[7] = (state[7]! + h) | 0;
}

const tail = new Uint8Array(128);
const H = new Int32Array(8);

// Continues a hash whose state already covers `prefix` bytes, over src[start, end), and finishes it into out.
function finish(state: Int32Array, prefix: number, src: Uint8Array, start: number, end: number, out: Uint8Array): void {
  let p = start;
  while (end - p >= 64) {
    compress(state, src, p);
    p += 64;
  }
  const rest = end - p;
  for (let i = 0; i < rest; i++) tail[i] = src[p + i]!;
  tail[rest] = 0x80;
  const blocks = rest + 9 > 64 ? 2 : 1;
  const last = blocks * 64;
  tail.fill(0, rest + 1, last);
  const bits = (prefix + (end - start)) * 8; // well below 2^32 for any token we accept
  tail[last - 4] = (bits >>> 24) & 0xff;
  tail[last - 3] = (bits >>> 16) & 0xff;
  tail[last - 2] = (bits >>> 8) & 0xff;
  tail[last - 1] = bits & 0xff;
  compress(state, tail, 0);
  if (blocks === 2) compress(state, tail, 64);
  for (let i = 0; i < 8; i++) {
    const v = state[i]!;
    out[i * 4] = v >>> 24;
    out[i * 4 + 1] = (v >>> 16) & 0xff;
    out[i * 4 + 2] = (v >>> 8) & 0xff;
    out[i * 4 + 3] = v & 0xff;
  }
}

const innerPad = new Int32Array(8);
const outerPad = new Int32Array(8);

export function setKey(key: Uint8Array): void {
  let k = key;
  if (k.length > 64) {
    const d = new Uint8Array(32);
    H.set(IV);
    finish(H, 0, k, 0, k.length, d);
    k = d;
  }
  const block = new Uint8Array(64);
  for (let i = 0; i < 64; i++) block[i] = (k[i] ?? 0) ^ 0x36;
  innerPad.set(IV);
  compress(innerPad, block, 0);
  for (let i = 0; i < 64; i++) block[i] = (k[i] ?? 0) ^ 0x5c;
  outerPad.set(IV);
  compress(outerPad, block, 0);
}

const innerDigest = new Uint8Array(32);
const mac = new Uint8Array(32);

function hmac(src: Uint8Array, start: number, end: number): void {
  H.set(innerPad);
  finish(H, 64, src, start, end, innerDigest);
  H.set(outerPad);
  finish(H, 64, innerDigest, 0, 32, mac);
}

// base64url without padding (a trailing '=' run is tolerated) into out; returns the length, or -1.
const B64 = new Int8Array(256).fill(-1);
{
  const a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  for (let i = 0; i < 64; i++) B64[a.charCodeAt(i)] = i;
}
function b64urlDecode(src: Uint8Array, start: number, end: number, out: Uint8Array): number {
  while (end > start && src[end - 1] === 61) end--;
  const n = end - start;
  if (n % 4 === 1) return -1;
  const outLen = (n * 3) >> 2;
  if (outLen > out.length) return -1;
  let o = 0, acc = 0, bits = 0;
  for (let i = start; i < end; i++) {
    const v = B64[src[i]!]!;
    if (v < 0) return -1;
    acc = ((acc << 6) | v) & 0xffffff;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (acc >> bits) & 0xff;
    }
  }
  return o;
}

export const AUTH_OK = 0;
export const AUTH_MISSING = 1;
export const AUTH_INVALID = 2;
export const AUTH_BAD_PAYLOAD = 3;

// Outputs of a successful verify.
export let authUserId = 0;
export let authUsername: Uint8Array = new Uint8Array(0);

const sig = new Uint8Array(48);
const decoded = new Uint8Array(8192);
const dec = new TextDecoder();
const enc = new TextEncoder();
const BEARER = [66, 101, 97, 114, 101, 114, 32]; // "Bearer "

// The Authorization header's value is src[start, end), or start < 0 when the header is absent.
export function verify(src: Uint8Array, start: number, end: number): number {
  if (start < 0 || end - start < 7) return AUTH_MISSING;
  for (let i = 0; i < 7; i++) if (src[start + i] !== BEARER[i]) return AUTH_MISSING;
  let t0 = start + 7, t1 = end;
  while (t0 < t1 && src[t0] === 32) t0++;
  while (t1 > t0 && src[t1 - 1] === 32) t1--;
  if (t1 - t0 > 8192) return AUTH_INVALID;
  // exactly three non-empty segments: header.payload.signature
  let first = -1, second = -1;
  for (let i = t0; i < t1; i++) {
    if (src[i] !== 46) continue;
    if (first < 0) first = i;
    else if (second < 0) second = i;
    else return AUTH_INVALID;
  }
  if (first <= t0 || second <= first + 1 || second >= t1 - 1) return AUTH_INVALID;
  // header: a JSON object whose "alg" is "HS256"
  let n = b64urlDecode(src, t0, first, decoded);
  if (n < 0) return AUTH_INVALID;
  try {
    const header = JSON.parse(dec.decode(decoded.subarray(0, n)));
    if (header === null || typeof header !== "object" || header.alg !== "HS256") return AUTH_INVALID;
  } catch {
    return AUTH_INVALID;
  }
  // signature over "header.payload"
  if (b64urlDecode(src, second + 1, t1, sig) !== 32) return AUTH_INVALID;
  hmac(src, t0, second);
  let diff = 0;
  for (let i = 0; i < 32; i++) diff |= mac[i]! ^ sig[i]!;
  if (diff !== 0) return AUTH_INVALID;
  // claims
  n = b64urlDecode(src, first + 1, second, decoded);
  if (n < 0) return AUTH_INVALID;
  let claims: any;
  try {
    claims = JSON.parse(dec.decode(decoded.subarray(0, n)));
  } catch {
    return AUTH_INVALID;
  }
  if (claims === null || typeof claims !== "object" || Array.isArray(claims)) return AUTH_INVALID;
  const now = Date.now() / 1000;
  if (typeof claims.exp !== "number" || claims.exp <= now) return AUTH_INVALID;
  if (claims.nbf !== undefined && (typeof claims.nbf !== "number" || claims.nbf > now)) return AUTH_INVALID;
  const sub = claims.sub;
  if (typeof sub !== "string" || sub.length === 0 || sub.length > 19) return AUTH_BAD_PAYLOAD;
  let id = 0;
  for (let i = 0; i < sub.length; i++) {
    const c = sub.charCodeAt(i) - 48;
    if (c < 0 || c > 9) return AUTH_BAD_PAYLOAD;
    id = id * 10 + c;
  }
  if (id <= 0 || id > Number.MAX_SAFE_INTEGER) return AUTH_BAD_PAYLOAD;
  if (typeof claims.username !== "string") return AUTH_BAD_PAYLOAD;
  authUserId = id;
  authUsername = enc.encode(claims.username);
  return AUTH_OK;
}

export function authMessage(r: number): string {
  return r === AUTH_MISSING ? "missing bearer token" : r === AUTH_BAD_PAYLOAD ? "invalid token payload" : "invalid or expired token";
}

// Self-check against node:crypto at startup.
export function selfTest(key: Uint8Array): void {
  const { createHmac } = require("node:crypto");
  const msg = enc.encode("eyJhbGciOiJIUzI1NiJ9.".repeat(7) + "x");
  for (const len of [0, 1, 55, 56, 63, 64, 65, 119, 120, msg.length]) {
    hmac(msg, 0, len);
    const want: Buffer = createHmac("sha256", key).update(msg.subarray(0, len)).digest();
    for (let i = 0; i < 32; i++) if (want[i] !== mac[i]) throw new Error(`HMAC self-test failed at length ${len}`);
  }
}
