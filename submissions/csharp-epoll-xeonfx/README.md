# C# epoll

C# on .NET 10 (10.0.402), no web framework and no database driver: an HTTP/1.1 server on raw epoll and a Postgres
client on the wire protocol, both in this folder. Linux only. One package: System.IO.Hashing 10.0.13, for gzip's
CRC-32.

## How it works

- **Independent event loops.** Each loop thread has its own listening sockets on both ports (SO_REUSEPORT lets the
  kernel spread connections over them), its own clients and its own Postgres connections, one loop per core. Loops
  share nothing, so there are no locks and no thread hops between reading a request and writing its answer.
- **epoll, level-triggered, called directly.** `accept4`, `recv` and `send` go straight to libc; `recv` and `send`
  skip the GC transition because the sockets never block. A connection is a small struct in an array indexed by its
  fd, so a million idle connections cost about 40 bytes each plus the kernel's socket buffers.
- **No allocations per request.** Requests are parsed in place from one receive buffer per loop. Responses are
  written into one buffer per loop, the body first and the head just before it, so each response leaves in one
  `send`. Only a request that arrives in pieces or a response the socket can't take at once borrows a pooled buffer.
- **A loop works in passes of 2 ms.** A pass serves everything that is ready, then the loop sleeps out the rest of
  the 2 ms before it looks again. At a million users a loop otherwise wakes for nearly every request and every
  Postgres reply (2 ms passes cut 56,000 context switches/s to 4,400 at 1.4M users). One wake-up serves all that
  arrived meanwhile. A pass that fills its event list goes straight on. The cost is a few milliseconds a request,
  well inside the 40 ms after which the kernel would ack the request in a packet of its own.
- **Postgres: a pass's reads are one batch.** They go to one read connection as Bind/Execute pairs under a single
  Sync: one packet out, one wake-up of one backend, one ReadyForQuery back. Past 64 reads the next connection takes
  over. A batch goes to a reader with nothing else waiting, and the readers take turns: eight of them on a quiet
  network, so Postgres keeps few backends warm, and more as batches take longer. Each read is answered as soon as
  its rows are in; if one fails, the reads after it in the batch are sent again. The spec's statements are prepared
  once and their parameters are binary.
- **Fewer bytes from Postgres.** Its replies are most of what the app box takes in, and the box's network runs out
  before its CPU does. So each column of a post comes in whichever format is shorter: `id` and `like_count` as text
  (six digits and one or two, against 8 and 4 bytes binary), `created_at` binary (8 bytes against 29) and formatted
  straight from microseconds. The digits go into the JSON as they are.
- **Group commit for writes, on write connections of their own.** A pass's new posts and likes go out together as
  one transaction (one Sync, one commit) on a write connection that has no group out (8 a loop on 4 cores, 4 on 8,
  2 on 16). While all of them wait for their commit, new writes queue and leave as one group with the pass that
  finds one free. So a commit that is slow, or whose packet was lost, holds up its own group and nothing behind it,
  and reads never wait for a commit. If a write fails (a like on a missing post), it gets its error and the rest
  of its group are sent again.
