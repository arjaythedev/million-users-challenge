# C# / Kestrel / Npgsql / NativeAOT — wylieagab

An implementation of the five challenge endpoints using C# 14, ASP.NET Core
Kestrel 10.0.14, and Npgsql 10.0.3. The submission builds a Linux x64 NativeAOT
executable with the pinned .NET SDK 10.0.403. NuGet dependencies are locked in
packages.lock.json; install.sh verifies the SDK archive's SHA-512 digest.

The design targets the challenge's combination of many mostly idle HTTP/1.1
connections and repeated database reads. There is no measured million-user
score yet, and this is not a claim to be the fastest possible C# implementation.

## Build and run

On the challenge's Ubuntu 24.04 x64 host:

    sudo bash submissions/csharp-kestrel-wylieagab/install.sh
    bash test/run.sh submissions/csharp-kestrel-wylieagab

The official harness creates a fresh database, builds the binary, starts both
listeners, and checks the exact response contract. For manual operation:

    cd submissions/csharp-kestrel-wylieagab
    bash build.sh
    DATABASE_URL='postgres://feed:password@db:5432/feed' \
      JWT_SECRET='million-users-challenge' \
      HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh

All application configuration comes from those five challenge environment
variables. The scripts do not change kernel settings, pin CPUs, create database
objects, alter PostgreSQL configuration, or require another service.

## Where the work is saved

- **A short HTTP path.** One RequestDelegate handles the five routes directly.
  There are no controllers, ORM objects, reflection-based serializers, or
  per-request logs. Kestrel serves both ports directly with HTTP/1.1.
- **Fresh queries with prepared plans.** One shared Npgsql data source uses
  positional parameters and automatic preparation after two uses. The five
  query shapes fit within eight preparation slots per physical connection.
  Every valid database-backed request executes the query from SPEC.md.
- **A bounded database pool.** The limit is clamp(16 × available processors,
  32, 256), below the challenge's 512-connection cap. A separate asynchronous
  gate bounds concurrently executing handlers at clamp(64 × processors,
  128, 1024). Waiting requests do not acquire a database connection or JSON
  buffer. There is no retry of an uncertain write.
- **Direct UTF-8 text reads and output.** NpgsqlDataReader.GetBytes copies the
  body and author fields into reusable byte scratch, and Utf8JsonWriter writes
  their UTF-8 spans with normal JSON escaping. The connection explicitly uses
  ClientEncoding=UTF8. This avoids the two GetString allocations per post and
  the UTF-8-to-UTF-16-to-UTF-8 conversion, plus a feed's intermediate DTO list.
  It still copies bytes; this is not zero-copy serialization. Timestamps use
  UTC and three fractional digits, truncating PostgreSQL microseconds.
- **Working memory is pooled, data is not cached.** Returned JSON buffers are
  cleared/reset, and the used portion of the byte scratch is cleared. Each
  serializer starts with 2 KiB of scratch and grows for longer usernames;
  neither output nor scratch buffers over 64 KiB are retained. Every next
  response is regenerated from its own request. JWT claims and verifications
  are never cached, and HS256 signatures are checked in constant time on every
  protected request.
- **Less connection bookkeeping.** Enlistment is disabled. Connection reset
  SQL is disabled because the application never changes session state.
  Npgsql's ordinary pool is used: its experimental multiplexing implementation
  has a centralized writer that can limit scaling across many cores.
- **Gzip and idle connection memory.** Gzip uses CompressionLevel.Fastest.
  Kestrel waits for incoming data before allocating a receive buffer, keeps
  idle connections for 130 seconds, and permits the host's large connection
  count. The listen backlog is 65,535. Request and response buffering limits
  provide backpressure.
- **Native compilation and managed-memory headroom.** NativeAOT removes JIT
  warm-up and trims the deployed runtime surface. Server GC is enabled;
  start.sh limits the GC heap to 60% of available memory to leave space for
  unmanaged and kernel socket allocations. This is a managed-heap cap,
  not a guarantee of total RSS or kernel memory at one million connections.

## Correctness details

