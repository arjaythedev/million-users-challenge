# C# per-core epoll and PostgreSQL pipelines

A Linux C# implementation using dedicated event loops and nonblocking libpq
pipelines. The default backend is epoll. The optional io_uring backend is retained
for comparison; the official start script uses epoll. See the measured comparison
below. It establishes no official challenge capacity score.

## Design

- One dedicated worker thread per available processor (up to 64); no CPU pinning
  inside the program. Each worker owns both HTTP listeners with `SO_REUSEPORT`,
  its clients, timers, buffers, and PostgreSQL connections.
- Linux epoll readiness notification. HTTP and libpq sockets are registered in
  the same worker event loop. No thread-pool dispatch in the request path.
- libpq prepared queries in nonblocking pipeline mode, four read connections
  and two write connections per worker; 384 connections maximum. Bounded queues
  and pipeline depth prevent unlimited query accumulation. Initially every
  command has its own Sync; writes succeed only after their synchronization
  result proves transaction completion. There are no uncertain-write retries.
- The same five endpoints, SQL, JWT checks, ordered JSON, Unicode body-length
  rules and commit-before-response contract as the challenge. No result caches.
- HTTP request buffers are rented on activity and returned after parsing. Idle
  clients retain connection state, not a large receive buffer. An intrusive
  timer wheel maintains the 130-second idle lifetime without scanning all clients.
- A synchronous gzip stream compresses negotiated responses of at least 1 KiB.
  Compression and JSON construction are intentionally ordinary BCL operations
  for the first measured prototype; no assumed benefit from replacing them.
- .NET SDK 10.0.403 / runtime 10.0.14, self-contained tiered JIT with PGO and
  Server GC. Runtime/SDK match the comparison builds.
- libpq 18.6, installed as the exact PGDG Ubuntu 24.04 packages
  `libpq5=18.6-1.pgdg24.04+2` and `libpq-dev=18.6-1.pgdg24.04+2` validated in CI.

`--io-uring` selects the optional io_uring **readiness polling** backend while
preserving the rest of the application. It does not use completion-based
accept/receive/send, registered files, provided buffers, or zero-copy send.
`--probe-io-uring` checks kernel/library support without opening a database.
There is no silent fallback to epoll when that mode fails.
`--probe-epoll` runs a local socket-pair check of readiness, unchanged interest,
stale registration rejection, and replacement without opening a database or HTTP
listener. Both probes exit after their checks.

## Build and tests

On Ubuntu 24.04 x86_64:

```sh
sudo bash install.sh
bash build.sh
./out/FeedApi --probe-epoll
DATABASE_URL=postgres://feed:password@127.0.0.1/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001 bash start.sh
```

