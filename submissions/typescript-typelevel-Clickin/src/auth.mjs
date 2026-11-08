// Token transport for the type checker: base64url decoding and SHA padding only.
// Extract a Bearer token's signing input (header.payload) and pad it to SHA-256
// blocks, counting the 64-byte HMAC key block in the length field. No secret and
// no hashing here -- the HMAC itself is computed by the type checker. The padded
// tail is passed as the last, raw field (it may contain separator bytes).
const b64urlDecode = (s) => { try { return Buffer.from(s.replace(/-/g, "+").replace(/_/g, "/"), "base64").toString("latin1"); } catch { return ""; } };
// From a request, derive the three auth values the type checker needs: the decoded
// header JSON, the decoded payload JSON, and the SHA-padded signing input. All are
// encoding/framing; the signature verification and claim checks happen in types.
export function authFields(rawReq) {
  const m = /\r\nauthorization:\s*Bearer\s+([^\r\n]+)/i.exec("\r\n" + rawReq);
  if (!m) return { hdr: "", pl: "", tail: "" };
  const parts = m[1].split(".");
  if (parts.length < 2) return { hdr: "", pl: "", tail: "" };
  const signing = parts[0] + "." + parts[1];
  const bytes = [...Buffer.from(signing, "latin1")];
  const bitlen = (64 + bytes.length) * 8;
  bytes.push(0x80);
  while (bytes.length % 64 !== 56) bytes.push(0);
  for (let i = 7; i >= 0; i--) bytes.push(Math.floor(bitlen / 2 ** (8 * i)) & 255);
  return { hdr: b64urlDecode(parts[0]), pl: b64urlDecode(parts[1]), tail: Buffer.from(bytes).toString("latin1") };
}
