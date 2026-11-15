# rust-epoll: thread per core on raw epoll, hand-written HTTP/1.1 and Postgres wire client

| | |
|---|---|
| Language | Rust 1.94.0 (edition 2024), installed with rustup into `/opt/rust-feed` by `install.sh` |
| Framework / IO | none: raw `epoll` through `libc`, one event loop per core, no async runtime |
| Driver | none: `src/pg.rs` speaks the Postgres v3 protocol (trust, cleartext or SCRAM-SHA-256 auth; named prepared statements; Bind/Execute/Sync; binary timestamps) |
| Front | direct, on `PORT` and `EXTRA_PORT` (no Nginx) |
| Crates | `libc` 0.2.177, `hmac` 0.12.1 + `sha2` 0.10.9 (HS256 and SCRAM), `libdeflater` 1.26.1 (gzip; libdeflate is built from the crate's vendored C source), `memchr` 2.8.3 |

Versions are pinned with `=x.y.z` in `Cargo.toml` and in `Cargo.lock` (`cargo build --locked`).

## Running

```bash
sudo bash install.sh   # build-essential, curl, Rust 1.94.0 in /opt/rust-feed (idempotent)
bash build.sh          # target/release/feed: fat LTO, codegen-units=1, target-cpu=native
DATABASE_URL=postgres://feed:pw@db:5432/feed JWT_SECRET=million-users-challenge \
  HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

`build.sh` uses `/opt/rust-feed` when it exists, otherwise a rustup install in `~/.cargo`.

## Design

- **One thread per core.** The thread count is `available_parallelism()`, so it follows the CPU affinity mask
  and cgroup quota on the 8- and 4-core runs. Each thread owns:
  - an epoll set;
  - its own `SO_REUSEPORT` listeners on both ports (the kernel spreads new connections over the threads);
  - its own Postgres connections.

  Threads share nothing but read-only config: no locks or hand-offs on the request path.
- **Accept.** The listeners use `TCP_DEFER_ACCEPT`, so a connection reaches `accept()` with its first request
  already received. That request is read and served straight after `accept`, in the same pass.
- **Per-connection state is 32 bytes** in a per-thread slab:
  - descriptor, generation, flags, and two optional boxed buffers;
  - the buffers exist only while a request is split across reads, a pipelined request waits behind one in
    flight, or a response could not be written in full.

  An idle keep-alive connection costs no user-space buffer. The epoll key is slab index plus generation, so a
  late Postgres reply for a connection that closed (and whose slot was reused) is dropped, not misdelivered.
- **HTTP/1.1** is parsed in place from a per-thread 64 KB read buffer:
  - handled: request line, `Content-Length`, `Authorization`, `Accept-Encoding`, `Connection`,
    `Expect: 100-continue`;
  - keep-alive by default, never timed out;
  - pipelined requests are answered in order;
  - a client that half-closes still gets every response it asked for.

  Each response is built in one buffer and sent with one `write`.
- **A 2 ms batching window.** A loop pass lasts at least 2 ms from the moment `epoll_wait` returns; the rest of
  the window is slept after the pass's work. A busy thread therefore wakes about 500 times a second, and the
  requests that arrive in between share one wake-up and one Postgres round trip. An idle thread blocks in
  `epoll_wait`, so the first request after a quiet spell is served at once, and a pass that fills the event
  array skips the sleep, so the window never caps throughput. The cost is a few ms of latency against a 500 ms
  budget.
- **Postgres**: exactly the SPEC's statements (plus `SELECT 1` for `/health`), one per request.
  - Each statement is prepared once per connection. A request appends one Bind and one Execute to its
    connection's buffer, and each connection gets one `write` at the end of the pass.
  - There is no Describe, so a reply is just BindComplete, DataRows and CommandComplete.
  - Result formats are chosen per column:
    - `created_at` comes back binary (8 bytes), formatted as UTC and truncated to milliseconds.
    - `id` and `like_count` come back as text, which is shorter than their binary forms and is copied straight
      into the JSON.
  - JSON is written straight from the DataRow bytes in the receive buffer, with no result objects.
  - **Reads share a Sync.** A pass's reads (up to 64) go to the least-loaded read connection as one pack under
    one Sync: one write, one round trip and one flush from Postgres. Each read is answered at its own
    CommandComplete.
  - **Writes are group commits on separate connections.** A pass's writes (up to 128) go under one Sync, so
    they are one implicit transaction with one commit and one WAL flush. Writes go to an idle write
    connection when there is one, so a group never queues behind another group's commit. Every write in the
    group is answered only at that Sync's ReadyForQuery, i.e. after the commit (rule 8). Because writes have
    their own connections, a read never waits behind an fsync.
  - **If a write fails, its group is unwound and retried.** A like on a missing post fails the foreign key
    (SQLSTATE 23503, answered 404). Postgres then rolls back the whole group and skips the rest of it. The
    failing write gets its own error; every other write in the group is sent again alone, under its own Sync,
    so each commits or fails on its own.
  - Totals are 64 read + 16 write connections whatever the core count (`max(2, 64/threads)` and
    `max(1, 16/threads)` per thread): 80, well under the 512 limit.
- **Lost Postgres connections.** On EOF, an error, or a reply that makes no sense:
  - Its reads are re-sent on another connection of the thread, at most 3 times. Reads have no side effects.
  - Its writes answer 500, since their commit is uncertain. A write is never sent twice.
  - The connection is re-dialled, at most one blocking attempt per loop pass and one per second after a
    failure.
  - Keepalive and `TCP_USER_TIMEOUT` (60 s) turn a silent database link into an error instead of a hang.
- **JWT.** Every request with auth is checked in full: header `alg` must be `HS256`, then the signature, then
  `exp` (required), then `sub` and `username`. The HMAC key schedule is computed once per thread and cloned
  per token; this caches the key, not any verification. The signature compare is constant-time.
- **JSON input** goes through a strict RFC 8259 parser that validates the whole body, including UTF-8 and
  surrogate pairs. The post body is trimmed as JavaScript's `String.prototype.trim` does, and its length is
  counted in code points, as Postgres's `char_length` CHECK does.
- **gzip.** When the request accepts gzip, bodies of 1 KB or more are compressed with libdeflate level 1. In
  practice that is only the feed: 5.3 KB down to 1.9 KB.

## Rules notes

- **Queries:** the five SPEC statements, prepared, one per request. Pipelining and group commit are the
  allowed forms of batching.
- **No caching:** nothing is kept across requests except connections and prepared statements.
- **JWT:** signature and `exp` are checked on every request.
- **Writes:** answered only after their commit.
- **Connections:** at most 80 to Postgres.
- **Box tuning:** none. The only settings are on this process's own sockets (`SO_REUSEPORT`,
  `TCP_DEFER_ACCEPT`, `TCP_NODELAY`, and keepalive on the Postgres links).

## Measurements (local)

Fedora workstation, 16 threads (8 cores), kernel 7.3, Postgres 18.6 with `box/postgresql.conf` on the same
machine, with `shared_buffers` reduced to 2 GB for the workstation. The server was pinned to 4 CPUs with
`taskset -c 0-3`, `bench/mload` ran on the other 12, and every run started on a fresh copy of the seed.

The machine runs a firewall that inspects every new connection, loopback included. It held client SYNs in
SYN-SENT while the server side had none in SYN-RECV and empty accept queues. That caps new connections at a
few hundred per second, so these runs use few users with a short think time to reach scoring-like request
rates. They measure CPU per request and latency, not connection count.

| Users × think-scale | Hold req/s | Hold p95 / p99 | Errors | Server CPU |
|---|---:|---:|---:|---|
| 6,000 × 0.04 | 15,700 | 4.1 / 4.2 ms | 0 | 0.16 cores avg over the run, ~15 µs per request |
| 8,000 × 0.015 (60 s hold) | 54,800 | 4.2 to 16 / 4.5 to 45 ms | 0 of 4.39M requests | 0.55 cores avg over the run, ~13 µs per request; RSS 9.4 MB |

55k req/s is about the rate of 1.3M users at the scoring think time. The server used under a quarter of its
4 cores there: Postgres and the load generator on the same machine were the limits.

Correctness beyond `test/test.sh`:

- **Concurrent writes, every response checked against its own request.** 600 keep-alive clients sent likes
  (20% on a missing post, which forces group rollbacks and solo retries), creates and reads: 29,400 requests,
  0 mismatches. The rows in the database exactly matched the 201s returned (2,400 posts, 19,199 likes).
- **Postgres restarted under load** (3,000 users, ~3,200 req/s): the requests in flight at that moment (about
  1 s worth) answered 500; all 80 connections came back by themselves, and latency was normal again within
  10 s.
- **HTTP edge cases:** pipelined requests (including after the client half-closes), gzip output identical to
  the golden feed after decompression.
- **`test/run.sh`:** 42/42, both locally and in a clean `ubuntu:24.04` container after `install.sh` as root
  and the build as a normal user.

## Judgment calls

- A token without `exp` is rejected (rule 9: `exp` is checked on every request).
- Chunked request bodies, bodies over 64 KB and heads over 16 KB get `400` and the connection is closed
  (bodies over 16 KB are out of scope).
- Unknown methods on known paths answer 404.
- An id that is all digits but does not fit in 64 bits answers 400 (ids beyond 2^53 are out of scope).
