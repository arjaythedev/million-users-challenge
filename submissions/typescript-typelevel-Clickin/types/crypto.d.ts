import type { Compress, Blk, StateBytes, BITS, B64, HIPAD, HOPAD } from "./sha";

// Fold the 64-char blocks of an already-padded message into the running state.
export type Fold<H extends number[][], S extends string> =
  S extends "" ? H
  : Blk<S> extends [infer B extends number[][], infer R extends string] ? Fold<Compress<H, B>, R> : H;

// HMAC-SHA256 with the fixed JWT secret. The first block of each hash (key^ipad,
// key^opad) is precomputed in HIPAD/HOPAD, so only the message is folded here.
// InnerTail is the signing input already padded by the harness to whole blocks
// with the total length field (64 + len(input)) baked in.
export type HmacInner<InnerTail extends string> = StateBytes<Fold<HIPAD, InnerTail>>;
export type HmacB64<InnerTail extends string> =
  B64UrlEncode<StateBytes<Compress<HOPAD, OuterBlock<HmacInner<InnerTail>>>>>;
// The outer hash digests 64 (key block) + 32 (inner digest) bytes = fixed padding.
type OuterBlock<Digest extends string> =
  Blk<`${Digest}\u0080\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0003\u0000`> extends [infer B extends number[][], infer _] ? B : never;

// ---- base64url ----
type ToBits<S extends string> = S extends `${infer C}${infer R}` ? `${BITS[C & keyof BITS]}${ToBits<R>}` : "";
// 32 bytes -> 256 bits; pad to 258 (= 43*6) and map each 6-bit group.
type B64UrlEncode<Bytes extends string> = Enc6<`${ToBits<Bytes>}00`>;
type Enc6<Bits extends string> =
  Bits extends `${infer A}${infer B}${infer C}${infer D}${infer E}${infer F}${infer R}`
    ? `${B64[`${A}${B}${C}${D}${E}${F}` & keyof B64]}${Enc6<R>}` : "";

