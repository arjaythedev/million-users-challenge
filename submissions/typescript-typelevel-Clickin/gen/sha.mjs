// Generates sha.d.ts: SHA-256 as TypeScript types. Words are 8-tuples of nibbles (MSB first).
import { writeFileSync } from "node:fs";

const R16 = [...Array(16).keys()];
const lines = [];
const out = (s) => lines.push(s);

const iface = (name, keys, f) =>
  out(`interface ${name} { ${keys.map((k) => `${k}: ${f(k)};`).join(" ")} }`);
const tup = (arr) => `[${arr.join(",")}]`;

iface("XOR", R16, (a) => tup(R16.map((b) => a ^ b)));
iface("CH", R16, (e) => tup(R16.map((f) => tup(R16.map((g) => ((e & f) ^ (~e & g)) & 15)))));
iface("MAJ", R16, (a) => tup(R16.map((b) => tup(R16.map((c) => (a & b) ^ (a & c) ^ (b & c))))));
for (const k of [1, 2, 3]) iface(`SH${k}`, R16, (a) => tup(R16.map((b) => ((a << (4 - k)) | (b >> k)) & 15)));
const PLUSN = 112;
iface("PLUS", [...Array(PLUSN).keys()], (a) => tup(R16.map((b) => a + b)));
iface("LO", [...Array(128).keys()], (v) => v & 15);
iface("HI", [...Array(128).keys()], (v) => v >> 4);

// latin1 char -> nibbles
const lit = (c) => JSON.stringify(c).replace(/[\u007f-ÿ]/g, (ch) => "\\u00" + ch.charCodeAt(0).toString(16).padStart(2, "0"));
const chars = [...Array(256).keys()].map((i) => String.fromCharCode(i));
out(`interface HN { ${chars.map((c, i) => `${lit(c)}: ${i >> 4};`).join(" ")} }`);
out(`interface LN { ${chars.map((c, i) => `${lit(c)}: ${i & 15};`).join(" ")} }`);
out(`interface HEXC { ${R16.map((i) => `${i}: "${i.toString(16)}";`).join(" ")} }`);

// rotr / shr expression of word expression W (string like "S[4]") at nibble i
const rot = (W, r, i, shr = false) => {
  const q = r >> 2, k = r & 3;
  const idx = (j) => {
    const raw = i - j; // source nibble index before wrapping
    if (raw < 0) return shr ? "0" : `${W}[${raw + 8}]`;
    return `${W}[${raw}]`;
  };
  if (k === 0) return idx(q);
  return `SH${k}[${idx(q + 1)}][${idx(q)}]`;
};
const xor3 = (a, b, c) => `XOR[XOR[${a}][${b}]][${c}]`;
const BS0 = (W, i) => xor3(rot(W, 2, i), rot(W, 13, i), rot(W, 22, i));
const BS1 = (W, i) => xor3(rot(W, 6, i), rot(W, 11, i), rot(W, 25, i));
const SS0 = (W, i) => xor3(rot(W, 7, i), rot(W, 18, i), rot(W, 3, i, true));
const SS1 = (W, i) => xor3(rot(W, 17, i), rot(W, 19, i), rot(W, 10, i, true));
const plus = (...xs) => xs.reduce((a, b) => `PLUS[${a}][${b}]`);
// normalize 8 totals (strings) into 8 nibble exprs, carrying from nibble 7 up
const norm = (tot) => [`Norm<${tup(tot)}>`];
// Norm<T>: carry-propagate 8 nibble totals (each <= 111)
{
  // N7 starts: V7 = T[7]
  out(`type Norm<T extends number[]> = N6<T, T[7]>;`);
  for (let i = 6; i >= 1; i--) {
    const vs = [];
    for (let j = 7; j > i; j--) vs.push(`V${j}`);
    out(`type N${i}<T extends number[], ${vs.map((v) => v + " extends number").join(", ")}> = N${i - 1}<T, ${vs.join(", ")}, PLUS[T[${i}]][HI[V${i + 1}]]>;`);
  }
  const vs = [7, 6, 5, 4, 3, 2, 1].map((j) => `V${j}`);
  out(`type N0<T extends number[], ${vs.map((v) => v + " extends number").join(", ")}> = [LO[PLUS[T[0]][HI[V1]]], ${[1, 2, 3, 4, 5, 6, 7].map((j) => `LO[V${j}]`).join(", ")}] & {};`);
}
const word = (f) => tup(R16.slice(0, 8).map(f));
const I8 = [0, 1, 2, 3, 4, 5, 6, 7];