Authentication runs before parsing the post ID on like routes. The token must
be an HS256 JWT with a valid signature and unexpired numeric exp; sub must be
a positive integer string and username must be a string. Body length counts
Unicode scalar values, matching PostgreSQL char_length rather than UTF-16 code
units. Body whitespace is trimmed.

INSERT ... RETURNING is fully drained/disposed before a success response is
written: receiving a returned row alone does not prove the implicit transaction
has committed. Likes await ExecuteNonQueryAsync; the affected row count
distinguishes a first like from an existing like. Foreign-key violations on
likes produce the specified 404. Reads always consult PostgreSQL.

The implementation uses the exact challenge SQL. It never moves JSON
construction into PostgreSQL or changes the fixed schema. Health performs
SELECT 1 and returns a degraded 503 response when the database is unavailable.

## Additional checks

After the official suite, against a running server and its existing test
database:

    node submissions/csharp-kestrel-wylieagab/tests/extended-regression.mjs \
      http://127.0.0.1:3000 http://127.0.0.1:3001 --idle

This dependency-free Node 18+ script checks both listeners, authentication
edge cases, response key order, malformed and wrong-shaped JSON, 500/501
Unicode-character boundaries, gzip negotiation, concurrent duplicate likes,
cross-port read-after-write, and reuse of the same socket after 76 seconds
idle. It creates posts and likes; run it on a disposable test database.
It supplements rather than replaces the official suite.

The `tests/serialization` project links the production JsonBuffer directly and
checks 18 parameter-only PostgreSQL cases, including Unicode, exact JSON,
oversized usernames, clearing, and actual pool reuse. See its README for the
connection settings and command. It does not modify the database.

## Validation and benchmarking

### Linux comparison with GetBytes

