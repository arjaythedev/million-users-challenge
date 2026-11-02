# elixir-bandit-tbsvttr: WhatsApp did this in 2012

In 2012 WhatsApp held over 2 million connections on one server with Erlang. This is the feed API (`SPEC.md`)
on the same VM, the BEAM, in Elixir: Bandit for HTTP, Postgrex for Postgres, one Erlang process per connection.
No Phoenix and no `Plug.Router`, just a bare Plug.

## Stack

| Piece | Version |
|---|---|
| Erlang/OTP | 29.1.2, the Hex team's prebuilt build for Ubuntu 24.04 (what `setup-beam`, asdf and mise install), SHA-256 pinned in `install.sh`, in `/opt/otp` |
| Elixir | 1.20.4, the official precompiled `elixir-otp-29.zip`, SHA-256 pinned, in `/opt/elixir` |
| HTTP | Bandit 1.14.0 on ThousandIsland 1.6.0, Plug 1.20.3 |
| Postgres driver | Postgrex 0.22.4 (DBConnection 2.10.2) |
| JSON | Elixir's built-in `JSON` (OTP's `:json`) for the request body; response JSON is built as iodata |
| JWT | hand-written HS256 check on `:crypto.mac/4` + `:crypto.hash_equals/2` |
| Compression | `:zlib`, gzip at level 1 |

`mix.lock` pins the whole dependency tree. `build.sh` installs Hex 2.6.0 and Mix's rebar3 into the folder, runs
`mix deps.get --check-locked` and builds a release with `mix release`. `start.sh` runs the release in the foreground
with distribution off.

## Process model

- **One BEAM node with one scheduler per CPU.** The BEAM reads the affinity mask, so systemd's
  `AllowedCPUs`/`CPUAffinity` or `taskset` decide the count (logged at startup).
- **One Erlang process per connection** (Bandit's HTTP/1 handler), and one more per socket from the `socket`
  backend (below). Both ports are served by the same node.
- `+P 4194304` and `+Q 2097152` leave room for a million connections.
- `+sbwt none` (and the dcpu/dio variants): schedulers sleep when idle instead of spinning, since the kernel's
  network processing needs the same cores.

## Connections and memory

These settings came out of measuring memory per open connection under `mload`:

| Setting | Server RSS at 10,000 connections |
|---|---:|
| Bandit defaults on `inet_drv` | ~2.4 GB |
| + full GC after every request (`gc_every_n_keepalive_requests: 1`) | ~1.6 GB |
| + `inet_backend: :socket` | ~0.97 GB |
| + `ERL_FULLSWEEP_AFTER 0` | ~0.59 GB |
| + `buffer: 4096` | ~0.56 GB |

- **The `socket` backend.** On the classic `inet_drv` backend, every connection that `mload` drove kept ~128 KB of
  binaries alive. They were referenced by no process and sat in no port queue, and they were freed when the
  connection closed. My own test clients (Node and Go, loopback and over the network, one or both ports, paced or
  burst) stayed at 7–13 KB per connection, and I didn't find what `mload` does differently. The `socket` backend
  doesn't have the problem.
- **GC after every request:** an idle connection doesn't keep its last response alive.
- **`ERL_FULLSWEEP_AFTER 0`:** every GC is a full sweep, so the heaps of mostly idle processes shrink back.
- **What's left is ~30 KB per connection:** Bandit's process (~6 KB), the `socket` backend's process per socket,
  and allocator overhead. A million connections would need ~30 GB, about all of the box.
- `read_timeout: 120_000`: ThousandIsland's default of 60 s is below the spec's 75 s.

## Database

- **One Postgrex pool per scheduler**, 256 connections in total (`DB_POOL` sets the total). A request uses the
  pool of the scheduler it runs on, so checkouts don't all queue on one pool process.
- Every statement is SPEC.md's SQL, prepared once per connection (`cache_statement`), one per request, in
  autocommit. Like on a missing post: `foreign_key_violation` becomes 404 `post not found`.
- `queue_target: 1000` and `queue_interval: 5000`: under load, a checkout may wait ~2 s before DBConnection starts
  dropping requests (the default is 100 ms). A slow answer beats an error.
- `created_at` is a UTC `DateTime` with microseconds, truncated (not rounded) to milliseconds.
- `body` and `username` are copied out of the Postgres receive buffer (`:binary.copy/1`) before they go into a
  response.

## Results

`test/run.sh`: **passed=42 failed=0** on Ubuntu 24.04 aarch64 (Docker) against `postgres:18`.
`install.sh` on x86_64 downloads and verifies both archives, but I couldn't run the BEAM under Rosetta (x86
emulation on an ARM Mac): `prim_tty`'s NIF doesn't load there, for OTP 28 as well. CI is the first real x86 run.

Also checked by hand: `created_at` `.999999` comes out `.999Z`; gzip on the feed (1,908 bytes); the full API on
`EXTRA_PORT`; 200 requests over one connection; a connection idle for 80 s still answers; `Expect: 100-continue`;
503/500 while the database is unreachable, then recovery.

### Local load tests (not the benchmark hardware)

Same setup as `typescript-bun-tbsvttr`: Docker on an Apple Silicon Mac, the server's container pinned to N cores,
Postgres and `mload` on their own cores.

| Server | Cores | Load | Result | Server CPU (hold) | Server RSS |
|---|---:|---|---|---:|---:|
| **Elixir** | 1 | 10,000 users, `-think-scale 0.5` (~2,100 req/s) | p95 0.9 ms | 0.31 cores (~145 µs/req) | 559 MB |
| Bun | 1 | the same | p95 0.9 ms | 0.21 cores (~99 µs/req) | 48 MB |
| Node example | 1 | the same | p95 2.0 ms | 0.43 cores (~200 µs/req) | 171 MB |
| **Elixir** | 2 | 125,000 users, `-gzip -think-scale 2.5`, 3 min hold | **fail**: p95 788 ms, p99 2.96 s, 0 errors | 1.38 cores | 3,972 MB |
| Bun | 2 | the same | pass: p95 3.9 ms | ~0.45 cores | 167 MB |
| Node example | 2 | the same | pass: p95 3.2 ms | 0.84 cores | 1,011 MB |

- **Per request** at moderate load, Elixir is between Bun and Node.
- **With 125,000 connections open**, it needs about 3× Bun's CPU and 1.6× Node's, and ~32 KB of memory per
  connection.
- **The p99 failure:** some of it came from `mload` collecting its own garbage near the end, while the server ran
  at ~1.8 of its 2 cores. Bun and Node passed with the same disturbance.
- **On the box:** going by these numbers, memory runs out before a million, and CPU limits it below what the Node
  example held. My guess for 16 cores is a few hundred thousand.