// Round: S = [a..h], K, W words. T passed to RoundB.
out(`export type T1<S extends number[][], K extends number[], W extends number[]> = ${(
  norm(I8.map((i) => plus(`S[7][${i}]`, BS1("S[4]", i), `CH[S[4][${i}]][S[5][${i}]][S[6][${i}]]`, `K[${i}]`, `W[${i}]`)))
)};`);
out(`type RoundB<S extends number[][], T extends number[]> = RoundC<S, ${
  norm(I8.map((i) => plus(`T[${i}]`, BS0("S[0]", i), `MAJ[S[0][${i}]][S[1][${i}]][S[2][${i}]]`)))
}, ${(norm(I8.map((i) => plus(`S[3][${i}]`, `T[${i}]`))))}>;`);
out(`type RoundC<S extends number[][], A extends number[], E extends number[]> = [A, S[0], S[1], S[2], E, S[4], S[5], S[6]] & {};`);
out(`export type Round<S extends number[][], K extends number[], W extends number[]> = RoundB<S, T1<S, K, W>>;`);
out(`export type Next<W extends number[][]> = NextB<W, ${(
  norm(I8.map((i) => plus(SS1("W[14]", i), `W[9][${i}]`, SS0("W[1]", i), `W[0][${i}]`)))
)}>;`);
out(`type NextB<W extends number[][], X extends number[]> = [${[...Array(15).keys()].map((j) => `W[${j + 1}]`).join(", ")}, X] & {};`);
out(`type Shift<W extends number[][]> = [${[...Array(15).keys()].map((j) => `W[${j + 1}]`).join(", ")}, W[15]] & {};`);

const K = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
  0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
  0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
  0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];
const nib = (x) => tup(I8.map((i) => (x >>> (28 - 4 * i)) & 15));
iface("KT", [...Array(64).keys()], (t) => nib(K[t]));
const IV = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
out(`type IV = ${tup(IV.map(nib))};`);

const P8 = "[infer A, infer B, infer C, infer D, infer E, infer F, infer G, infer H]";
const P16 = "[" + [...Array(16).keys()].map((i) => `infer W${i}`).join(", ") + "]";
const L16 = "[" + [...Array(16).keys()].map((i) => `W${i}`).join(", ") + "]";
out(`export type Rounds<S extends number[][], W extends number[][], I extends 0[] = []> = I["length"] extends 64 ? S
  : Round<S, KT[I["length"]], W[0]> extends infer S2 extends number[][]
    ? (I["length"] extends 48|49|50|51|52|53|54|55|56|57|58|59|60|61|62|63 ? Shift<W> : Next<W>) extends infer W2 extends number[][]
      ? Rounds<S2, W2, [...I, 0]> : never : never;`);
out(`type AddState<A extends number[][], B extends number[][]> = AddS2<${
  I8.map((w) => norm(I8.map((i) => plus(`A[${w}][${i}]`, `B[${w}][${i}]`)))[0]).join(", ")
}>;`);
out(`type AddS2<${I8.map((w) => `X${w} extends number[]`).join(", ")}> = [${I8.map((w) => `X${w}`).join(", ")}] & {};`);
out(`export type Compress<H extends number[][], B extends number[][]> = Rounds<H, B> extends infer R extends number[][] ? AddState<H, R> : never;`);

