# fsharp-aspnetcore-delneg: plain ASP.NET Core in F#

The feed API (SPEC.md) written the way a normal F# web app would be: stock ASP.NET Core on Kestrel (default
Socket transport), endpoint routing with `RequestDelegate` handlers, Npgsql's `NpgsqlDataSource` pool,
System.Text.Json, `GZipStream` for the one big body. One file, `src/Program.fs`.
No hand-written HTTP, Postgres or JSON code, no native code, no `unsafe`.

## Stack

| Piece | Version |
|---|---|
| Runtime / SDK | .NET 11 RC 3 (`11.0.100-rc.3.26522.148`, SHA-512 checked in `install.sh`, `global.json` pinned) |
| Language | F# (FSharp.Core `11.0.101-rc3.26522.148`) |
| Web | ASP.NET Core 11 (shared framework), Kestrel, Socket transport |
| Postgres | Npgsql 10.0.3 (`packages.lock.json`, locked restore) |
| JSON | System.Text.Json (shared framework) |
| gzip | `System.IO.Compression.GZipStream` (zlib-ng, in the runtime), `CompressionLevel.Fastest` |

## Design

- Handlers are plain `HttpContext -> Task` functions mapped with `MapGet`/`MapPost`; F# records are the JSON
  bodies (field order = key order), written with `JsonSerializer.SerializeToUtf8Bytes` and a Content-Length.
  `UseExceptionHandler` gives the 500 body, `MapFallback` the 404.
- gzip only for bodies over 1 KB (the feed, ~5 KB -> ~2.3 KB) when the client accepts it. Setting up a deflate
  stream for a 30-byte reply costs more than it saves; the ResponseCompression middleware did that for every
  response (worklog e4: -5% CPU per request).
- SQL: the SPEC's statements verbatim, one per request, autocommit (rule 3). Npgsql auto-prepare
  (`Max Auto Prepare=10`), `No Reset On Close`, multiplexing (requests share connections; worklog e5). Pool `min(128, 4 x cores)` (worklog e18), `DB_POOL` overrides (rule 5: <= 512).
- No caching of any kind across requests (rule 7); writes are autocommitted before the response (rule 8).
- JWT: HS256 checked by hand with `HMACSHA256.HashData` + `CryptographicOperations.FixedTimeEquals`, `exp` checked,
  on every request (rule 9). IdentityModel rejects the 23-byte challenge key (see examples/csharp), so no JwtBearer.
- Kestrel: both ports, keep-alive 130 s, accept backlog 65535. Requests run on the socket threads
  (`UnsafePreferInlineScheduling` + `DOTNET_SYSTEM_NET_SOCKETS_INLINE_COMPLETIONS=1`): every handler is short and
  async, so the thread-pool hop is pure cost (worklog e7: -8% CPU per request).
- Runtime: Server GC (the Web SDK default), `InvariantGlobalization`, thread-pool workers that don't spin for work
  (`DOTNET_ThreadPool_UnfairSemaphoreSpinLimit=0`; worklog e1: -10% CPU per request), and a GC heap hard limit of
  70% of RAM (`GCHeapHardLimitPercent=0x46`, as the C# example, against heap growth near 800k connections). The
  knobs are set in `start.sh`, each with its reason.

### Kept changes

| change | where | lines | measured (worklog) | source |
|---|---|---|---|---|
| thread-pool spin limit 0 | start.sh | +2 | -10% CPU/req, -16% cycles (e1) | EgorBo's blazing-fast-dotnet skill |
| gzip only bodies > 1 KB, in `send` | Program.fs | +13 | -5% CPU/req, -10% cycles (e4) | profile (deflate init per reply) |
| Npgsql multiplexing | Program.fs | +1 | -4.5% cycles, better p95, Postgres -10% (e5) | Npgsql docs |
| inline scheduling + inline socket completions | Program.fs, start.sh | +3, +2 | -8% CPU/req, -10% cycles (e7) | EgorBo's skill, Kestrel options |
| GC heap hard limit 70% | start.sh | +2 | CPU-neutral; insurance (e11) | examples/csharp README |
| pool 4 per core (was 8) | Program.fs | +1 (comment) | -5.5% CPU/req at 52k users on 4 cores, -7.4% at 40k on 2 cores, Postgres -10%; neutral at 20k (e18) | holds |
| simplify passes 1 and 2 | Program.fs | -5 | neutral (pass 2: -1%, 4 pairs) | `simplify` skill |

About 35 more experiments were measured and discarded, all listed in `experiments/worklog.md`. They include GC
modes, socket thread counts, Kestrel buffer options, pool sizes, Npgsql write coalescing, typed parameters,
Npgsql.FSharp (+2.7% CPU), Oxpecker routing (same speed, more packages), and the JIT/PGO knobs. Dynamic PGO, on by
default, is worth 7%. R2R gains nothing, and AOT doesn't run.

## Results (local, NOT official)

Synteq box, 8 shared vCPUs, 16 GB; Postgres 18, the load generator and an idle-connection holder share the cores
the server doesn't use. The C# example (`examples/csharp`, retargeted to net11.0) was measured the same way, with
the runs interleaved. Details and every run: `experiments/worklog.md`.

| | C# example | this entry |
|---|---|---|
| server CPU per request, 4 cores, 20k users + 100k idle (interleaved, 31 Oct) | 302 us, 765 kcycles, p95 62-71 ms | 252 us, 545 kcycles, p95 8-18 ms |
| Postgres CPU per request (same screens) | 295 us | 155 us |
| 2 cores, max users held 5 min (+100k idle) | ~20k (passes 20k x3, fails 22k) | ~40k (passes 32-40k; 40k at the edge, fails 44k) |
| 4 cores, max users held 5 min (+100k idle) | ~24k (fails 28k x2; Postgres-bound on this box) | ~52k (at the edge; 56k fails with the server 85% busy: box-bound) |
| RSS at 400k idle connections | 6.3-6.8 GB, 9-30% of pings fail | 6.0-6.1 GB, all pings answered |

## Lines of code

`src/Program.fs`: 230 lines (`wc -l`), plus `start.sh` (knobs) and the install/build scripts. The worklog has the
count after each change.
