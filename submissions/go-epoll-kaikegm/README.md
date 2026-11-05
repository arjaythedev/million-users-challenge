# go-epoll-kaikegm: Unified Epoll Loop + Hijacked PG Wire Protocol + Custom Dynamic Deflate + zerojwt

A pure-Go, zero-allocation server engineered for **The Million Users Challenge** (`C1000K`, 16/8/4 cores).

Every OS thread runs a single unified `epoll(7)` event loop (`runtime.LockOSThread()`) that owns its own `SO_REUSEPORT` listeners on `PORT` and `EXTRA_PORT`, its client sockets, and its hijacked PostgreSQL wire sockets. A request never leaves the thread that accepted it: HTTP parsing, JWT verification, pipelined Postgres wire encoding/decoding, JSON formatting, dynamic Huffman gzip compression, and non-blocking `SYS_WRITE` all execute on that single thread with **zero goroutines per connection or per request, zero channels, and zero locks**.

| Piece | Version |
|---|---|
| Front | Direct (serves `PORT` and `EXTRA_PORT` simultaneously, no Nginx) |
| Go | 1.27.3 (`/usr/local/go`, official tarball with sha256 check), `CGO_ENABLED=0` |
| Event Loop | Unified Linux `epoll(7)` (`golang.org/x/sys/unix`), `runtime.LockOSThread()`, raw non-blocking syscalls |
| Postgres | `github.com/jackc/pgx/v5/pgconn` (connects, authenticates & prepares the 5 SPEC statements once, then hijacks the socket into the loop's `epoll` set; wire protocol v3 `Bind`+`Execute`+`Sync`) |
| JWT | `github.com/KaikeGM/zerojwt/v2` v2.3.2 (Zero-alloc HS256 verification + explicit `exp` requirement) |
| Compression | Own per-loop gzip encoder (`deflate.go`: greedy LZ77 + Moffat-Katajainen dynamic Huffman tables) |

## Architecture & Tail-Latency ($p_{95}$ / $p_{99}$) Optimizations

1. **8-Byte Pointer-Free FD Slot Table (Zero GC Overhead at 1M Connections):**
   - Per-connection state is an 8-byte pointer-free `slot` (`gen uint32`, `loop uint16`, `kind uint8`, `flags uint8`) indexed directly by file descriptor in a demand-paged slice.
   - Because `slot` contains no pointers, the Go garbage collector never scans the connection table regardless of how many clients are connected.
   - Idle keep-alive connections hold no read or write buffers; memory is allocated only while a partial/pipelined request or an unsent `EAGAIN` response is in flight.

2. **In-Loop Hijacked PostgreSQL Wire Protocol (`pgwire.go` & `db.go`):**
   - At startup, `pgconn` connects, authenticates, and prepares the 5 verbatim `SPEC.md` statements (`s0`..`s4`), then hands the underlying socket descriptor (`conn.Hijack()`) to the loop's `epoll` instance.
   - 80 total Postgres connections (48 read + 32 write) are split evenly across the loops so the dedicated 16-core database machine is fully utilized even on 4-core tiebreaker runs.
   - **Mixed Wire Result Formats (`resultFormats`):** `id` and `like_count` are requested in text format (`0`) and copied straight from the Postgres `DataRow` into the JSON buffer without integer-to-ASCII conversion; `created_at` is requested as an 8-byte binary `timestamptz` (`1`) and formatted in-place using Howard Hinnant's integer civil-date algorithm (`appendPgTime`).
   - Literal `LIMIT 20` is preserved in the prepared feed statement so PostgreSQL's planner uses the index scan on `(created_at DESC, id DESC)`.

3. **Adaptive CPU $\leftrightarrow$ PostgreSQL Balancer (`epoll.go`, `db.go`, `deflate.go`):**
   - **PG-First Drain (`serve` Step 1):** Each `epoll_wait` pass drains all ready `kindPG` sockets **before** reading `kindClient` or `kindListener` sockets. Completed database responses leave immediately (cutting $p_{95}$/$p_{99}$ tail latency), and `c.inflight` counters reflect exact real-time load before new queries are dispatched.
   - **Elastic Read Cap (`dynReadCap`, `serve` Step 2):** Dynamically scales the per-connection batch cap (`8..64`) from the number of idle read connections (`c.inflight == 0`) and the incoming client burst size (`clientEvents`), spreading bursts across idle PostgreSQL backend processes in parallel while preserving single-`Sync` pipelining under sustained load.
   - **Rule 8 Group-Commit Writes & Solo Replay:** Write statements (`POST /posts` and `POST /posts/:id/like`) use dedicated write connections so reads never wait behind WAL commits. All writes in a loop pass share a single `Sync` (one implicit transaction, one commit). Each write's HTTP response is held in `c.held` after its `CommandComplete` and is transmitted **only when `ReadyForQuery` (`'Z'`) arrives after the commit**. If any statement in a write batch fails (e.g. SQLSTATE `23503` $\to$ `404`), any open batch is closed with `Sync` first and the remaining writes from the aborted batch are automatically replayed with `solo = true`.
   - **Closed-Loop CPU Utilization Meter & Gzip Relief Valve (`serve` Step 4):** Each loop tracks an EWMA of its own CPU utilization (`cpuPermil = workNs * 1000 / cycleNs`). When loop CPU exceeds $90\%$ or the event batch is $\ge 96$, `fastGzip` switches `deflate.go` from Moffat-Katajainen dynamic Huffman tree construction to a fast zero-sort tokenizer with precomputed RFC 1951 fixed Huffman tables (`BTYPE=01`), preventing CPU saturation from reaching $100\%$.
   - **Adaptive Coalescing Window (`unix.Nanosleep`):** Keeps the full $2\text{ ms}$ batching window on light passes ($< 32$ events) when no new listener connections are waiting (`listenerEvents == 0`), tapers the sleep window down linearly between $80\%$ and $96\%$ CPU utilization, and disables sleeping completely at $96\%$ CPU.

4. **Non-Blocking I/O & TCP Kernel Tuning:**
   - **Non-Blocking `SYS_WRITE` + `EPOLLOUT`:** Client and Postgres sockets use `unix.RawSyscall(SYS_READ / SYS_WRITE / SYS_ACCEPT4)` to bypass Go scheduler `entersyscall`/`exitsyscall` overhead. A slow client (`EAGAIN`) never blocks the loop; unsent bytes wait in `outbuf[fd]` until `EPOLLOUT` fires.
   - **Delayed ACK Piggybacking (`TCP_QUICKACK = 0`) & Linear Retransmissions (`TCP_THIN_LINEAR_TIMEOUTS = 1`):** `TCP_QUICKACK` is disabled on listening sockets (and re-disabled after responses taking $\ge 25\text{ ms}$) so request ACKs piggyback on HTTP response packets, cutting outbound packet count in half. `TCP_THIN_LINEAR_TIMEOUTS` prevents exponential RTO doubling on lost packets.

5. **Dual-Mode Per-Loop Gzip Encoder (`deflate.go`) & Zero-Alloc Auth (`auth.go`):**
   - Responses $\ge 1,024$ bytes (`GET /feed`) are compressed by a zero-allocation per-loop DEFLATE encoder supporting both Moffat & Katajainen canonical dynamic Huffman tables (`BTYPE=10`) under normal CPU load and fast fixed Huffman (`BTYPE=01`) when the adaptive CPU valve triggers.
   - Every authenticated request verifies the compact HS256 signature and required `exp` claim via `zerojwt/v2` v2.3.2 (`AllowMissingExp: false` in `jwt.go:522, 1411-1413` plus an explicit `!claims.HasExp` check in `auth.go`).