// 64 chars -> 16 words (exported: used to fold message blocks)
const cs = [...Array(64).keys()].map((i) => `c${i}`);
out(`export type Blk<S extends string> = S extends \`${cs.map((c) => `\${infer ${c}}`).join("")}\${infer R}\` ? [${tup(
  [...Array(16).keys()].map((w) => tup([0, 1, 2, 3].flatMap((b) => [`HN[c${w * 4 + b}]`, `LN[c${w * 4 + b}]`])))
)}, R] : never;`);
out(`export type Hex<H extends number[][]> = \`${I8.flatMap((w) => I8.map((i) => `\${HEXC[H[${w}][${i}]]}`)).join("")}\`;`);

// ---- reverse tables: state nibbles -> bytes, bytes -> bits, base64url -------
// NB: [hi][lo] -> the latin1 character with code hi*16+lo
out(`export interface NB { ${R16.map((h) => `${h}: ${tup(R16.map((l) => lit(String.fromCharCode(h * 16 + l))))};`).join(" ")} }`);
// State (8 words x 8 nibbles) -> 32-byte latin1 string
out(`export type StateBytes<H extends number[][]> = \`${I8.flatMap((w) => [0, 2, 4, 6].map((i) => `\${NB[H[${w}][${i}]][H[${w}][${i + 1}]]}`)).join("")}\`;`);
// BITS: latin1 char -> 8-char bit string (MSB first)
out(`export interface BITS { ${chars.map((c, i) => `${lit(c)}: "${i.toString(2).padStart(8, "0")}";`).join(" ")} }`);
// base64url alphabet
const B64A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
out(`export interface B64 { ${[...Array(64).keys()].map((i) => `"${i.toString(2).padStart(6, "0")}": "${B64A[i]}";`).join(" ")} }`);

// ---- precomputed HMAC key states: the secret is fixed, so the first SHA block
// of the inner (key^ipad) and outer (key^opad) hashes is a constant. --------------
function jsSha256Block(state, block /* 64 bytes */) {
  const w = new Array(64);
  for (let i = 0; i < 16; i++) w[i] = (block[i * 4] << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | block[i * 4 + 3];
  const rotr = (x, n) => (x >>> n) | (x << (32 - n));
  for (let i = 16; i < 64; i++) {
    const s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >>> 3);
    const s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >>> 10);
    w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
  }
  let [a, b, c, d, e, f, g, h] = state;
  for (let i = 0; i < 64; i++) {
    const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const ch = (e & f) ^ (~e & g);
    const t1 = (h + S1 + ch + K[i] + w[i]) | 0;
    const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const maj = (a & b) ^ (a & c) ^ (b & c);
    const t2 = (S0 + maj) | 0;
    h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
  }
  return [a, b, c, d, e, f, g, h].map((x, i) => (x + state[i]) | 0);
}
const SECRET = "million-users-challenge";
function keyBlock(pad) {
  const kb = new Uint8Array(64); // secret < 64 bytes, zero-padded, then XOR pad
  for (let i = 0; i < SECRET.length; i++) kb[i] = SECRET.charCodeAt(i);
  for (let i = 0; i < 64; i++) kb[i] ^= pad;
  return kb;
}
const stateNib = (st) => tup(st.map((x) => tup(I8.map((i) => (x >>> (28 - 4 * i)) & 15))));
const Hipad = jsSha256Block(IV, keyBlock(0x36));
const Hopad = jsSha256Block(IV, keyBlock(0x5c));
out(`export type HIPAD = ${stateNib(Hipad)};`);
out(`export type HOPAD = ${stateNib(Hopad)};`);
out(`export type { IV };`);

writeFileSync(new URL("../generated/sha.d.ts", import.meta.url), lines.join("\n") + "\n");
console.log("bytes", lines.join("\n").length);
