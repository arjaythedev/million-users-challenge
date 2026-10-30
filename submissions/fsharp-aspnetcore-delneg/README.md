# fsharp-aspnetcore-delneg: plain ASP.NET Core in F#

The feed API (SPEC.md) written the way a normal F# web app would be: stock ASP.NET Core on Kestrel (default
Socket transport), endpoint routing with `RequestDelegate` handlers, Npgsql's `NpgsqlDataSource` pool,
System.Text.Json, the framework's `ResponseCompression` middleware for gzip. One file, `src/Program.fs`.
No hand-written HTTP, Postgres or JSON code, no native code, no `unsafe`.

## Stack

| Piece | Version |
|---|---|
| Runtime / SDK | .NET 11 RC 3 (`11.0.100-rc.3.26522.148`, SHA-512 checked in `install.sh`, `global.json` pinned) |
| Language | F# (FSharp.Core `11.0.101-rc3.26522.148`) |
| Web | ASP.NET Core 11 (shared framework), Kestrel, Socket transport |
| Postgres | Npgsql 10.0.3 (`packages.lock.json`, locked restore) |
| JSON | System.Text.Json (shared framework) |
| gzip | `Microsoft.AspNetCore.ResponseCompression` (shared framework) |

## Design

- Handlers are plain `HttpContext -> Task` functions mapped with `MapGet`/`MapPost`; F# records are the JSON
  bodies (field order = key order). `UseExceptionHandler` gives the 500 body, `MapFallback` the 404.
- SQL: the SPEC's statements verbatim, one per request, autocommit (rule 3). Npgsql auto-prepare
  (`Max Auto Prepare=10`), `No Reset On Close`. Pool `min(128, 8 x cores)`, `DB_POOL` overrides (rule 5: <= 512).
- No caching of any kind across requests (rule 7); writes are autocommitted before the response (rule 8).
- JWT: HS256 checked by hand with `HMACSHA256.HashData` + `CryptographicOperations.FixedTimeEquals`, `exp` checked,
  on every request (rule 9). IdentityModel rejects the 23-byte challenge key (see examples/csharp), so no JwtBearer.
- Kestrel: both ports, keep-alive 130 s, accept backlog 65535.

## Results

Local numbers on a Synteq 8-vCPU box are in `experiments/worklog.md`: **local, NOT official**.

## Lines of code

`src/Program.fs`: see the worklog for the count after each change.
