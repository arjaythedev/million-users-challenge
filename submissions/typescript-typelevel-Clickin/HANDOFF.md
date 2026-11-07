# HANDOFF — typescript-typelevel-Clickin

Status at handoff (2026-11-07): **passes all 42 checks locally**, committed and
pushed to the fork branch `typescript-typelevel-Clickin`
(github.com/Clickin/million-users-challenge). Fork CI (GitHub Actions, amd64 +
postgres:18) was just triggered by the push — **check that run first.**

Written by the previous session (Opus 4.8). Picking up in a new session: read this,
then `git log`/`git diff` the submission folder.

## What this is
The feed API with all application logic evaluated by the TypeScript 7.1 **type
checker** (native `tsgo`, via the `typescript/sync` programmatic API), once per
request. A thin Node harness does only transport/framing. See `README.md` for the
exact split of responsibilities.

## Current result
- `test/test.sh` against a fresh `feed_test`: **passed=42 failed=0**.
- Verified locally in the dev containers (see below), NOT yet via the fork's CI at
  time of writing — confirm the Actions run is green.

## Known-good, already verified
- Read paths (`/health`, `/feed`, `/posts/:id`, 400/404 errors) are byte-exact vs
  `test/golden/*.json`.
- Type-level HMAC-SHA256 matches `node:crypto` for real tokens (short & 3-block).
- All auth error paths, body validation, create, like, created_at formatting pass.

## Architecture (files)
- `types/app.ts` — entry `App<Input>`; phase `R` (route) and `F` (format).
- `types/{http,jwt,json,format,crypto,util,minilib}.d.ts` — the logic.
- `gen/sha.mjs` → `generated/sha.d.ts` (SHA/HMAC tables; HMAC key first-blocks
  `HIPAD`/`HOPAD` precomputed since the secret is fixed).
- `src/server.mjs` (HTTP + orchestration), `evaluator.mjs` (drives the checker),
  `pool.mjs`+`pg.mjs` (hand-written PG wire client: SCRAM in `node:crypto`).

## Harness⇄types protocol
- Phase 1 eval input: `R <US> now <US> rawRequest <US> headerJson <US> payloadJson <US> signingTail`
  - `US`=\u001f, `RS`=\u001e, `GS`=\u001d, `FS`=\u001c (see `types/util.d.ts`).
  - headerJson/payloadJson = harness base64url-decode of the token's 1st/2nd
    segments (pure transport). signingTail = SHA-padded `header.payload`, LAST field
    because its bytes may include a US.
- Phase 1 output: `D <RS> status <RS> body` (direct) or
  `Q <RS> act <RS> ctx <RS> p1 <RS> p2…` (run statement `act` with text params).
- Phase 2 eval input: `F <RS> ctx <RS> cmdTag <RS> sqlstate <RS> rowsBlob`
  (rows joined by GS, columns by FS). Output: `status <RS> body`.
- Harness substitutes two placeholders in the body: `\u0003` = uptime_s,
  `\u0004` = live DB error string.

## Key design decision
The harness does sockets, HTTP framing, PG wire framing (Parse/Bind/Execute/Sync
lengths), SCRAM auth, base64url-decode, and SHA message padding — all encoding/
framing. The type checker does every *decision*: routing, validation, the HMAC
compression itself, alg/sig/exp/claim checks, SQL selection, row interpretation,
response assembly. This mirrors PR #79 (css-chromium), whose `web/pg.js` likewise
did SCRAM/framing while CSS did the logic.

## Gotchas discovered (don't rediscover these)
1. **Type-level base64 DECODE of long inputs blows the depth limit** (`DecBits`
   "infinitely circular", TS5114). That's why header/payload are decoded in the
   harness. `B64UrlDecode` still exists in `crypto.d.ts` but is unused on the hot
   path — safe to delete if you want.
2. `never extends infer V extends string ? …` silently distributes to `never`. Use
   `[X] extends [never] ? …` (see `http.d.ts` `FindHeader`).
3. Alias-body tuples must be made eager (`[...] & {}`) and each `Rounds` step forced
   with `extends infer S2 extends number[][]`, or SHA either errors or is ~2.4×
   slower. Already done in `gen/sha.mjs`.
4. `PostFromRow` JSON-escapes the body (`EscJSON`): seeded bodies are plain ASCII so
   golden still matches, but user-created posts in the feed can contain quotes.
5. **Dev deployment staleness bit us repeatedly.** `docker cp <dir> <dest>` NESTS
   when dest exists. Always `rm -rf <dest> && mkdir && docker cp <src>/. <dest>/`.

## Dev environment (local, arm64 Mac + OrbStack)
- `mu-pg` (postgres:18, role feed/feed, DBs feed_seed + feed_test, net mu-net,
  scram-sha-256).
- `tbf-app`: Node 22 at `/opt/node22`, the submission deployed at `/tl`, reaches
  `mu-pg` over mu-net. Test suite + golden copied to `/repo-test`.
- Full local test cycle:
  ```
  docker exec mu-pg psql -U postgres -qc "DROP DATABASE IF EXISTS feed_test WITH (FORCE)" \
    -c "CREATE DATABASE feed_test TEMPLATE feed_seed"
  docker exec tbf-app bash -c 'pkill -f server.mjs; sleep 2; rm -rf /tl/types /tl/gen /tl/src /tl/generated; mkdir -p /tl/types /tl/gen /tl/src'
  (cd submissions/typescript-typelevel-Clickin && docker cp types/. tbf-app:/tl/types/ && docker cp gen/. tbf-app:/tl/gen/ && docker cp src/. tbf-app:/tl/src/)
  docker exec tbf-app bash -c 'cd /tl && export PATH=/opt/node22/bin:$PATH && bash build.sh'
  docker exec -d tbf-app bash -c 'cd /tl && export PATH=/opt/node22/bin:$PATH DATABASE_URL="postgres://feed:feed@mu-pg:5432/feed_test" HOST=127.0.0.1 PORT=3000 EXTRA_PORT=3001 DB_POOL=4 && exec node src/server.mjs > /tmp/tl.log 2>&1'
  sleep 10
  docker exec tbf-app bash -c 'cd /repo-test && JWT_SECRET=million-users-challenge bash test/test.sh http://127.0.0.1:3000' | tail
  ```

## Remaining risks / TODO
- **Confirm the fork CI run is green** (amd64 `@typescript/typescript-linux-x64`
  resolves from the lockfile; `build.sh` has `|| pnpm install`/`|| npm install`
  fallbacks). This is the real "make CI pass" check.
- `build.sh` prefers pnpm (user rule) with npm fallback; `pnpm-lock.yaml` committed.
- `src/server.mjs` keeps two `console.error` debug lines on the 500 paths — harmless,
  useful in CI; remove if undesired.
- Not performance-tuned by design (one checker eval per request). If you want higher
  throughput: reuse a base snapshot + `runWithTemporaryFileUpdate` instead of a new
  program per request (watch rule 7/9 — must not memoize JWT verification across
  requests; a fresh program per request satisfies that).
- Upstream PR to arjaythedev/million-users-challenge was NOT opened — awaiting the
  user's decision.
