# Swift, io_uring, and a Postgres client written for this

| | |
|---|---|
| Language | Swift 6.4.1 in Swift 6 language mode, no Foundation, stdlib linked statically |
| Framework | none. io_uring through liburing 2.15 |
| Driver | my own PostgreSQL wire client: SCRAM-SHA-256, extended protocol, pipelining, binary rows |
| gzip | ISA-L 2.31.1 igzip at level 0, for bodies of 1 KB or more |
| Front | direct, on `PORT` and `EXTRA_PORT` |

There is one worker thread per CPU the process may use, and the workers share nothing: each has its
own rings, its own listening sockets, its own Postgres connections and its own buffers. There are no
locks on the request path because no two threads ever touch the same memory.

Most of the time on this challenge is spent waiting for Postgres, and that shaped everything else.
The server's job is to hand Postgres as little work as possible, in as few packets as possible, and
to get out of the way.

## Talking to Postgres

No Swift Postgres driver fits this design, so `Sources/Server/Postgres.swift` speaks the wire
protocol directly in about 500 lines. Startup is blocking and happens before a worker listens:
SCRAM-SHA-256 with PBKDF2 built on the same SHA-256 the JWT code uses, then one `Parse` for each
statement. After that a connection is just two buffers in a ring, and requests become `Bind` and
`Execute` bytes appended to a buffer.

The rows come back in binary. A `like_count` is four bytes, `id` is eight, and `created_at` is a
count of microseconds since 2000. Text formats would cost a parse on the Postgres side and another
one here. Dates turn into `2025-12-31T23:59:19.409Z` with integer arithmetic, truncated to
milliseconds as the spec asks, and rows go straight into the JSON buffer.

## Reads travel in packs

Sending each read on its own is the expensive way. Every read would get a `Sync`, a `write()` on the
Postgres side, a wakeup and a packet.

So all the reads that arrive during one pass of the event loop go into one pack on one connection,
up to 64, with a single `Sync` at the end. Postgres answers them back to back and flushes once. The
next pass takes whichever connection has the shortest queue. Every request still runs its own SPEC
query, and nothing is shared or remembered between them. This is the single biggest thing on this
page.

A pass waits for 256 completions or 2 ms, whichever comes first. A quiet worker therefore collects a
few requests before it wakes Postgres, and a busy one never sleeps at all.

## Writes wait for the commit

Writes use their own connections, and each has its own `Sync`, so each is its own transaction and a
failed like can't take a neighbour down with it. Postgres already merges concurrent commits into one
WAL flush, so there's no need to do that by hand.

`CommandComplete` is not a commit. It only says the statement ran. The response goes out at
`ReadyForQuery`, the first message that proves the transaction is over. A like on a missing post
comes back as SQLSTATE 23503, which becomes a 404, and `INSERT 0 0` in the command tag tells a
repeated like from a new one.

## Nothing waits for nothing

An HTTP connection has at most one request in flight. If a client pipelines, the later bytes stay in
its input buffer until the first response is done, so answers can't come back out of order. Rows
from Postgres can split across receives, so each connection builds its response in a scratch buffer
and copies it into the pass's output arena at `CommandComplete`.

Losing Postgres doesn't take the server down. Requests on the lost connection get a 500, and the
worker reconnects once a second. While I tested it, a Postgres restart produced a second or two of
500s and then normal 200s.

## A million sockets

Accepted sockets are io_uring direct descriptors, so they live in a ring's own file table. A ring
holds a little over a million of them, and a worker adds another ring when one fills up. An open
connection that isn't doing anything costs the server a small fixed slot and no buffers, because
receives share a ring of 2 KiB buffers that the kernel picks from.

Ubuntu's 6.8.0-136 and later have an inverted check when a provided-buffer ring is registered: they
refuse all-zero reserved words and accept a nonzero one. The server tries the correct way first and retries the wrong way if `EINVAL` comes back.

## Compression is cheap, and worth checking

Every request says `Accept-Encoding: gzip`. The feed is about 5 KB of JSON, and ISA-L at level 0
turns it into about 2 KB, which saves segments on the wire. Bodies under 1 KB
already fit in one segment, so they go out as they are.

## The Swift part

A request is a set of `ByteView`s pointing into the receive buffer, and there is no `String`
anywhere on the request path. Output goes into a `~Copyable` `OutBuffer` with one owner. The
JSON reader validates in one pass and hands out views. JWT verification is my own HS256, with the
HMAC pads hashed once at startup. It is built with `-Ounchecked`, and arithmetic uses the wrapping
operators.

The workers are plain pthreads started through a small C shim. Swift's concurrency runtime would be
a scheduler on top of the scheduler I already have, so it doesn't appear here at all.

## Where things live

| | |
|---|---|
| `Sources/Server/Ring.swift` | rings, accepts, receives, sends, the pass |
| `Sources/Server/Postgres.swift` | the wire client: startup, SCRAM, messages, connection state |
| `Sources/Server/Service.swift` | routing, read packs, writes, row decoding into JSON |
| `Sources/Server/HTTP.swift`, `JSON.swift`, `JWT.swift`, `SHA256.swift`, `Bytes.swift` | parsing, gzip framing, HS256, buffers |
| `Sources/Server/Entry.swift` | startup, one worker per CPU, connection split |
| `Sources/CUring`, `Sources/CShim` | liburing helpers, sockets, threads, the ISA-L call |
