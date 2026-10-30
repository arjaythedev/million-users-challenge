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
  (`Max Auto Prepare=10`), `No Reset On Close`, multiplexing (requests share connections; worklog e5). Pool `min(128, 8 x cores)`, `DB_POOL` overrides (rule 5: <= 512).
- No caching of any kind across requests (rule 7); writes are autocommitted before the response (rule 8).
- JWT: HS256 checked by hand with `HMACSHA256.HashData` + `CryptographicOperations.FixedTimeEquals`, `exp` checked,
  on every request (rule 9). IdentityModel rejects the 23-byte challenge key (see examples/csharp), so no JwtBearer.
- Kestrel: both ports, keep-alive 130 s, accept backlog 65535.
- Runtime: Server GC (the Web SDK default), `InvariantGlobalization`, and thread-pool workers that don't spin
  for work (`DOTNET_ThreadPool_UnfairSemaphoreSpinLimit=0` in `start.sh`; worklog e1: -10% CPU per request).

## Results

Local numbers on a Synteq 8-vCPU box are in `experiments/worklog.md`: **local, NOT official**.

## Lines of code

`src/Program.fs`: see the worklog for the count after each change.