- **Built for a network that loses packets.** Near its limit the scoring box's network drops packets, and what gives
  way first is the rows coming from Postgres: failed runs on boxes like it (nish-std-amritk's) and runs with packets
  dropped on purpose (below) both show reads taking seconds while writes still take 220 ms.
  On a pipelined connection one lost packet holds up every read behind it. A packet with none after it (the last
  of an exchange, or one that was lost again when it was resent) waits out a timer of 200 ms that doubles. And TCP
  at Postgres's end shrinks the connection's window at every loss, so a few connections that carry everything stop
  carrying it. This is what keeps the reads moving:
  - *Nudges.* A connection that owes replies and has been silent for 5 ms (20 ms for a writer: a commit waits for
    its WAL flush) gets a lone Sync, and another every 5 ms while it stays silent, four of them, then less often,
    down to one every 80 ms. Each reaches Postgres past the hole, or its ReadyForQuery reaches the server past it,
    and the side that sent the lost packet resends it at once, where the timer would take 200 ms. On a quiet
    network none are sent, so it costs no packets there.
  - *Readers by the hundred.* 480 connections in all, under the 512 of rule 5: 112 readers a loop on 4 cores, 56 on
    8, 28 on 16. A quiet network uses eight a loop. Under loss a batch takes longer, more are out at once, and each
    is on a reader of its own: a reader that has gone silent on what it owes is never given a batch while another
    answers.
  - *Smaller batches while it lasts.* When more than 1 % of the reads have waited 350 ms or more (through more than
    one timer), the batch size halves, each quarter of a second, down to 4: more batches, each a shorter stream
    with fewer packets to lose and fewer reads behind them. When fewer than 0.25 % have, it doubles again, back to
    64, because fewer and fuller packets are what a healthy network wants. It also doubles while nine readers in
    ten are busy: more batches need more readers.

  No statement runs twice for this: a read is sent once, on one connection, and waits for it. A Sync is not a
  query. Every socket also uses `TCP_THIN_LINEAR_TIMEOUTS` to limit timeout backoff for small amounts of data in
  flight.
- **Fewer packets per client request.** Clients inherit their TCP options from the listener, with no call per
  connection: `TCP_NODELAY`, `TCP_THIN_LINEAR_TIMEOUTS`, `TCP_DEFER_ACCEPT` and delayed-ACK mode (`TCP_QUICKACK`
  off). With `TCP_DEFER_ACCEPT` a connection is handed over once its first request is in, and that request is read
  in the same pass as the accept. Delayed-ACK mode lets the ACK of a request ride on its response; without it the
  kernel acks a request that follows a user's idle seconds in a packet of its own. The kernel leaves the mode when a
  response takes more than 40 ms, so a response that took over 25 ms turns it back on.
- **JWT checked on every request** with HMAC-SHA256 on the stack: signature first, then HS256 (the usual header's
  exact bytes need no decoding), exp, sub and username.
- **The feed is gzipped** when the request's Accept-Encoding takes gzip. Only a body that wouldn't fit in one TCP
  segment with its head is worth it, and that is the feed: about 3.6 KB, three segments, one gzipped. `Gzip.cs` writes
  it without allocating: one deflate block with the fixed Huffman codes and greedy matches from a hash table that is
  never cleared (each call's positions sit past the last call's). About 4 µs a feed on an M1, where zlib-ng takes 9,
  ISA-L (level 0) 6 and libdeflate (level 1) 12, because at this size their setup costs more than the compression.
- NativeAOT, workstation GC without background GC, invariant globalization. `FEED_JIT=1 ./build.sh` builds the JIT
  version instead (start.sh runs whichever is built).

Login to Postgres is SCRAM-SHA-256 (or trust). A dropped database connection fails what it owed with 500 (503 for
`/health`) and reconnects after a second; when many drop together they come back 10 ms apart, the writers first.

## Measured with packets dropped on purpose

Not the scoring box: GitHub's 4-core runners (Ubuntu 24.04), Postgres 18 in a container on a bridge, so the link has
a TCP stack of its own at the far end, with 0.1 ms added each way and no segmentation offload; mload on loopback.
Packets are dropped at random where they arrive. "In" drops that share of what Postgres sends the server and of what
the clients send it, as a box whose receive side overflows would; "each way" drops it in both directions on both
links. One loop with the connections a 4-core box gives it (112 readers, 8 writers), 16,000 users at
`-think-scale 0.25` (6,200 requests/s), 15 s ramp, 60 s hold, a fresh database each run. p95 / p99 in ms; a run
passes as the challenge's do (p95 under 500 ms, p99 under 1 s).

| dropped | #114 (8 readers, nudges after 50 ms) | this |
|---|---|---|
| none | pass, 5 / 6 | pass, 5 / 6 |
| 5 % in | pass, 214 / 234 | pass, 210 / 220 |
| 10 % in | fail, 5,167 / 7,542 | pass, 218 / 425 |
| 15 % in | fail | pass, 227 / 639 |
| 18 % in | fail | pass, 425 / 845 |
| 20 % in | fail | fail, 621 / 1,296 |
| 5 % each way | pass, 234 / 434 | pass, 216 / 417 |
| 10 % each way | fail, 6,181 / 7,393 | pass, 325 / 713 |
| 12 % each way | fail | fail, 429 / 1,041 |
| 15 % each way | fail | fail, 621 / 1,712 |

Where #114 fails, its reads take seconds and its likes still take about 240 ms. At 28,000 users on the one loop
(10,800 requests/s) this passes 10 % in at 218 / 425 and 15 % in at 232 / 639, where #114 fails both. With four
loops it passes 15 % in at 225 / 633, where #114 fails at 2,549 / 3,758.

On a quiet network it costs nothing: 4.46 packets a request against #114's 4.47 at this load, 4.36 against 4.36 at
10,800 requests/s, 4.93 against 4.93 with four loops, and 8.21 against 8.26 at the scoring run's think time
(`-think-scale 2.5`, where the clients' keep-alive probes and their answers are a third of the packets), with the
same latency.

## The code

- `Program.cs` starts the loops; `Loop.cs` is the loop itself and its passes, `Loop.Http.cs` its clients
  (accept, parse, respond), `Loop.Routes.cs` the routes, `Loop.Postgres.cs` the reads (which reader takes a batch,
  how many reads it holds), write groups and database connections.
- `Pg.cs` is one Postgres connection on the wire protocol, `Scram.cs` its login.
- `Json.cs` writes the responses and reads a new post's body, `Gzip.cs` compresses the feed, `Jwt.cs` checks tokens,
  `Native.cs` holds the libc calls.
