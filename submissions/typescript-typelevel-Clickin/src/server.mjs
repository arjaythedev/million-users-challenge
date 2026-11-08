// The harness. It owns only what a type system cannot: TCP sockets, HTTP request
// framing (find the header/body boundary and Content-Length), the Postgres
// connection + authentication, and substituting two runtime values the types
// cannot know (process uptime, and a live DB error string). Every application
// decision -- routing, id/JSON validation, JWT+HMAC-SHA256 verification, which
// statement runs and with what bytes, reading the row columns, building the exact
// response body -- is computed by the TypeScript type checker in types/app.ts.
import net from "node:net";
import os from "node:os";
import cluster from "node:cluster";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { Evaluator } from "./evaluator.mjs";
import { Pool } from "./pool.mjs";
import { authFields } from "./auth.mjs";

const US = "\u001f", RS = "\u001e", GS = "\u001d", FS = "\u001c";
const HERE = path.dirname(fileURLToPath(import.meta.url));
const TYPES = path.join(HERE, "..", "generated");

// Server start time, shared by all workers so a restarted worker keeps reporting it.
const START = +(process.env.MU_START_MS ||= String(Date.now()));
const DATABASE_URL = process.env.DATABASE_URL;
const HOST = process.env.HOST || "0.0.0.0";
const PORT = +(process.env.PORT || 3000);
const EXTRA_PORT = +(process.env.EXTRA_PORT || 3001);
// One worker process per core, each with its own type checker child: a checker
// evaluation is single-threaded, so throughput scales with the process count.
const WORKERS = Math.max(1, +(process.env.WORKERS || os.availableParallelism()));
// DB_POOL is the total across workers; split it, at least 2 connections each.
const POOL = Math.max(2, Math.ceil(+(process.env.DB_POOL || 64) / WORKERS));

let ev, pool;

function httpResponse(status, bodyLatin1) {
  const text = { 200: "OK", 201: "Created", 400: "Bad Request", 401: "Unauthorized", 404: "Not Found", 500: "Internal Server Error", 503: "Service Unavailable" }[status] || "OK";
  const body = Buffer.from(bodyLatin1, "latin1");
  const head = `HTTP/1.1 ${status} ${text}\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: ${body.length}\r\nConnection: keep-alive\r\n\r\n`;
  return Buffer.concat([Buffer.from(head, "latin1"), body]);
}

function parseStatusBody(out) {
  const i = out.indexOf(RS);
  return { status: +out.slice(0, i), body: out.slice(i + 1) };
}

async function handle(rawReq) {
  const now = Math.floor(Date.now() / 1000);
  const a = authFields(rawReq);
  let out;
  try { out = ev.eval("R" + US + now + US + rawReq + US + a.hdr + US + a.pl + US + a.tail); }
  catch { return httpResponse(500, '{"error":"internal server error"}'); }

  if (out.charCodeAt(0) === 1) return httpResponse(500, '{"error":"internal server error"}'); // not a literal

  const fields = out.split(RS);
  if (fields[0] === "D") {
    return httpResponse(+fields[1], fields[2] ?? "");
  }
  if (fields[0] === "Q") {
    const act = +fields[1];
    const ctx = fields[2];
    const params = fields.slice(3);
    let res;
    try { res = await pool.run(act, params); }
    catch (e) { return formatPhase2(ctx, { rows: [], cmd: null, code: "08006", err: String(e.message || e) }); }
    return formatPhase2(ctx, res);
  }
  return httpResponse(500, '{"error":"internal server error"}');
}

function formatPhase2(ctx, res) {
  const rowsBlob = res.rows.map((r) => r.map((c) => (c === null ? "\u0002" : c)).join(FS)).join(GS);
  const input = "F" + RS + ctx + RS + (res.cmd ?? "") + RS + (res.code ?? "") + RS + rowsBlob;
  let out;
  try { out = ev.eval(input); }
  catch { return httpResponse(500, '{"error":"internal server error"}'); }
  if (out.charCodeAt(0) === 1) return httpResponse(500, '{"error":"internal server error"}');
  let { status, body } = parseStatusBody(out);
  // Substitute the two runtime placeholders the types leave behind.
  body = body.replace("\u0003", String(Math.floor((Date.now() - START) / 1000)));   // uptime_s
  if (body.includes("\u0004")) body = body.replace("\u0004", jsonEscape(res.err || "unreachable")); // db error
  return httpResponse(status, body);
}

function jsonEscape(s) { return s.replace(/[\\"\u0000-\u001f]/g, (c) => ({ "\\": "\\\\", '"': '\\"', "\n": "\\n", "\r": "\\r", "\t": "\\t" }[c] || "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"))); }

// Minimal HTTP/1.1 framing over a keep-alive socket.
function onSocket(sock) {
  sock.setNoDelay(true);
  let buf = Buffer.alloc(0);
  let busy = false;
  const pump = async () => {
    if (busy) return;
    const headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) { if (buf.length > 1 << 20) sock.destroy(); return; }
    const header = buf.subarray(0, headerEnd).toString("latin1");
    const m = /content-length:\s*(\d+)/i.exec(header);
    const clen = m ? +m[1] : 0;
    const total = headerEnd + 4 + clen;
    if (buf.length < total) { if (clen > 1 << 20) sock.destroy(); return; }
    const rawReq = buf.subarray(0, total).toString("latin1");
    buf = buf.subarray(total);
    busy = true;
    let resp;
    try { resp = await handle(rawReq); } catch { resp = httpResponse(500, '{"error":"internal server error"}'); }
    sock.write(resp);
    busy = false;
    if (buf.length) setImmediate(pump);
  };
  sock.on("data", (c) => { buf = Buffer.concat([buf, c]); pump(); });
  sock.on("error", () => {});
}

async function main() {
  ev = new Evaluator(TYPES);
  pool = new Pool(DATABASE_URL, POOL);
  await pool.start();
  // Warm the checker so the first real request isn't slow and /health is fast.
  try { ev.eval("R" + US + Math.floor(Date.now() / 1000) + US + "GET /health HTTP/1.1\r\n\r\n"); } catch {}
  for (const p of [PORT, EXTRA_PORT]) {
    const srv = net.createServer(onSocket);
    srv.listen(p, HOST, () => console.log(`listening ${HOST}:${p}`));
  }
}
if (cluster.isPrimary && WORKERS > 1) {
  for (let i = 0; i < WORKERS; i++) cluster.fork();
  cluster.on("exit", (w, code, sig) => {
    console.error(`worker ${w.process.pid} exited (${sig || code}), restarting`);
    cluster.fork();
  });
} else {
  main().catch((e) => { console.error(e); process.exit(1); });
}
