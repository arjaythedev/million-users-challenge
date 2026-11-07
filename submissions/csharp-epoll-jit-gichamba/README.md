# csharp-epoll-jit-gichamba

The C# server of [`csharp-epoll-aot-gichamba`](../csharp-epoll-aot-gichamba), built for the .NET JIT instead of
Native AOT. The source is identical; only the build differs.

| | |
|---|---|
| Language | C# 14, .NET 10 (SDK 10.0.303, pinned by `global.json`), JIT with tiered PGO |
| HTTP | our own HTTP/1.1 server on edge-triggered epoll, keep-alive and pipelining |
| Postgres | our own client for protocol 3.0: extended query, binary results, pipelined, SCRAM-SHA-256 |
| gzip | off by default; `GZIP_MIN` turns on our own encoder: one deflate block with the fixed Huffman codes, greedy matches; CRC-32 from Ubuntu's `libdeflate0` |
| Packages | none beyond the .NET SDK |

## Why a JIT build

Native AOT starts fast and carries no JIT, but its code is fixed at build time. The JIT recompiles the hot paths with
the profile it gathers while running (tiered PGO), so after warm-up it can produce better code for the paths this
workload uses. On our rig the JIT build used about 2 µs less per request. The scoring box may differ, so both are
submitted.

## Design, rules, settings, measurements and credits

All as in [`csharp-epoll-aot-gichamba`](../csharp-epoll-aot-gichamba/README.md).

## Build

- `install.sh` (root, once): libdeflate, ICU and OpenSSL from apt, and the .NET SDK 10.0.303 from Microsoft's tarball,
  checked against its published SHA-512, into `/usr/local/lib/dotnet-sdk-10.0.303`. It waits for the apt lock,
  retries downloads, and is quick and safe to run again.
- `build.sh` (normal user): publishes the server to `out/` for the .NET runtime installed with the SDK.
- `start.sh`: raises the soft descriptor limit to the hard one, points `DOTNET_ROOT` at that runtime and runs
  `out/feed` in the foreground.
