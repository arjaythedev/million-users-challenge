import type { HmacB64 } from "./crypto";
import type { After, ReadStr, HasStr, ReadNum } from "./json";
import type { IsPosInt, DecLt, Equal } from "./util";

// Verify a Bearer JWT. The type checker does the security-relevant work: the HMAC
// over Tail (the SHA-padded signing input), the alg check, the signature compare,
// and claim validation. Base64url decoding is pure transport, so the harness hands
// the decoded header/payload JSON in (Hdr, Pl) -- like it hands in the padded Tail.
// Result: an ok record, or an error tag matching SPEC's messages:
//   "missing" -> missing bearer token, "invalid" -> invalid or expired token,
//   "payload" -> invalid token payload.
export type Auth<AuthHdr, Tail extends string, Now extends string, Hdr extends string, Pl extends string> =
  [AuthHdr] extends [never] ? { err: "missing" }
  : AuthHdr extends `Bearer ${infer Tok}` ? CheckTok<Tok, Tail, Now, Hdr, Pl>
  : { err: "missing" };

type CheckTok<Tok extends string, Tail extends string, Now extends string, Hdr extends string, Pl extends string> =
  Tok extends `${infer _H}.${infer _P}.${infer S}`
    ? AlgOk<Hdr> extends true
      ? Equal<HmacB64<Tail>, S> extends true
        ? CheckClaims<Pl, Now>
        : { err: "invalid" }
      : { err: "invalid" }
    : { err: "invalid" };

type AlgOk<HdrJson extends string> = HdrJson extends `${infer _A}"alg":"HS256"${infer _B}` ? true : false;

// exp is required and must be strictly in the future. Every lookup that can come
// back `never` (claim absent) is tested with [X] extends [never] first: a bare
// `never extends true` is vacuously true and would accept the token.
type CheckClaims<PL extends string, Now extends string> =
  [After<PL, "exp">] extends [never] ? { err: "invalid" }
  : ReadNum<After<PL, "exp">> extends infer E extends string
    ? E extends "" ? { err: "invalid" }
      : [DecLt<Now, E>] extends [true] ? ValidClaims<PL> : { err: "invalid" }
    : { err: "invalid" };

type ValidClaims<PL extends string> =
  [After<PL, "sub">] extends [never] ? { err: "payload" }
  : After<PL, "sub"> extends infer SA
    ? SA extends string
      ? HasStr<SA> extends true
        ? ReadStr<SA> extends infer Sub extends string
          ? IsPosInt<Sub> extends true ? CheckName<PL, Sub> : { err: "payload" }
          : { err: "payload" }
        : { err: "payload" }
      : { err: "payload" }
    : { err: "payload" };

type CheckName<PL extends string, Sub extends string> =
  After<PL, "username"> extends infer NA
    ? [NA] extends [never] ? { err: "payload" }
      : NA extends string ? HasStr<NA> extends true ? { ok: true; sub: Sub; name: ReadStr<NA> } : { err: "payload" }
      : { err: "payload" }
    : { err: "payload" };
