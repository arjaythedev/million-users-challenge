# `rust-iouring-tbsvttr` — The Million Users Challenge

Zero-dependency Rust submission built for the 16-core, 8-core, and 4-core leaderboards.

## Architecture

1. **Affinity-Aware Shared-Nothing Worker Per Core (`SO_REUSEPORT` on `PORT` + `EXTRA_PORT`):**
   - Detects allowed CPUs via `sched_getaffinity` (respecting `systemd` `AllowedCPUs=` on 8-core and 4-core runs) and pins each worker thread to its dedicated core.
   - Binds both `PORT` and `EXTRA_PORT` per worker with `TCP_NODELAY = 1`, `TCP_DEFER_ACCEPT = 1`, and `TCP_QUICKACK = 0` after `listen()` so TCP ACKs piggyback onto HTTP responses.
   - Zero-allocation idle connection slab (`ClientSlot` frees `rx_buf`/`tx_buf` immediately after request/response completion), holding 3,000,000+ idle connections in `< 48 MB` RAM.
2. **Custom Zero-Alloc PostgreSQL v3 Wire Engine (`src/pg.rs`):**
   - **Pipelined Read Packs (`s1` Feed, `s2` GetPost, `s0` Health):** Packs up to 64 read `Bind + Execute` pairs under a single `Sync`, answering each HTTP client immediately on `CommandComplete` (`'C'`) and formatting binary `TIMESTAMPTZ` directly to UTC ISO-8601.
   - **Group-Commit Write Batches (`s3` InsertPost, `s4` LikePost):** Batches up to 64 writes into a single implicit transaction (`Bind + Execute + ... + Sync` = 1 WAL `fsync` per batch) with automatic mid-batch error isolation (`SQLSTATE 23503` -> `404 post not found`, re-queuing remaining items in the batch).
3. **Epoch-Tagged Fixed-Huffman LZ77 Gzip Compressor (`src/gzip.rs`):**
   - Compresses `/feed` responses (`~3.6 KB -> ~870 B`) with zero allocations and O(1) epoch-counter hash resets so every `/feed` response fits inside a single TCP segment.
4. **Precomputed HMAC-SHA256 Midstate JWT Verifier (`src/jwt.rs`):**
   - Pre-absorbs the 64-byte `ipad` and `opad` blocks once at startup, cutting per-request SHA-256 block compressions in half.

