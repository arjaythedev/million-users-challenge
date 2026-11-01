# Go 1.27: own epoll loops, pipelined Postgres wire protocol

Feed API submission for `trungdlp`.

| Component | Version |
| --- | --- |
| Go | 1.27.3, CGO disabled, Linux only |
| I/O | own epoll loop per core (`golang.org/x/sys/unix` v0.49.0), hand-written HTTP/1.1 |
| PostgreSQL | `pgconn` (pgx v5.11.0) connects, authenticates and prepares; the loop then speaks the extended protocol on the socket itself |
| JSON | responses written by hand from binary rows; request bodies and JWT claims decoded by github.com/bytedance/sonic v1.15.5 |
| Compression | own gzip encoder (`deflate.go`): greedy LZ77, fixed Huffman codes, bodies of 1 KB or more |

## Design

- **Loops**: one per `GOMAXPROCS` (Go reads the CPU affinity mask and cgroup limit), each
  on its own locked OS thread with its own epoll set and its own `SO_REUSEPORT` listener
  on both ports. A connection stays on the loop that accepted it; loops share nothing on
  the request path and take no locks. No goroutine per connection or per request.
- **Connection table**: one 8-byte slot per descriptor (generation, loop, kind, flags) in a
  table sized by `RLIMIT_NOFILE` (raised to its hard limit, capped at 16M). It holds no
  pointers, so the garbage collector never scans it; pages are touched only where
  descriptors exist. Buffers exist only while a request or response is split across
  reads or writes. The generation is bumped on close, so a result that arrives after its
  client left never reaches a new connection on the same descriptor.
- **HTTP**: requests are parsed in place from the loop's 64 KB read buffer. While a request
  waits for Postgres its connection is busy and later pipelined requests wait in order.
  A response is built in one buffer and sent with one `write`. Keep-alive connections are
  never timed out. Reads and writes on the non-blocking sockets are raw system calls,
  skipping the scheduler's syscall bookkeeping; only `epoll_wait` blocks.
- **Postgres**: each loop owns 4 connections (64 on 16 cores): 3 for reads, 1 for writes.
  `pgconn` connects, authenticates and prepares the SPEC's statements; the socket is then
  hijacked into the loop's epoll set. A request is one Bind+Execute of its prepared
  statement with binary parameters and results.
  - All reads of a loop iteration go to the least-loaded read connection and share one
    Sync: one write, one round trip, one implicit transaction.
  - Each write goes to a write connection with its own Sync, so it is its own transaction:
    a like on a missing post fails alone (`23503` -> 404), and the response is sent only
    after Postgres reports the commit (rule 8). A commit never delays a read.
  - A lost connection fails its queued requests with 500 (`/health`: 503) and is
    reconnected in the background.
- **Batching window**: after an iteration with fewer than 32 events, the loop sleeps 2 ms
  (`nanosleep` on its own thread; `time.Sleep` needed another thread to wake it: 6% more
  CPU per request at 7k req/s per loop).
  The next iteration serves everything that arrived meanwhile, and its reads share one
  round trip. A busy loop never sleeps, so the window costs latency, not throughput.
- **gzip**: one hash probe per position for LZ77 matches, coded with DEFLATE's fixed
  Huffman tables. Building dynamic tables was most of the cost of compressing a 5.3 KB
  feed (klauspost level 1: 14 µs, 1.9 KB). Fixed codes take 7 µs for 2.3 KB, still two
  TCP segments. Only bodies of 1 KB or more are compressed (in practice `/feed`).
- **JSON**: post objects are written straight from the binary row (int8, text,
  timestamptz, text, int4); `created_at` is formatted from the microsecond count
  (civil-from-days), truncated to milliseconds. Strings escape
  quotes, backslashes and control characters; non-ASCII stays UTF-8.
- **Validation**: unchanged from the previous version: HS256 signature, required numeric
  `exp` and payload types checked on every request, strict JSON request bodies, the
  SPEC's id and body rules. A panic in validation answers 500 for that request only.
- Reference SQL, one statement per request. No response, query-result or JWT caching.
- `start.sh` sets a soft Go memory limit to 75% of host RAM unless already set.

## Run

On the clean Ubuntu 24.04 x86_64 benchmark box, run `install.sh` as root once.
It installs Go 1.27.3 from the official archive after checking its SHA-256.
`build.sh` runs as the normal user; `start.sh` runs in the foreground.

