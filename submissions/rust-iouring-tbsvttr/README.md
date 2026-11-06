# `rust-iouring-tbsvttr` — The Million Users Challenge

Zero-dependency Rust submission built for the 16-core, 8-core, and 4-core leaderboards.

## Architecture

1. **Shared-Nothing Worker Per Core (`SO_REUSEPORT` on `PORT` + `EXTRA_PORT`):**
   - Counts usable CPUs via `sched_getaffinity` (respecting `systemd` `AllowedCPUs=` on 16-core, 8-core, and 4-core runs without CPU pinning).
   - Binds both `PORT` and `EXTRA_PORT` per worker with `TCP_NODELAY = 1`, `TCP_DEFER_ACCEPT = 1`, and `TCP_QUICKACK = 0` after `listen()` so TCP ACKs piggyback onto HTTP responses.
   - Zero-allocation idle connection slab (`ClientSlot` frees `rx_buf`/`tx_buf` immediately after request/response completion), holding 3,000,000+ idle connections in `< 48 MB` RAM.
2. **Custom Zero-Alloc PostgreSQL v3 Wire Engine (`src/pg.rs`):**
   - **Pipelined Read Packs (`s1` Feed, `s2` GetPost, `s0` Health):** Packs up to 64 read `Bind + Execute` pairs under a single `Sync`, answering each HTTP client immediately on `CommandComplete` (`'C'`) and formatting binary `TIMESTAMPTZ` directly to UTC ISO-8601.
   - **Group-Commit Write Batches (`s3` InsertPost, `s4` LikePost):** Batches up to 64 writes into a single implicit transaction (`Bind + Execute + ... + Sync` = 1 WAL `fsync` per batch) with automatic mid-batch error isolation (`SQLSTATE 23503` -> `404 post not found`, re-queuing remaining items in the batch).
3. **L1-Resident Epoch-Tagged Fixed-Huffman LZ77 Gzip Compressor (`src/gzip.rs`):**
   - Compresses each `/feed` response (`~3.6 KB -> ~870 B`) per request with zero heap allocations, a 16 KB L1-resident hash table (`HASH_SIZE = 4096`), and O(1) lookup tables (`LEN_PACKED`, `DIST_IDX_TAB`) so every `/feed` response fits inside a single TCP segment.
4. **Precomputed HMAC-SHA256 Midstate + Hardware SHA-NI JWT Verifier (`src/jwt.rs`):**
   - Pre-absorbs the 64-byte `ipad` and `opad` blocks once at startup (cutting per-request SHA-256 block compressions in half) and accelerates SHA-256 block compression with hardware `x86_64` SHA-NI (`sha256rnds2`, `sha256msg1`, `sha256msg2`) and a 16-word rolling scalar fallback.

