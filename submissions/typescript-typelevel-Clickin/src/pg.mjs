// Minimal PostgreSQL v3 wire client. The harness owns exactly the parts the type
// system cannot: the socket, the startup packet and the authentication handshake
// (SCRAM-SHA-256 via node:crypto). Everything above the wire -- which statement to
// run, the parameter bytes, and how to read the reply -- is produced and consumed
// by the TypeScript type checker (see types/). This file is a byte mover.
import net from "node:net";
import crypto from "node:crypto";

const enc = (s) => Buffer.from(s, "latin1");
const i32 = (n) => { const b = Buffer.alloc(4); b.writeInt32BE(n | 0); return b; };
// a wire message: type byte + int32 length + payload
function msg(type, payload) {
  const body = Buffer.concat(payload.map((p) => (typeof p === "string" ? enc(p) : p)));
  return Buffer.concat([Buffer.from(type, "latin1"), i32(body.length + 4), body]);
}
const cstr = (s) => Buffer.concat([enc(s), Buffer.from([0])]);

// SCRAM-SHA-256 (RFC 5802 / 7677) using node:crypto.
function hmac(key, data) { return crypto.createHmac("sha256", key).update(data).digest(); }
function sha256(data) { return crypto.createHash("sha256").update(data).digest(); }
function xor(a, b) { const o = Buffer.alloc(a.length); for (let i = 0; i < a.length; i++) o[i] = a[i] ^ b[i]; return o; }

export class PgConn {
  constructor(opts) { this.opts = opts; this.sock = null; this.buf = Buffer.alloc(0); this.scram = null; }

  connect() {
    const o = this.opts;
    return new Promise((resolve, reject) => {
      const sock = net.connect({ host: o.host, port: o.port });
      this.sock = sock;
      let authResolve = resolve, authReject = reject, ready = false;
      sock.on("error", (e) => { if (!ready) authReject(e); else this._onFatal?.(e); });
      sock.on("close", () => { if (!ready) authReject(new Error("closed during auth")); else this._onFatal?.(new Error("closed")); });
      sock.on("connect", () => {
        const params = ["user", o.user, "database", o.database, "application_name", "tl", ""];
        const body = Buffer.concat([i32(196608), ...params.map((p) => cstr(p).subarray(0, p === "" ? 1 : undefined))]);
        sock.write(Buffer.concat([i32(body.length + 4), body]));
      });
      sock.on("data", (chunk) => {
        this.buf = Buffer.concat([this.buf, chunk]);
        for (;;) {
          if (this.buf.length < 5) return;
          const len = this.buf.readInt32BE(1);
          if (this.buf.length < len + 1) return;
          const type = String.fromCharCode(this.buf[0]);
          const body = this.buf.subarray(5, len + 1);
          this.buf = this.buf.subarray(len + 1);
          if (ready) { this._onMessage?.(type, body); continue; }
          try {
            const done = this._handleAuth(type, body);
            if (done === "ready") { ready = true; authResolve(this); }
            else if (done === "error") { ready = true; authReject(new Error(this._lastErr || "auth error")); }
          } catch (e) { authReject(e); }
        }
      });
    });
  }

  _handleAuth(type, body) {
    const o = this.opts;
    if (type === "R") {
      const code = body.readInt32BE(0);
      if (code === 0) return; // AuthenticationOk
      if (code === 10) { // SASL: offer SCRAM
        const nonce = crypto.randomBytes(18).toString("base64");
        this.scram = { nonce, first: `n=,r=${nonce}` };
        const init = `n,,${this.scram.first}`;
        this.sock.write(msg("p", [cstr("SCRAM-SHA-256"), i32(init.length), init]));
        return;
      }
      if (code === 11) { // SASLContinue
        const serverFirst = body.subarray(4).toString("latin1");
        const m = Object.fromEntries(serverFirst.split(",").map((kv) => [kv[0], kv.slice(2)]));
        const salt = Buffer.from(m.s, "base64"), iter = parseInt(m.i, 10);
        if (!m.r.startsWith(this.scram.nonce)) throw new Error("bad server nonce");
        const salted = crypto.pbkdf2Sync(Buffer.from(o.password, "utf8"), salt, iter, 32, "sha256");
        const clientKey = hmac(salted, "Client Key");
        const storedKey = sha256(clientKey);
        const withoutProof = `c=biws,r=${m.r}`;
        const authMsg = `${this.scram.first},${serverFirst},${withoutProof}`;
        const clientSig = hmac(storedKey, authMsg);
        const proof = xor(clientKey, clientSig).toString("base64");
        const serverKey = hmac(salted, "Server Key");
        this.scram.expect = hmac(serverKey, authMsg).toString("base64");
        const final = `${withoutProof},p=${proof}`;
        this.sock.write(msg("p", [final]));
        return;
      }
      if (code === 12) { // SASLFinal
        const sf = body.subarray(4).toString("latin1");
        if (sf !== `v=${this.scram.expect}`) throw new Error("server signature mismatch");
        return;
      }
      throw new Error("unsupported auth code " + code);
    }
    if (type === "E") { this._lastErr = parseErr(body); return "error"; }
    if (type === "Z") return "ready"; // ReadyForQuery
    // S (ParameterStatus), K (BackendKeyData), N (notice): ignore
    return;
  }

  write(bytes) { this.sock.write(bytes); }
  onMessage(fn) { this._onMessage = fn; }
  onFatal(fn) { this._onFatal = fn; }
  end() { try { this.sock?.destroy(); } catch {} }
}

export function parseErr(body) {
  const fields = {};
  let i = 0;
  while (i < body.length && body[i] !== 0) {
    const f = String.fromCharCode(body[i]); i++;
    let j = i; while (j < body.length && body[j] !== 0) j++;
    fields[f] = body.subarray(i, j).toString("latin1"); i = j + 1;
  }
  return fields; // C = SQLSTATE, M = message
}