```bash
bash submissions/go-epoll-trungdlp/build.sh
DATABASE_URL=postgres://feed:password@localhost:5432/feed \
JWT_SECRET=million-users-challenge HOST=0.0.0.0 PORT=3000 EXTRA_PORT=3001 \
  bash submissions/go-epoll-trungdlp/start.sh
```

Only `DATABASE_URL` and `JWT_SECRET` are required. Defaults: `HOST=127.0.0.1`
(IPv4), `PORT=3000`; `EXTRA_PORT` enables the second listener. The Postgres link must be
plain TCP (the box has `ssl = off`).

## Validation

```bash
cd submissions/go-epoll-trungdlp
go test -race ./...        # parser, wire format, JSON, routing and validation
GOOS=linux go vet ./...
cd ../..
bash seed/make-seed.sh
bash test/run.sh submissions/go-epoll-trungdlp   # on Linux
```

`cmd/checker` opens many keep-alive connections and checks that every `GET /posts/:id`
response carries the requested id, in order, with pipelined requests, requests split
across writes, and connections closed while their statement is in flight:
`go run ./cmd/checker -addr 127.0.0.1:3000 -conns 1000 -d 40s`. Latest run: 4.8M
responses, 0 mismatches, 0 errors, 187k mid-request closes, 21k slow readers (4 KB
receive buffer, 100 pipelined requests). With every Postgres backend terminated twice
during a run, in-flight requests get 500, the connections come back within a second,
and no request is left unanswered. A `-tags debug` build logs each loop's state on
SIGUSR1.

42/42 official checks pass for linux/arm64 and for linux/amd64 (x86_64 emulation on an
arm64 Mac), each in a container against Postgres 18.

## Measured locally

Docker on an Apple M4 Pro, linux/arm64 containers on one bridge network: server on 2
pinned vCPUs, `bench/mload` on 6, Postgres 18 on 6. `mload -gzip -think-scale 0.1`
(the challenge's request rate per user times 25, so fewer connections reach the same
request rate), 30 s ramp, 90 to 120 s hold. Server CPU per request is from its cgroup.

| Build | Users passing | CPU per request | p95 / p99 at 30k users | RSS at 30k users |
| --- | ---: | ---: | ---: | ---: |
| nbhttp + `pgxpool` | 30,000 | 48 µs | 164 / 286 ms | 179 MB |
| nbhttp + pipelined `pgconn` | 70,000 | 32 µs | 13 / 20 ms | 154 MB |
| own epoll loops | 80,000 | 16 µs | 7.5 / 8.3 ms | 34 MB |
| + own gzip encoder | 80,000 | 13 µs | 7.3 / 8.0 ms | 32 MB |
| + own timestamp formatting | 80,000 | 12 µs | 7.2 / 7.9 ms | 32 MB |

- The current build at 80,000 users: 73k req/s, 13 µs per request, p95 25 ms, p99 74 ms,
  0 errors.
  At 90,000 users the load generator was at 69% of its CPUs while the server used about
  two thirds of its 2: the local setup set that ceiling before the server did.
- 24 read + 8 write connections per loop instead of 3 + 1: 18 µs per request and 50% more
  Postgres CPU (smaller batches). No batching window: 21 µs per request; 4 ms: 17 µs.
- At the per-loop rate of the challenge's 8-core runs (~7k req/s per loop: 70,000 users,
  `-think-scale 0.5`): 15.5 µs per request, p95 6.0 ms, p99 7.6 ms.
- **200,000 connections** (two load generators of 100,000 users, `-think-scale 2.5`,
  120 s ramp, 60 s hold): 6.2k req/s, p95 9.0 ms, p99 10.3 ms, max 26 ms, 0 errors.
  Server RSS 30 MB with 200,020 descriptors open, 0.17 cores. The server's listen
  queues never overflowed (`ListenOverflows=0`, `ListenDrops=0`). A 60 s ramp failed
  with 2 to 4 s connect times, but only while both load generators were at 100% of
  their CPUs; the server's counters showed no drops. (The Docker VM's conntrack table,
  262,144 entries, kept this test under that size.)
- The other runs use far fewer connections than the challenge (tens of thousands, not
  millions), so they show CPU per request, not the memory of millions of idle sockets.
