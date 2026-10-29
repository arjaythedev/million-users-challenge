# Serialization regression checks

This .NET 10 console project links the production `../../JsonBuffer.cs` directly.
It uses Npgsql 10.0.3 and requires an existing PostgreSQL database, but does not need
the challenge schema or seed. It issues exactly 18 parameter-only `SELECT` queries;
it does not create objects, write data, or start a database.

Run from the submission directory with the .NET 10 SDK:

```sh
SERIALIZATION_DATABASE='Host=127.0.0.1;Port=5432;Username=postgres;Password=your-password;Database=postgres' \
  dotnet run --project tests/serialization/SerializationTests.csproj -c Release
```

PowerShell equivalent:

```powershell
$env:SERIALIZATION_DATABASE = 'Host=127.0.0.1;Port=5432;Username=postgres;Password=your-password;Database=postgres'
dotnet run --project tests/serialization/SerializationTests.csproj -c Release
```

Alternatively set `TEST_DATABASE_URL=postgres://user:password@host:port/database`.
Percent-encoded credentials are accepted. For extra connection options, use the full
Npgsql connection string in `SERIALIZATION_DATABASE`, which takes precedence.
Without either variable the local test default is
`Host=127.0.0.1;Port=55432;Username=postgres;Database=feed_perf_20261029`.
The test explicitly selects UTF-8 client encoding and never prints credentials.

No Native AOT compiler is required: this project runs as a normal managed executable
for both the Native AOT and JIT submissions. It does not build or start the API.

Checks cover exact compact JSON bytes, field order, UTC millisecond truncation,
quotes/backslashes/control characters, BMP and astral Unicode, 500-scalar body
boundaries, growing username buffers, and transitions from long text to short text.
Expected JSON is built independently from the original strings and an explicit
expected timestamp; there is no copied production serializer or benchmark code.

A 66,004-byte Unicode username crosses the 64 KiB retention cutoff. The test calls
`JsonBuffer.Policy.Return` and requires rejection, empty `Written`, and zeroed old
JSON memory. Actual `DefaultObjectPool` return/get cycles verify ordinary instances
are reused and oversized instances are discarded. PostgreSQL TEXT cannot contain
NUL or malformed Unicode, so those are not sent as database values.

A failure throws and exits nonzero. Successful execution prints one summary line.
This is a correctness test, not a load or capacity benchmark.
