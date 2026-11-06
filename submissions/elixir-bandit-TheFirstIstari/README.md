# elixir-bandit-TheFirstIstari

Elixir + Bandit + Plug + Postgrex + Jason. One BEAM VM, two Bandit listeners (PORT + EXTRA_PORT), same router.

## Versions
- Elixir 1.20.4 / OTP 29.1.2 via Hex prebuilt tarballs into `/opt/otp` + `/opt/elixir`, SHA-256 pinned in `install.sh` (needs >= OTP 27 / Elixir 1.17, gated at end of `install.sh`)
- Pinned in `mix.lock`: bandit 1.14.0, plug 1.20.3, postgrex 0.22.4, jason 1.4.5, thousand_island 1.6.0, db_connection 2.10.2
- Driver: Postgrex (direct, no Ecto); HTTP: Bandit; router: Plug.Router

## Why this is fast
- Minimal stack: no Phoenix, no Ecto. Plug.Router -> direct Postgrex, iodata responses, no access logs.
- Hand-rolled HS256 JWT (`:crypto.mac` + `hash_equals`, no JOSE): `:binary.split` token on `.`, header compared against pre-encoded `@header_b64`, single `Base.url_decode64` + single `Jason.decode` of payload per request; `sub`/`username`/`exp` checked every request, no caching (rule 7).
- Manual JSON encoder with exact key order, compact, millisecond-truncated `created_at`, UTF-8 safe; Jason only for request/JWT decode.
- One SPEC query per request via named-prepared Postgrex (`prepare: :named`, statement cache warmed at boot): pool `cores*14` (224 on 16-core, `DB_POOL` override, capped 512), `queue_target 1000` / `queue_interval 5000`, 5s query/connect timeouts (rule 5: 224 < 512 cap).
- Bandit/Thousand Island (`lib/feed/application.ex`): `read_timeout 120s` (spec needs 75s+ idle for 1M conns), `num_acceptors = max(2*cores, 4)` (32 on 16-core), `num_connections = max(32768, ceil(600000/acceptors))` per acceptor per port (16-core: 32 acceptors -> 32768 floor, total 1M/port; 4-core: 8 acceptors -> 75000), `num_listen_sockets = min(cores, 16)` with `reuseport: true`, `backlog 65535`, `recbuf/sndbuf/buffer 4096` (small for 1M conns), dual ports sharing one sharded pool (`min(cores,8)` Postgrex pools, per-scheduler routing without `Module.concat`). Cores from `System.schedulers_online()` (affinity/cgroup aware). `http_options: [compress: false]` + manual level-1 gzip in router (fast path for `Accept-Encoding: gzip`, threshold 1024). `gc_every_n_keepalive_requests: 1` keeps 1M idle heaps small.
- BEAM flags in `start.sh`: `+P/+Q 2097152` (LimitNOFILE 2097152), `+A $CORES` (async I/O threads = visible cores via `nproc`), `+K true`, `+sbwt none +sbwtdcpu none +sbwtdio none`, `ERL_MAX_PORTS 2097152`. `ulimit -n 2097152` (falls back to 1048576) in `start.sh`.
- Like detection via `num_rows` (INSERT ... ON CONFLICT DO NOTHING), post FK 23503 -> 404, user FK 23503 -> 500, no extra SELECT (rule 3). `GET /health` 503 returns fixed body (no DB message leak).

## Test results (testbed: shared single box, app+DB+mload)
- `test/run.sh`: 42/42 pass.
- 200k users: pass, p95 1.18ms, 0% errors.
- 500k users: pass, p95 2.9ms, p99 30.7ms, 0% errors.

## Testbed notes (local bench only, not part of submission)
- `ulimit -n 1048576` required on bench box (bench systemd sets LimitNOFILE=2097152).
- 20 loopback IPs needed for >56k conns (ephemeral-port ceiling per IP).
- conntrack NOTRACK rule is testbed-only (single-box NAT hairpin); bench topology uses separate loadgen machines, no tuning needed there.
- Single-box ceiling ~500k: app+DB+mload share 29GB, so mload starves past that. 1M needs separate loadgen per `bench/` topology.

## Scripts
- `install.sh` (root): OTP + Elixir via Hex prebuilt tarballs (x86_64/aarch64, SHA-256 pinned) into `/opt/otp` + `/opt/elixir`, then `mix local.hex/rebar`; ends with an Elixir>=1.17/OTP>=27 version gate.
- `build.sh` (user): `mix deps.get` + `mix release` to `_build/prod/rel/feed`.
- `start.sh` (foreground): `ulimit -n 2097152`, reads `DATABASE_URL/JWT_SECRET/HOST/PORT/EXTRA_PORT/DB_POOL` (PORT+EXTRA_PORT required, DB_POOL default `cores*14`), execs release `bin/feed start`, `/health` in <60s.
- `mix test`: pure unit tests (`test/feed_test.exs`, no DB) for JWT edge cases, JSON escaping, UTF-16 length, timestamp truncation.
