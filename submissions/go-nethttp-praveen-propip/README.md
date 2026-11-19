# Go - net/http, the boring one

A normal, boring-in-a-good-way Go web service, tuned with the settings that
matter for this challenge. If you run a Go API in production, this is the
shape you already know — it just has the right knobs turned.

**Stack:** Go 1.27.3 (pinned in `go.mod` / `install.sh`) · standard library
`net/http` · `jackc/pgx/v5` `pgxpool` (Postgres driver, pure Go) ·
`golang-jwt/jwt/v5` (bearer tokens) · `klauspost/compress` `gzhttp`
(response compression) · `encoding/json/v2` for JSON (built with
`GOEXPERIMENT=jsonv2`).

## How one request flows

Take `GET /posts/42`:

1. The kernel hands the connection to `net/http`, which already has one
   goroutine waiting on it (that's how net/http always works).
2. `handlers.go`'s router matches the path pattern and calls `getPost`.
3. `auth.go` is not involved — this route needs no token.
4. `db.go`'s pool picks an idle Postgres connection that already has the
   query prepared (planned once, reused forever). One query goes out, one
   row comes back.
5. `post.go` turns the row into JSON with exactly the key order the spec
   requires, `server.go`'s gzip layer squeezes it, and it goes back on the
   same keep-alive connection.

Writes (`POST /posts`, `POST /posts/:id/like`) do the same with an INSERT,
plus `auth.go` verifying the JWT signature and expiry first — every request,
never cached.

## The pieces

| File | What it does |
|---|---|
| `config.go` | reads the env vars from SPEC.md, fails fast if they're wrong |
| `main.go` | starts the pool, wires the handler, opens both ports, shuts down cleanly on SIGTERM |
| `server.go` | the `http.Server` settings that matter (see below) |
| `handlers.go` | the five endpoints + every error body, in spec order |
| `auth.go` | bearer-token middleware: HS256 + expiry checked on every request |
| `post.go` | the response shapes; key order = field order; timestamp formatting |
| `db.go` | pool sizing: held at 256 connections from startup, plans reused |

## The choices, and why each one is here

**No web framework.** The baseline Go entry used Gin and lost 2.6× to the
same code with different compression middleware (248k → 650k users). The
standard library router costs a few hundred nanoseconds per request; a
framework adds allocations and gives nothing back for a five-endpoint API.

**Compression via `gzhttp` with `MinSize(0)`.** The benchmark sends
`Accept-Encoding: gzip` and measures the network. `gzhttp` only takes a
deflate writer when a response is actually being written; the middleware the
baseline used grabbed one per *queued* request, which is where its 2.6× went.

**TCP keep-alive probes off (`KeepAlive: -1`).** Go probes every idle
connection every 15 s by default. At half a million mostly-idle users that is
~33k wasted packets per second. Our own 120 s idle timeout is what reaps dead
connections; the probes buy nothing.

**Pool held at size from startup (`MinConns = MaxConns = 256`).** Users ramp
in over 60 s; a pool that grows lazily spends that window opening connections
instead of answering. 256 sits comfortably inside the 512-connection rule.

**Prepared statements, reused per connection.** pgx prepares the four queries
once per pooled connection. That removes a parse/plan round trip from every
request — the rules explicitly allow it.

**`GOMEMLIMIT` at 75 % of RAM (`start.sh`).** Go's garbage collector will
happily target twice the live heap unless told a budget. Telling it keeps
memory predictable when a million idle connections are sharing the box with
the kernel's socket buffers.

**`encoding/json/v2`.** Same output, faster and allocation-lighter marshal of
the small response structs. Key order is pinned by unit tests at the byte
level.

## Measured 

Same challenge API, seeded database, load generator, on a 48-core dev box
with the server pinned to 16 cores and Postgres in a container:

- 42/42 checks in `test/test.sh`
- 50k simulated users: p95 4.7 ms, p99 34.6 ms, 0 % errors, pass
- ~1.6 GB RSS at 50k connections