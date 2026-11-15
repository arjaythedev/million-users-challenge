# Go - Nuclear Reactor, the fun one
A from-scratch server in pure Go: `epoll`, a hand-written HTTP/1.1 parser, a hand-written Postgres wire
client, and JSON and gzip written byte by byte.

**Stack:** Go 1.27.3 (pinned) and `golang.org/x/sys` for the syscalls. No other dependency and no cgo. The
Postgres protocol (SCRAM-SHA-256, extended query, pipelining) is implemented here, in `pgwire.go` and `pipe.go`.

Two numbers decide the benchmark: the memory an idle connection costs, and the CPU a request costs.

## How it works

`start.sh` runs a small supervisor that starts one worker per usable core: the same binary with `GOMAXPROCS=1`
and the GC off. Workers are separate processes, so they share no descriptor table, allocator or scheduler. Each
has its own `SO_REUSEPORT` listeners on both ports, its own epoll loop and its own Postgres connections, and
there are no locks and no goroutine per connection. A worker that dies is not replaced: the others keep serving.

One `GET /posts/42`:

1. The socket wakes the loop, edge-triggered. The bytes are read into the loop's one buffer and parsed in place.
2. The query goes into the loop's current pipeline: its own Bind and Execute on a shared Postgres connection. Up
   to 64 reads share one Sync, so one syscall carries many requests.
3. Result rows are decoded straight from the wire into JSON: the binary timestamp becomes
   `2025-01-01T00:00:34.152Z` by integer math.
4. The header goes in front of the body in the same buffer, and the response leaves in one `sendto`.

Nothing on that path allocates, so with the GC off there are no pauses.

## The pieces

| File | What it does |
|---|---|
| `main.go` | env config, the supervisor, worker wiring |
| `reactor.go` | the epoll loop, connection table, backpressure, idle sweep, panic containment |
| `uring.go` | the optional io_uring batch of client reads and writes |
| `httpparse.go` | request framing, method, path, the few headers that matter |
| `jwt.go` | HS256 signature and `exp` on every request, with no allocation |
| `bodyjson.go` | `{"body":"..."}` validation exactly as SPEC's error table demands |
| `encode.go` | headers and JSON, with SPEC's key order and timestamps |
| `gz.go` | the gzip encoder |
| `pgwire.go` | Postgres startup, SCRAM, prepared statements |
| `pipe.go` | pipelined reads, group commit, reconnects |

## Optimizations

- **12 bytes per idle connection.** One mmap'd, pointer-free table indexed by descriptor. Buffers belong to the
  loop, not to connections, so an idle connection holds none.
- **Pipelined reads.** Every request still runs its own SPEC query; they only share round trips. The pool is 64
  connections in all, far under the 512 allowed.
- **Group commit .** Writes queue up and go out as one implicit transaction on a free write connection.
  Responses are released only at `ReadyForQuery`, after the commit. Likes are sorted so concurrent batches can't
  deadlock. If a batch fails, its statements are replayed one by one, so each write gets its own answer, and a like
  on a missing post is still a 404.
- **Adaptive batching.** After a light iteration the loop sleeps so that one wake-up serves what would have been
  many. The pause steers each worker toward 32 events per wake-up, between 0.5 and 8 ms, so it follows the
  worker's own rate, and a busy loop never pauses.
- **One `io_uring_enter` per iteration.** Client reads and writes queue on a per-loop ring and run in a single
  syscall instead of one each. Sockets stay nonblocking and epoll still reports readiness. A kernel that refuses
  io_uring gets plain `read` and `sendto`, with nothing else changed.
- **Literal `LIMIT 20`.** A parameter hides the limit from the planner.
- **A gzip encoder that knows the response's shape.** The JSON writer records where each row's fields are, so
  each key is a direct back-reference to the same key one row up, with no searching. The Huffman tables are fixed
  at startup from English letter frequencies and JSON's structure, so nothing is counted per response. Post text
  gets a small LZ77 over word starts. This trades ratio for speed: the same feed is larger than `gzip -1` makes it.
  It is ordinary gzip, it works on any text, and it holds no data from any table or from a load test.

## Staying up

- A client that stops reading its responses stops being served, and reading pauses, so its memory stays bounded.
- Lost Postgres connections are redialed in the background. Requests on them get a 500, or a 503 on `/health`.
- A panic costs only the connection that caused it, as in `net/http`.
- `go test -fuzz` targets cover the HTTP parser, JWT verification, the body parser and the gzip encoder.

## Measured

On a shared dev box, not the benchmark boxes, with `mload -gzip` and 50,000 connections:

| Server cores | req/s in the hold | Server CPU per request | Errors | RSS |
|---|---|---|---|---|
| 4 | 40,000 | 34 us | 0 | 27 MB |
| 8 | 43,700 | 40 us | 0 | 44 MB |

500,000 idle connections on 4 workers: 50 MB of server RSS, about 3.4 KB of kernel memory per socket, and 1% of
a core while idle, with a p99 of 10 ms on requests to random connections.
