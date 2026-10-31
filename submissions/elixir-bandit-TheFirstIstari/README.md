# elixir-bandit-TheFirstIstari

Elixir + Bandit + Plug + Postgrex + Jason. One BEAM VM, two Bandit listeners (PORT + EXTRA_PORT), same router.

## Versions
- Elixir 1.20.4 / OTP 29.1 (testbed CachyOS; `install.sh` pins via Erlang Solutions repo on Ubuntu 24.04 noble — esl-erlang 1:28+ + elixir 1.19+ — because the noble archive (1.14/OTP 25) is too old; needs >= OTP 27 / Elixir 1.17, gated at end of `install.sh`)
- Pinned in `mix.lock`: bandit 1.14.0, plug 1.20.3, postgrex 0.22.4, jason 1.4.5, thousand_island 1.6.0, db_connection 2.10.2
- Driver: Postgrex (direct, no Ecto); HTTP: Bandit; router: Plug.Router

## Why this is fast
- Minimal stack: no Phoenix, no Ecto. Plug.Router -> direct Postgrex, iodata responses, no access logs.
- Hand-rolled HS256 JWT (`:crypto.mac` + `hash_equals`, no JOSE): `:binary.split` token on `.`, header compared against pre-encoded `@header_b64`, single `Base.url_decode64` + single `Jason.decode` of payload per request; `sub`/`username`/`exp` checked every request, no caching (rule 7).
- Manual JSON encoder with exact key order, compact, millisecond-truncated `created_at`, UTF-8 safe; Jason only for request/JWT decode.
- One SPEC query per request via named-prepared Postgrex (`prepare: :named`, statement cache warmed at boot): pool 224 (`DB_POOL`, capped 512), `queue_target 50` / `queue_interval 1000`, 5s query/connect timeouts (rule 5: 224 < 512 cap).
- Bandit/Thousand Island (`lib/feed/application.ex`): `read_timeout 120s` (spec needs 75s+ idle for 1M conns), `num_acceptors = max(2*cores, 4)` (32 on 16-core), `num_connections = max(32768, ceil(600000/acceptors))` per port (16-core: 32 acceptors -> 32768 floor; 4-core: 8 acceptors -> 75000, so acceptors*cap >= 600k headroom for 500k/port), `num_listen_sockets = min(cores, 16)` with `reuseport: true`, `backlog 4096`, dual ports sharing one pool. Cores from `System.schedulers_online()` (affinity/cgroup aware, not logical processors). `http_options: [compress: false]` (skips per-response deflate negotiation at 42k rps).
- BEAM flags in `start.sh`: `+P/+Q 2097152` (LimitNOFILE 2097152), `+A $CORES` (async I/O threads = visible cores via `nproc`; was `+A 96`, changed to `+A $CORES` in Pass7 — 16-core behavior changed 96 -> 16 but re-validated 42/42 + 200k pass), `+K true`, `+sbwt none +sbwtdcpu none +sbwtdio none`, `ERL_MAX_PORTS 2097152`. `ulimit -n 1048576` in `start.sh`. (No `ERL_MEM_TOTAL`: not a real OTP var, was a no-op — removed; memory bounded by pool/connection caps.)
- Like detection via `num_rows` (INSERT ... ON CONFLICT DO NOTHING), FK 23503 -> 404, no extra SELECT (rule 3).

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
- `install.sh` (root): Erlang+Elixir via Erlang Solutions repo on apt (esl-erlang + elixir, OTP 28 / Elixir 1.19) or pacman on Arch, then `mix local.hex/rebar`; ends with an Elixir>=1.17/OTP>=27 version gate.
- `build.sh` (user): `mix deps.get` + `mix release` to `_build/prod/rel/feed`.
- `start.sh` (foreground): `ulimit -n 1048576`, reads `DATABASE_URL/JWT_SECRET/HOST/PORT/EXTRA_PORT/DB_POOL` (PORT+EXTRA_PORT required, DB_POOL default 224), execs release `bin/feed start`, `/health` in <60s.
