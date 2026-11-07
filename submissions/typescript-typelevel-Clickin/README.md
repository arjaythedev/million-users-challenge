# typescript-typelevel

The feed API, with its application logic **computed by the TypeScript type checker**
(TypeScript 7.1 beta, the native `tsgo`). It's a meme in the spirit of
`css-chromium-0x95` (PR #79): the interesting work runs at the type level, and a
thin Node harness does only what a type system cannot.

**Language / runtime:** TypeScript `7.1.0-beta` type checker driven through its new
programmatic API (`typescript/sync`), on Node.js 22. Driver: a hand-written
PostgreSQL v3 wire client (no external DB library).

## What runs where

The point of the challenge's rules is that the *application logic* is yours to
implement; here that logic lives in `types/*.d.ts` and is evaluated by the type
checker, once per request. The harness (`src/*.mjs`) owns only encoding/transport —
the same class of work (framing, lengths, sockets, auth handshake) that any server's
plumbing does.

| Computed by the **type checker** (`types/`) | Done by the **harness** (`src/`) |
|---|---|
| HTTP request parsing, routing | TCP sockets on ports 80 & 81, HTTP request framing (header/body boundary, Content-Length) |
| id / JSON-body validation, error selection | Postgres connection + **SCRAM-SHA-256** authentication (`node:crypto`) |
| JWT: **HMAC-SHA256** compression, `alg` check, signature compare, `exp`/`sub`/`username` validation | base64url-decode of the token header/payload, SHA message padding (length framing) |
| which prepared statement runs and the parameter bytes | Parse/Bind/Execute/Sync wire framing; splitting DataRow into column bytes |
| interpreting the reply: `created_at` → ISO-millis, JSON assembly, `like` 201/200, FK-miss → 404 | substituting two values a type can't know: process uptime, a live DB error string |

SHA-256 is implemented as types over nibble lookup tables (`gen/sha.mjs` generates
`generated/sha.d.ts`); the HMAC key blocks are constant (the secret is fixed) so
their first compression is precomputed at generation time.

## Files

- `types/` — the type-level program: `app.ts` (entry/router/formatter), `http.d.ts`,
  `jwt.d.ts`, `json.d.ts`, `format.d.ts`, `crypto.d.ts`, `util.d.ts`, `minilib.d.ts`.
- `gen/sha.mjs` — generates `generated/sha.d.ts` (SHA/HMAC tables + helpers).
- `src/` — `server.mjs` (HTTP + orchestration), `evaluator.mjs` (drives the checker),
  `pool.mjs` + `pg.mjs` (the wire client).
- `install.sh` / `build.sh` / `start.sh` — toolchain, assemble `generated/`, run.

## Performance

This is a correctness/meme entry, not a throughput entry: each request triggers a
fresh type-checker evaluation (tens of milliseconds), so throughput is low by
design. It passes all 42 checks in `test/run.sh`.
