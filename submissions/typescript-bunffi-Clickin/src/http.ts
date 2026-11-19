// HTTP/1.1 request parsing over raw bytes. Only the headers the API needs are looked at, and the results land in
// module-level scalars (offsets into the parsed buffer) instead of an object per request.
export const MAX_HEAD = 16 * 1024;
export const MAX_BODY = 1024 * 1024;
export const INCOMPLETE = 0;
export const BAD = -1;
export const TOO_LARGE = -2;

export const M_OTHER = 0;
export const M_GET = 1;
export const M_POST = 2;

export let method = M_OTHER;
export let pathStart = 0;
export let pathEnd = 0;
export let authStart = -1; // -1: no Authorization header
export let authEnd = 0;
export let acceptsGzip = false;
export let keepAlive = true;
export let bodyStart = 0;
export let bodyEnd = 0;
export let needContinue = false;

function lower(c: number): number {
  return c >= 65 && c <= 90 ? c | 0x20 : c;
}

// Does b[s, e) equal the lowercase ASCII name, ignoring case?
function nameIs(b: Uint8Array, s: number, e: number, name: Uint8Array): boolean {
  if (e - s !== name.length) return false;
  for (let i = 0; i < name.length; i++) if (lower(b[s + i]!) !== name[i]) return false;
  return true;
}

// Does the comma-separated value b[s, e) contain the token (case-insensitive, parameters after ';' ignored)?
function hasToken(b: Uint8Array, s: number, e: number, token: Uint8Array): boolean {
  let i = s;
  while (i < e) {
    let j = i;
    while (j < e && b[j] !== 44) j++;
    let a = i, z = j;
    for (let k = a; k < z; k++) if (b[k] === 59) { z = k; break; }
    while (a < z && (b[a] === 32 || b[a] === 9)) a++;
    while (z > a && (b[z - 1] === 32 || b[z - 1] === 9)) z--;
    if (nameIs(b, a, z, token)) return true;
    i = j + 1;
  }
  return false;
}

const enc = new TextEncoder();
const H_AUTHORIZATION = enc.encode("authorization");
const H_ACCEPT_ENCODING = enc.encode("accept-encoding");
const H_CONTENT_LENGTH = enc.encode("content-length");
const H_CONNECTION = enc.encode("connection");
const H_EXPECT = enc.encode("expect");
const H_TRANSFER_ENCODING = enc.encode("transfer-encoding");
const T_GZIP = enc.encode("gzip");
const T_CLOSE = enc.encode("close");
const T_KEEP_ALIVE = enc.encode("keep-alive");
const T_CONTINUE = enc.encode("100-continue");

// Parses one request starting at b[off] (data ends at end). Returns its total length (head + body) when complete,
// INCOMPLETE when more bytes are needed, or BAD / TOO_LARGE.
export function parse(b: Uint8Array, off: number, end: number): number {
  needContinue = false;
  // the blank line ending the head
  let blank = -1;
  for (let i = off + 3; i < end; i++) {
    if (b[i] === 10 && b[i - 1] === 13 && b[i - 2] === 10 && b[i - 3] === 13) {
      blank = i - 3;
      break;
    }
  }
  if (blank < 0) return end - off > MAX_HEAD ? TOO_LARGE : INCOMPLETE;

  // request line: METHOD SP target SP HTTP/1.x
  let le = off;
  while (le < blank && b[le] !== 13) le++;
  let sp1 = off;
  while (sp1 < le && b[sp1] !== 32) sp1++;
  let sp2 = le - 1;
  while (sp2 > sp1 && b[sp2] !== 32) sp2--;
  if (sp1 <= off || sp2 <= sp1 + 1) return BAD;
  // "HTTP/1."
  if (le - sp2 - 1 < 8 || b[sp2 + 1] !== 72 || b[sp2 + 2] !== 84 || b[sp2 + 3] !== 84 || b[sp2 + 4] !== 80 ||
      b[sp2 + 5] !== 47 || b[sp2 + 6] !== 49 || b[sp2 + 7] !== 46) return BAD;
  keepAlive = !(le - sp2 - 1 === 8 && b[sp2 + 8] === 48); // HTTP/1.0 closes by default
  const ml = sp1 - off;
  if (ml === 3 && b[off] === 71 && b[off + 1] === 69 && b[off + 2] === 84) method = M_GET;
  else if (ml === 4 && b[off] === 80 && b[off + 1] === 79 && b[off + 2] === 83 && b[off + 3] === 84) method = M_POST;
  else method = M_OTHER;
  pathStart = sp1 + 1;
  pathEnd = sp2;
  for (let i = pathStart; i < sp2; i++) if (b[i] === 63) { pathEnd = i; break; }

  authStart = -1;
  authEnd = 0;
  acceptsGzip = false;
  let length = 0;
  let wantsContinue = false;
  let ls = le + 2;
  while (ls < blank) {
    let lineEnd = ls;
    while (lineEnd < blank && b[lineEnd] !== 13) lineEnd++;
    let colon = ls;
    while (colon < lineEnd && b[colon] !== 58) colon++;
    if (colon > ls && colon < lineEnd) {
      let vs = colon + 1, ve = lineEnd;
      while (vs < ve && (b[vs] === 32 || b[vs] === 9)) vs++;
      while (ve > vs && (b[ve - 1] === 32 || b[ve - 1] === 9)) ve--;
      const c0 = lower(b[ls]!);
      if (c0 === 97) {
        if (nameIs(b, ls, colon, H_AUTHORIZATION)) { authStart = vs; authEnd = ve; }
        else if (nameIs(b, ls, colon, H_ACCEPT_ENCODING)) acceptsGzip = hasToken(b, vs, ve, T_GZIP);
      } else if (c0 === 99) {
        if (nameIs(b, ls, colon, H_CONTENT_LENGTH)) {
          if (ve === vs || ve - vs > 10) return BAD;
          length = 0;
          for (let i = vs; i < ve; i++) {
            const d = b[i]! - 48;
            if (d < 0 || d > 9) return BAD;
            length = length * 10 + d;
          }
        } else if (nameIs(b, ls, colon, H_CONNECTION)) {
          if (hasToken(b, vs, ve, T_CLOSE)) keepAlive = false;
          else if (hasToken(b, vs, ve, T_KEEP_ALIVE)) keepAlive = true;
        }
      } else if (c0 === 101) {
        if (nameIs(b, ls, colon, H_EXPECT)) wantsContinue = hasToken(b, vs, ve, T_CONTINUE);
      } else if (c0 === 116) {
        if (nameIs(b, ls, colon, H_TRANSFER_ENCODING)) return BAD; // chunked bodies are out of scope
      }
    }
    ls = lineEnd + 2;
  }
  if (length > MAX_BODY) return TOO_LARGE;
  bodyStart = blank + 4;
  bodyEnd = bodyStart + length;
  if (bodyEnd > end) {
    needContinue = wantsContinue;
    return INCOMPLETE;
  }
  return bodyEnd - off;
}
