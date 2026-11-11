# java-epoll-CH-Abhinav

High-performance zero-allocation Java 25 server for the Million Users Challenge.

## Architecture
- **I/O & Networking:** Thread-per-core pinned Linux `epoll` (`EPOLLET`) + `SO_REUSEPORT` via JDK 25 Foreign Function & Memory (FFM) API (`Linker.Option.critical(false)`).
- **Connection Memory Model:** 4-byte per-FD off-heap generation-guarded state table + shared 64 KB per-worker `recvScratch` buffer.
- **PostgreSQL Wire Client:** Custom non-blocking PG v3.0 Extended Query protocol with separated Read/Write connection pools, binary parameter binding, text `DataRow` streaming, and single-`Sync` group commit.
- **Authentication:** Per-request HS256 verification using precomputed HMAC `IPAD`/`OPAD` SHA-256 midstates and constant-time 64-bit word-XOR signature checks.
- **Serialization & Compression:** Unaligned 64-bit JSON templates, AVX-512 / AVX2 `jdk.incubator.vector` string escape scanning, and Intel ISA-L (`libisal.so.2`) hardware stateless deflate with fused CRC32.