The [four-variant Linux CI comparison](https://github.com/wylieagab/million-users-challenge/actions/runs/40907888710)
tested the preceding GetString implementation and the GetBytes implementation
in both NativeAOT and tiered-JIT builds. Each variant passed **42 official
checks and 122 extended HTTP checks**. All measured runs had zero errors.
The experiment is preserved in [fork PR #2](https://github.com/wylieagab/million-users-challenge/pull/2).

The runner had four AMD EPYC 9V74 vCPUs and Ubuntu 24.04, with PostgreSQL 18.6
and .NET 10.0.14. Each process received 60 seconds of initial warm-up and a
five-second refresh before each of three 30-second measurements. The workload
was GET /feed only, with gzip, both ports, 128 keep-alive connections, and four
validating Node workers. All builds used Server GC and the same 60% managed
heap limit. Results below are medians of the three measured runs:

| Serializer / publish mode | Requests/s | App CPU µs/request | End-of-run process RSS MiB |
|---|---:|---:|---:|
| GetString / NativeAOT | 5,851.05 | 137.962 | 57.22 |
| GetString / JIT | 6,018.29 | 122.423 | 121.14 |
| GetBytes / NativeAOT | 5,955.35 | 129.701 | 80.86 |
| GetBytes / JIT | 6,084.22 | 117.114 | 138.41 |

PostgreSQL and the load generator shared the runner with the applications.
All four warmed application processes remained resident; only one received
benchmark traffic at a time. Trial order was reversed/rotated, but the three
orders were not fully balanced. App CPU includes all application threads;
RSS is process memory and excludes kernel socket memory. The small throughput
differences do not establish a general speedup. These measurements exercise
neither the official mixed workload nor millions of idle connections, and
they do not provide a capacity score or prove which publish mode will score
higher on the challenge boxes.

### Historical checks and Windows comparison, before GetBytes

Verified on 2026-10-27, before the UTF-8 read change:

- **Ubuntu 24.04 / PostgreSQL 18.6 / Linux NativeAOT:** the unchanged repository
  workflow installed the checksum-pinned SDK, compiled native code, started both
  ports, and passed the original test/run.sh suite: **42 passed, 0 failed**.
  [CI run](https://github.com/wylieagab/million-users-challenge/actions/runs/40612677527),
  implementation commit 9f66d627075c758e59288cc4dd462676d5dd3ad9.
- **Windows .NET 10.0.14 JIT:** Release build with zero warnings/errors;
  **1,060 authentication checks** and **124 extended HTTP checks** passed,
  including reuse of the same socket after 76 seconds idle.
- NuGet restore with the Linux RID and --locked-mode succeeded.

The earlier local, feed-only comparison against the unchanged examples/csharp
application used the submission's preceding GetString implementation, a fresh
copy of the official seed, gzip, four Node workers, 128 keep-alive
connections, a five-second warm-up per application, and three alternating
15-second measurements. Both applications used .NET 10.0.14 JIT, Server GC,
and the same 60% GC heap limit. Hardware: Windows 11, Ryzen 7 9800X3D
(8 cores / 16 logical processors), approximately 32 GB RAM. Both servers,
PostgreSQL, the validating load generator, and normal desktop processes shared
the host. There were no writes during these measurements.

| Run | C# example req/s | Submission req/s | Example p95 ms | Submission p95 ms |
|---|---:|---:|---:|---:|
| 1 | 11,124 | 12,666 | 24.58 | 19.74 |
| 2 | 7,420 | 11,626 | 38.46 | 22.25 |
| 3 | 5,319 | 8,457 | 53.94 | 32.15 |

All six runs had zero errors. The median rates were 7,420 and 11,626 req/s.
Host throughput drifted substantially over the sequence, so these short runs
are diagnostic evidence, not a stable capacity estimate or proof of a general
speedup. This historical comparison does **not** measure the current GetBytes
implementation, the Linux NativeAOT binary, a million idle sockets, the
official mixed workload, or separate database/load-generator hosts.

### Running further measurements

For a local feed-read comparison against the same database and hardware:

    node submissions/csharp-kestrel-wylieagab/tests/bench.mjs \
      --target http://127.0.0.1:3000 --seconds 15 --connections 128 --workers 4

The dependency-free Node harness keeps persistent connections, requests gzip,
validates every response, and reports JSON containing throughput, p50/p95/p99,
errors, and transferred body bytes. Its default workload is GET /feed only.
It uses bounded logarithmic histograms and joins workers after draining
in-flight requests. This closed-loop test has no simulated-user think time and
does not measure a million idle connections. Warm each implementation first;
repeat under identical conditions and report run-to-run variation. Running
the generator and database on the application machine creates shared-resource
contention, so these figures are diagnostic comparisons, not capacity scores.

For comparable capacity results, follow the current
[upstream scoring procedure](https://github.com/arjaythedev/million-users-challenge#scoring)
and use mload on separate machines with the specified seed, PostgreSQL settings,
gzip, and think-scale 2.5. Every scored run starts the application and database
fresh. Two-minute searches establish a candidate user count, followed by a
five-minute hold with p95 below 500 ms, p99 below one second, and fewer than 1%
errors. The challenge reports separate scores on 16, 8, and 4 cores; the current
ceilings are one million users on 16 cores and three million on 8 and 4 cores.
Pull requests are benchmarked as they arrive, oldest first, with results on the
leaderboard and PR. Record the exact commit, host resources, user count, ramp
and hold duration, achieved requests per second, p95/p99 latency, error rate,
open connections, process memory, and kernel socket memory.

Performance choices such as pool size, handler concurrency, gzip level, GC
headroom, and NativeAOT versus tiered JIT are hypotheses until measured on
that workload. No cached responses, database shortcuts, synthetic health-only
benchmark, or local requests-per-second figure is presented as an official
capacity score.

References:
[Npgsql preparation](https://www.npgsql.org/doc/prepare.html),
[Npgsql 10.0.3 GetBytes implementation](https://github.com/npgsql/npgsql/blob/v10.0.3/src/Npgsql/NpgsqlDataReader.cs#L1413-L1442),
[Npgsql multiplexing scaling discussion](https://github.com/npgsql/npgsql/issues/6449),
[Kestrel socket transport options](https://github.com/dotnet/aspnetcore/blob/main/src/Servers/Kestrel/Transport.Sockets/src/SocketTransportOptions.cs).
