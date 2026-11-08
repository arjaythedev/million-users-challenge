// Feed API on Bun with nothing but Bun: Bun.serve (uWebSockets underneath) for HTTP, Bun.SQL (the built-in
// Postgres client) for the database, node:crypto for HS256 and Bun.gzipSync (libdeflate) for gzip.
//
// Process model: the primary spawns one worker process per CPU (WORKERS, set by start.sh from nproc, which
// follows the affinity mask). Every worker listens on PORT and EXTRA_PORT with SO_REUSEPORT, so the kernel
// spreads the connections over the workers and nothing is shared between them. On one CPU there is no primary:
// the single process is the worker.
import { SQL } from "bun";
import { createHmac, timingSafeEqual } from "node:crypto";

const WORKERS = Math.max(1, Number(process.env.WORKERS) || navigator.hardwareConcurrency);
const STARTED_MS = Number(process.env.STARTED_MS) || Date.now();

if (process.env.WORKER_ID === undefined && WORKERS > 1) primary();
else worker();

function primary() {
  const children = new Map<number, ReturnType<typeof Bun.spawn>>();
  let stopping = false;
  const spawn = (i: number) => {
    children.set(i, Bun.spawn([process.execPath, import.meta.path], {
      env: { ...process.env, WORKER_ID: String(i), STARTED_MS: String(STARTED_MS) },
      // The worker exits when this pipe closes, so workers never outlive the primary (kill -9 included).
      stdin: "pipe",
      stdout: "inherit",
      stderr: "inherit",
      onExit(_proc, code, signal) {
        if (stopping) return;
        console.error(`worker ${i} exited (code=${code} signal=${signal}), restarting`);
        spawn(i);
      },
    }));
  };
  console.log(`bun ${Bun.version} primary ${process.pid}: ${WORKERS} workers`);
  for (let i = 0; i < WORKERS; i++) spawn(i);
  for (const sig of ["SIGTERM", "SIGINT"] as const) {
    process.on(sig, () => {
      stopping = true;
      for (const child of children.values()) child.kill("SIGTERM");
      process.exit(0);
    });
  }
}