`install.sh` uses the [official PostgreSQL Apt repository](https://www.postgresql.org/download/linux/ubuntu/)
and verifies its signing key fingerprint
`B97B0AFCAA1A47F044F244A07FCC7D46ACCC4CF8` before configuring the signed source.
It installs and verifies the exact libpq package versions above, prints them for
the installation log, and refuses to downgrade a newer installed libpq package.
The SDK archive remains verified with its pinned SHA-512 digest.

For the optional backend, install the host C compiler and make (Ubuntu's
`build-essential` package), then run `bash install-uring.sh` before `bash build.sh`.
The script builds the checksum-pinned liburing FFI 2.5 source and checks its ABI.
The default epoll backend does not need liburing or this optional build.

Run `/opt/million-users-dotnet-10.0.403/dotnet run --project
tests/http/ParserTests.csproj -c Release` for the fragmentation/framing unit checks.
The installer does not add this SDK to PATH. The fork-only comparison harness runs the
unchanged official 42 checks, extended API checks, and transport regressions
against the Linux binary before measuring it. Raw artifacts and environment
metadata are kept by the [fork-only comparison harness](https://github.com/wylieagab/million-users-challenge/tree/b1d3f258c3426730bf5b000112a27d393c3db871/experiments/reactor-comparison).
That harness records native package versions, kernel information, and both
readiness-probe results in its environment metadata and logs alongside the
.NET SDK/runtime information.

`tests/worker/WorkerTests.csproj` checks ordered overload responses, including
the degraded health contract. On Linux, `tests/worker-check.py --binary
./out/FeedApi --backend epoll` starts isolated child servers on ports 3300/3301
to check pipelining continuation bounds and recovery from descriptor exhaustion.
It requires `DATABASE_URL`; `--backend io-uring` repeats the same checks for the
optional backend. The comparison harness runs both automatically.

## Current limitations

The server binds IPv4. A hosted CI runner sharing its CPU/RAM with PostgreSQL and
the load generator cannot establish an official million-user score. The measurements
below evaluate the combined transport, allocation and database-driver design;
they do not isolate the effect of epoll or database pipelining individually.


## Controlled comparison — October 31, 2026

[Completed run and raw artifacts](https://github.com/wylieagab/million-users-challenge/actions/runs/41222739277)
compare five variants on each of three independent Ubuntu 24.04 runner allocations:
the original Minimal API, submitted Kestrel NativeAOT and JIT versions, and this
application with epoll and io_uring readiness polling.

The tested experiment head was `b1d3f258c3426730bf5b000112a27d393c3db871`.
Actions built merge checkout `a1f06c4e529d175921cb91377ccca72fe73a7b68`;
its recorded submission source hashes match that head. This submission retains
the same executable sources and scripts; this README describes the completed run.

Each variant ran once per host at 30,000 and 80,000 users, with rotated order,
a fresh app/database, a 60-second ramp and a 120-second hold. The app had two
logical CPUs, PostgreSQL one, and the generator one. Traffic used loopback,
the unchanged official workload with gzip and think-scale 2.5, and both ports.
CPU and latency metrics below include the ramp. RSS measures the app process,
not kernel socket memory. PostgreSQL 18.6, libpq 18.6, SDK 10.0.403 and runtime
10.0.14 were pinned.

Frozen baseline commits:
- Minimal API: `954719a0411aedf9f65884e810d2deefd8db7cdb`.
- Kestrel NativeAOT: `3cff0a51d19018639af2614be2308762a738133e`.
- Kestrel JIT: `a4af720c410b29b030fbec16fb37617c14d34d21`.

Ratios are candidate / baseline on the **same host and load**, summarized as
median [minimum, maximum]. Values below one mean less measured CPU, latency
or process memory. Each row below has three eligible paired trials on three hosts.

| Users | Candidate / baseline | CPU per successful request | Overall p99 | Peak app RSS |
|---:|---|---|---|---|
| 30,000 | epoll / Minimal API | 0.177 [0.146, 0.186] | 0.038 [0.025, 0.043] | 0.127 [0.125, 0.138] |
| 30,000 | epoll / Kestrel AOT | 0.237 [0.206, 0.248] | 0.157 [0.005, 0.216] | 0.145 [0.143, 0.147] |
| 30,000 | epoll / Kestrel JIT | 0.196 [0.160, 0.203] | 0.186 [0.070, 0.261] | 0.126 [0.123, 0.133] |
| 80,000 | epoll / Kestrel AOT | 0.302 [0.244, 0.309] | 0.067 [0.046, 0.149] | 0.083 [0.079, 0.089] |
| 80,000 | epoll / Kestrel JIT | 0.268 [0.206, 0.272] | 0.074 [0.035, 0.174] | 0.077 [0.073, 0.082] |
| 30,000 | io_uring / epoll | 1.052 [1.049, 1.113] | 1.000 [0.951, 1.105] | 1.013 [1.012, 1.078] |
| 80,000 | io_uring / epoll | 1.053 [1.039, 1.062] | 0.672 [0.465, 2.195] | 1.014 [0.997, 1.035] |

All 30 planned trials completed with zero HTTP failures and the requested
connection count held throughout the settled window. Twenty-eight passed the
diagnostic validity and workload thresholds. Minimal API at 80,000 users was
excluded on two hosts: database CPU was 91.2% and 89.5%, and overall p99 was
3.11 and 2.40 seconds. Only one eligible Minimal API pair remains at that load,
so it is not presented as a replicated comparison.

The epoll design reduced CPU and RSS consistently in these trials. The io_uring
polling adapter cost more CPU on every host at both loads, while its 80,000-user
tail-latency result varied in direction. Epoll remains the default. Neither
backend has an official user-capacity score from these diagnostics.

Both reactors passed, on all three hosts:
- all 42 unchanged official checks and 122 extended API checks;
- all 14 transport cases, including fragmentation, chunking, pipelining,
  write half-close and 76-second idle keep-alive;
- pipeline error/recovery and committed-write readback checks;
- worker continuation bounds and descriptor-exhaustion recovery.

Frozen baseline differences remain visible in the artifacts: all baselines pass
the 42 official checks; the original Minimal API fails an additional gzip
negotiation assertion, and all three baseline variants fail the two additional
write-half-close transport cases. Their sources were not modified or those
assertions reclassified as passing.

These are fixed-load, short, shared-host diagnostics with three runner
allocations. They support the measured implementation choice, not a projected
million-user capacity or a claim that epoll alone caused the improvement.
