// JWT claim cases, evaluated by the type checker alone (no server, no database).
// Run after build.sh:  node test/jwt-claims.mjs
// Each case sends POST /posts with a token signed with JWT_SECRET and checks the
// type program's first-phase decision: a 401 with the SPEC message, or a query
// (the token was accepted).
import crypto from "node:crypto";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Evaluator } from "../src/evaluator.mjs";
import { authFields } from "../src/auth.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SECRET = "million-users-challenge";
const US = "\u001f", RS = "\u001e";
const now = Math.floor(Date.now() / 1000);

const b64 = (s) => Buffer.from(s).toString("base64url");
function sign(payload, header = { alg: "HS256", typ: "JWT" }, secret = SECRET) {
  const si = b64(JSON.stringify(header)) + "." + b64(typeof payload === "string" ? payload : JSON.stringify(payload));
  return si + "." + crypto.createHmac("sha256", secret).update(si).digest("base64url");
}

const ACCEPT = "accept";
const INVALID = '{"error":"invalid or expired token"}';
const PAYLOAD = '{"error":"invalid token payload"}';
const MISSING = '{"error":"missing bearer token"}';

const cases = [
  ["valid token", sign({ sub: "1", username: "alice", iat: now, exp: now + 3600 }), ACCEPT],
  ["no exp", sign({ sub: "1", username: "alice", iat: now }), INVALID],
  ["exp in the past", sign({ sub: "1", username: "alice", exp: now - 10 }), INVALID],
  ["exp == now", sign({ sub: "1", username: "alice", exp: now }), INVALID],
  ["exp not a number", sign({ sub: "1", username: "alice", exp: "soon" }), INVALID],
  ["exp null", sign({ sub: "1", username: "alice", exp: null }), INVALID],
  ["alg none", sign({ sub: "1", username: "alice", exp: now + 3600 }, { alg: "none" }), INVALID],
  ["wrong secret", sign({ sub: "1", username: "alice", exp: now + 3600 }, undefined, "nope"), INVALID],
  ["no sub", sign({ username: "alice", exp: now + 3600 }), PAYLOAD],
  ["sub not an integer", sign({ sub: "abc", username: "alice", exp: now + 3600 }), PAYLOAD],
  ["no username", sign({ sub: "1", exp: now + 3600 }), PAYLOAD],
  ["no Authorization", null, MISSING],
];

const ev = new Evaluator(path.join(HERE, "..", "generated"));
let failed = 0;
for (const [name, tok, want] of cases) {
  const body = '{"body":"hi"}';
  const raw = `POST /posts HTTP/1.1\r\nHost: x\r\n${tok ? `Authorization: Bearer ${tok}\r\n` : ""}Content-Type: application/json\r\nContent-Length: ${body.length}\r\n\r\n${body}`;
  const a = authFields(raw);
  const out = ev.eval("R" + US + now + US + raw + US + a.hdr + US + a.pl + US + a.tail);
  const f = out.split(RS);
  const got = f[0] === "Q" ? ACCEPT : f[0] === "D" && f[1] === "401" ? f[2] : JSON.stringify(out);
  const ok = got === want;
  if (!ok) failed++;
  console.log(`  ${ok ? "ok  " : "FAIL"} ${name}${ok ? "" : ` (want ${want}, got ${got})`}`);
}
console.log(`passed=${cases.length - failed} failed=${failed}`);
process.exit(failed ? 1 : 0);