function worker() {
  const { DATABASE_URL, JWT_SECRET } = process.env;
  const HOST = process.env.HOST || "127.0.0.1";
  const PORT = Number(process.env.PORT || 3000);
  const EXTRA_PORT = Number(process.env.EXTRA_PORT) || 0;
  if (!JWT_SECRET) { console.error("JWT_SECRET is required"); process.exit(1); }
  if (!DATABASE_URL) { console.error("DATABASE_URL is required"); process.exit(1); }

  if (process.env.WORKER_ID !== undefined) {
    (async () => {
      for await (const _ of Bun.stdin.stream()) { /* the primary never writes; we only wait for EOF */ }
      process.exit(0);
    })();
  }

  // Bun.SQL is a pool: it opens connections on demand up to `max`, prepares every query below once per
  // connection (named statements) and reuses it. 256 connections for the whole app by default, at most 64
  // per worker, which leaves room under the 512 limit. DB_POOL sets the per-worker size.
  const POOL = Number(process.env.DB_POOL) || Math.min(64, Math.max(4, Math.floor(256 / WORKERS)));
  const sql = new SQL({ url: DATABASE_URL, max: POOL, idleTimeout: 0, maxLifetime: 0, connectionTimeout: 5 });

  // SPEC.md's queries, verbatim. id is BIGINT (Bun.SQL returns int8 as a string), like_count is ::int (a number),
  // created_at is TIMESTAMPTZ (a Date).
  const selectFeed = () => sql`SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id
 ORDER BY p.created_at DESC, p.id DESC LIMIT 20`;
  const selectPost = (id: number) => sql`SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id
 WHERE p.id = ${id}`;
  const insertPost = (userId: number, body: string) =>
    sql`INSERT INTO posts (user_id, body) VALUES (${userId}, ${body}) RETURNING id, created_at`;
  const insertLike = (userId: number, postId: number) =>
    sql`INSERT INTO likes (user_id, post_id) VALUES (${userId}, ${postId}) ON CONFLICT (user_id, post_id) DO NOTHING`;

  // Post objects are written by hand: the keys in the spec's order, and the id goes in as it came (a digit string).
  // toISOString() is UTC with millisecond precision and a Z suffix.
  const postJson = (id: string | number, body: string, createdAt: Date, author: string, likeCount: number) =>
    `{"id":${id},"body":${JSON.stringify(body)},"created_at":"${createdAt.toISOString()}","author":${JSON.stringify(author)},"like_count":${likeCount}}`;
  const rowJson = (r: any) => postJson(r.id, r.body, r.created_at, r.username, r.like_count);

  // gzip (libdeflate, level 1) for bodies of 1 KB or more when the client asks for it: in practice the feed,
  // 5.1 KB -> 1.9 KB. A single post is ~300 bytes and goes out as is.
  const GZIP_MIN = 1024;
  const GZIP = { level: 1, library: "libdeflate" } as const;
  const JSON_HEADERS = { "Content-Type": "application/json" };
  const GZIP_HEADERS = { "Content-Type": "application/json", "Content-Encoding": "gzip", "Vary": "Accept-Encoding" };
  const json = (status: number, body: string, req?: Request) => {
    if (req && body.length >= GZIP_MIN && req.headers.get("accept-encoding")?.includes("gzip")) {
      return new Response(Bun.gzipSync(body, GZIP as any), { status, headers: GZIP_HEADERS });
    }
    return new Response(body, { status, headers: JSON_HEADERS });
  };
  const error = (status: number, message: string) => json(status, `{"error":${JSON.stringify(message)}}`);

  // ---- auth: HS256 only, signature and exp checked on every request (nothing is remembered) ----
  const KEY = Buffer.from(JWT_SECRET);
  const fromB64url = (s: string) => JSON.parse(Buffer.from(s, "base64url").toString());

  function verifyJwt(token: string): Record<string, unknown> | null {
    const d1 = token.indexOf(".");
    const d2 = token.indexOf(".", d1 + 1);
    if (d1 < 1 || d2 < 0 || token.indexOf(".", d2 + 1) !== -1) return null;
    let header, payload;
    try {
      header = fromB64url(token.slice(0, d1));
      payload = fromB64url(token.slice(d1 + 1, d2));
    } catch {
      return null;
    }
    if (header === null || typeof header !== "object" || header.alg !== "HS256") return null;
    const expected = Buffer.from(createHmac("sha256", KEY).update(token.slice(0, d2)).digest("base64url"));
    const given = Buffer.from(token.slice(d2 + 1));
    if (given.length !== expected.length || !timingSafeEqual(given, expected)) return null;
    if (payload === null || typeof payload !== "object") return null;
    const now = Date.now() / 1000;
    if (typeof payload.exp !== "number" || payload.exp <= now) return null;
    if (payload.nbf !== undefined && (typeof payload.nbf !== "number" || payload.nbf > now)) return null;
    return payload;
  }

  type User = { id: number; username: string };
  function authenticate(req: Request): User | Response {
    const h = req.headers.get("authorization");
    if (h === null || !h.startsWith("Bearer ")) return error(401, "missing bearer token");
    const payload = verifyJwt(h.slice(7));
    if (payload === null) return error(401, "invalid or expired token");
    const { sub, username } = payload;
    const id = typeof sub === "string" && /^[0-9]+$/.test(sub) ? Number(sub) : 0;
    if (!(id > 0) || !Number.isSafeInteger(id) || typeof username !== "string") return error(401, "invalid token payload");
    return { id, username };
  }

  // A positive integer, or 0 for anything else (0, -1, abc, 1.5, beyond 2^53).
  const parseId = (raw: string) => {
    if (!/^[0-9]+$/.test(raw)) return 0;
    const id = Number(raw);
    return Number.isSafeInteger(id) ? id : 0;
  };

  // ---- handlers ----
  async function health() {
    try {
      await sql`SELECT 1`;
    } catch (e: any) {
      return json(503, JSON.stringify({ status: "degraded", db: "unreachable", error: String(e?.message ?? e) }));
    }
    return json(200, `{"status":"ok","db":"ok","uptime_s":${Math.floor((Date.now() - STARTED_MS) / 1000)}}`);
  }

  async function feed(req: Request) {
    const rows = await selectFeed();
    let body = '{"posts":[';
    for (let i = 0; i < rows.length; i++) body += (i ? "," : "") + rowJson(rows[i]);
    return json(200, body + "]}", req);
  }

  async function getPost(req: Bun.BunRequest<"/posts/:id">) {
    const id = parseId(req.params.id);
    if (id === 0) return error(400, "invalid post id");
    const rows = await selectPost(id);
    if (rows.length === 0) return error(404, "post not found");
    return json(200, `{"post":${rowJson(rows[0])}}`, req);
  }

  async function createPost(req: Request) {
    const user = authenticate(req);
    if (user instanceof Response) return user;
    let parsed: any;
    try {
      parsed = JSON.parse(await req.text());
    } catch {
      return error(400, "malformed JSON body");
    }
    let text = parsed !== null && typeof parsed === "object" ? parsed.body : undefined;
    if (typeof text !== "string" || (text = text.trim()).length === 0) return error(400, "body is required");
    // "characters" are code points, like Postgres's char_length() in the CHECK constraint
    if (text.length > 500 && [...text].length > 500) return error(400, "body must be at most 500 characters");
    const [row] = await insertPost(user.id, text);
    return json(201, `{"post":${postJson(row.id, text, row.created_at, user.username, 0)}}`, req);
  }

  async function like(req: Bun.BunRequest<"/posts/:id/like">) {
    const user = authenticate(req);
    if (user instanceof Response) return user;
    const id = parseId(req.params.id);
    if (id === 0) return error(400, "invalid post id");
    let inserted: number;
    try {
      inserted = (await insertLike(user.id, id)).count;
    } catch (e) {
      if (e instanceof SQL.PostgresError && e.errno === "23503") return error(404, "post not found");
      throw e;
    }
    return json(inserted === 1 ? 201 : 200, `{"liked":true,"already_liked":${inserted !== 1},"post_id":${id}}`);
  }

  const routes = {
    "/health": { GET: health },
    "/feed": { GET: feed },
    "/posts": { POST: createPost },
    "/posts/:id": { GET: getPost },
    "/posts/:id/like": { POST: like },
  };

  for (const port of EXTRA_PORT ? [PORT, EXTRA_PORT] : [PORT]) {
    Bun.serve({
      hostname: HOST,
      port,
      reusePort: true,
      // Users sit idle up to ~40 s between requests; keep their connections for 2 minutes (Bun's maximum is 255 s).
      idleTimeout: 120,
      development: false,
      maxRequestBodySize: 64 * 1024,
      routes,
      fetch: () => error(404, "not found"),
      error(e) {
        console.error(e);
        return error(500, "internal server error");
      },
    });
  }
  if ((process.env.WORKER_ID ?? "0") === "0") {
    console.log(`worker ${process.pid} listening on ${HOST}:${PORT}${EXTRA_PORT ? ` and ${HOST}:${EXTRA_PORT}` : ""}, ` +
      `pg pool ${POOL} per worker x ${WORKERS} workers`);
  }
}
