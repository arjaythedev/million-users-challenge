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

- SO_REUSEPORT listeners and independent PostgreSQL lanes. The normal build
  uses two reactors when exactly four CPUs are available to the process;
  other CPU counts retain one reactor per available CPU, up to the worker cap.
  Four available CPUs use 32 PostgreSQL connections per reactor (64 total:
  48 read and 16 write). The normal 2/8/16-core configurations also use
  approximately 64 connections, and the worker cap keeps the total at or below
  512. CPU availability follows process affinity, rather than physical-core
  detection. The four-CPU worker-count experiment was motivated by
  [`rust-iouring-mehrojmajidov` PR #125](https://github.com/arjaythedev/million-users-challenge/pull/125)
  and independently implemented and measured here.
- The prescribed SQL runs for each request. Prepared statements, bounded
  pipelines and separate read/write lanes reduce round trips. Partial writes
  keep offsets instead of repeatedly moving their remaining bytes.
- Read and write batches use 2 ms soft buffering targets, with immediate flushing
  at full batch boundaries. Partial event-loop turns coalesce up to a 2 ms
  cadence; a full event batch drains again immediately. This restores the
  original entry's batching cadence while retaining bounded queues and the
  connection-lifetime fixes. A partial-turn sleep can defer a pending flush by up to another 2 ms before
  scheduler delays. Idle loops still block in epoll. Queue, buffer
  and per-turn admission limits bound work; PG replies precede client events.
- Writes are held until a correctly framed, idle ReadyForQuery confirms the
  implicit transaction boundary. Definite rollbacks isolate failed statements;
  uncertain writes fail without replay. Reads can retry after a lost lane.
- Bounded SSE2 JSON scanning/escaping and direct UTC timestamp formatting avoid
  general serializers. ISA-L full Huffman tables adapt to live JSON statistics
  once per second per worker; expanding compression falls back to identity.
  Response data and verification outcomes are not cached.
- Every authenticated request verifies its HMAC and claims against fresh wall
  time. The exact common HS256 header has a syntax fast path; other valid
  representations use the complete JSON and algorithm parser. JWT segments and
  decoded usernames share the existing request-size budget; subjects use checked
  integer parsing, including long leading-zero forms. Scratch is worker-local,
  and queued inserts copy the body and token username before that scratch is reused.
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
handshakes, and independent Python HMAC vectors. JWT boundary regressions cover 16-digit
subjects through 2^53, leading zeros, long request-fitting headers/payloads and
usernames, auth-before-path/body ordering, and allocation-failure retention.
Both HTTP ports also passed fresh-seed official checks and a real PostgreSQL
large-username insert/echo/committed-row smoke in development. ASan/UBSan and an optimized
transport run are included. HTTP scanner tests use scalar oracles, protected
pages, a frozen baseline parser, all-byte mutations and fragmented requests. Deterministic worker-loop tests
exercise idle/full/partial turns, elapsed budgets and interrupted sleeps.
Startup regressions exercise the disabled and two-worker policies against the
actual main function. A separate development smoke uses real PostgreSQL behind
a framing proxy: it holds commit-boundary replies, disconnects committed writes,
checks exact row identities and no replay, then requires a fresh 64-connection
pool and read/write progress on both HTTP ports. This is a connection-failure
smoke, not a PostgreSQL crash or power-loss test.

Laptop loopback tests are development comparisons, not an official capacity
score. Parser and backpressure policies still need measurements on the scoring
machines; output draining can affect reactor fairness. PostgreSQL DNS/login/reconnect still runs synchronously
on its owning reactor. Two reactors on four available CPUs each initialize
and recover 32 PostgreSQL connections serially, compared with 16 per reactor
with four reactors. Slow or failing database handshakes can therefore delay
startup or block one reactor for longer; a 60-second startup is not guaranteed
for arbitrarily slow database peers. Allocation limits are per connection/lane
rather than a global memory quota. No host tuning or CPU pinning is performed
by the submission.
