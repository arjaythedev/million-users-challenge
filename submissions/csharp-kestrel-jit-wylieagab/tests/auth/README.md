# Authentication regression checks

This dependency-free console test program links the production `Auth.cs` directly.
From the submission directory run:

```sh
dotnet run --project tests/auth/AuthTests.csproj -c Release
```

It checks signature and algorithm validation, expiration and optional `nbf`, malformed
base64/JSON/UTF-8, claim types, escaped names and Unicode strings, bounded large tokens,
and deterministic malformed-input fuzz cases. A failure throws and exits nonzero.
