# C epoll feed server

C, pthreads and Linux epoll, with a custom PostgreSQL v3 wire client and
source-built ISA-L 2.31.1 gzip. Based on the MIT-licensed
`c-epoll-unorig-username` at upstream commit
`5942bfafc1d5325679c88dbeb6ab8dd3f3c3fe42`; original attribution and LICENSE
are retained.

Run `bash install.sh` as root on Ubuntu 24.04, then `bash build.sh` as the
application user. The build verifies the pinned ISA-L source archive checksum.
`bash start.sh` runs in the foreground and reads `DATABASE_URL`, `JWT_SECRET`,
`HOST`, `PORT` and `EXTRA_PORT` from the challenge specification. Both ports
serve the same HTTP/1.1 API directly.

## Implementation

- One reactor per available CPU, with SO_REUSEPORT listeners and independent
  PostgreSQL lanes. The worker cap keeps total DB connections at or below 512;
  the normal 4/8/16-core configurations use approximately 64 connections.
- The prescribed SQL runs for each request. Prepared statements, bounded
  pipelines and separate read/write lanes reduce round trips. Partial writes
  keep offsets instead of repeatedly moving their remaining bytes.
- Read batches have a 250 us maximum buffering deadline and writes 1 ms, with
  immediate flushing at full batch boundaries. Queue, buffer and per-turn
  admission limits bound work; PG responses are serviced before client events.
- Writes are held until a correctly framed, idle ReadyForQuery confirms the
  implicit transaction boundary. Definite rollbacks isolate failed statements;
  uncertain writes fail without replay. Reads can retry after a lost lane.
- Bounded SSE2 JSON scanning/escaping and direct UTC timestamp formatting avoid
  general serializers. ISA-L full Huffman tables adapt to live JSON statistics
  once per second per worker; expanding compression falls back to identity.
  Response data and verification outcomes are not cached.
- Every authenticated request verifies its HMAC and claims against fresh wall
  time. The exact common HS256 header has a syntax fast path; other valid
  representations use the complete JSON and algorithm parser.
- Client closes are deferred until the returned epoll batch and its callbacks
  finish, preventing fd reuse underneath stale events. Generation checks drop
  late query results. Normal input remains level-triggered without per-response
  rearming. An event received while busy pauses input lazily; blocked output
  drains to EAGAIN with edge-triggered notifications. Replies remain ordered
  after a peer half-closes.
- Idle clients retain a 40-byte record without input/output allocations.
  Fragmented input starts at 1 KiB and grows to at most 64 KiB. Connections have
  no application idle eviction. Framing validates lengths, CRLF, versions and
  header tokens. An exact byte table and bounded SSE2 value scans combine
  header validation with delimiter detection; gzip negotiation respects quality
  values and exclusions.

## Validation

The combined implementation passes all 42 official checks on the full official
seed with PostgreSQL 18. Focused tests run with `bash tests/run.sh` after the
normal build; they need GCC and Python 3, but no database. They cover JSON guard
pages and Unicode, UTC timestamp boundaries, independent gzip decoding, partial
I/O, fd reuse across workers, half-close/pipeline ordering, HTTP framing and
negotiation, commit failures and uncertain-write holdback, fake startup
handshakes, and independent Python HMAC vectors. ASan/UBSan and an optimized
transport run are included. HTTP scanner tests use scalar oracles, protected
pages, a frozen baseline parser, all-byte mutations and fragmented requests.

Laptop loopback tests are development comparisons, not an official capacity
score. Parser and backpressure policies still need measurements on the scoring
machines; output draining can affect reactor fairness. PostgreSQL DNS/login/reconnect still runs synchronously
on its owning reactor, and allocation limits are per connection/lane rather
than a global memory quota. No host tuning or CPU pinning is performed by the
submission.
