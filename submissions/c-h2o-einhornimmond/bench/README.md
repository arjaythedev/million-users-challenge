# cload: the challenge's load generator in C

`cload` runs the same user loop as `bench/mload` (feed, think 3-7 s, one post from the feed,
think 3-8 s, like 15 %, write 2 %, idle 5-15 s, every wait times `-think-scale`) and writes a
result file with mload's schema, so `mload -merge` combines runs of both tools and the pass
criteria are the same: p95 < 500 ms, p99 < 1 s, failed requests < 1 %.

It started from raoz's `tests/loadgen.c` of the twelve-dollar challenge: one epoll loop per
thread and a min-heap of users per thread, one keep-alive connection per user, no allocation
while a user thinks. Added for this challenge: mload's flags and user loop, several target
hosts and ports, a per-request timeout, one retry of a GET on a keep-alive connection the
server closed, gzip (feeds are inflated with zlib to pick a post), chunked and
`Connection: close` bodies, per-endpoint and connect histograms, the 5-second timeline and
the JSON result.

```
make
./cload -target 10.0.0.2 -ports 80,81 -users 250000 -ramp 10m -hold 5m -gzip -think-scale 2.5 \
        -tokens ../../../seed/tokens.json -out run.json [-offset 250000] [-src 10.0.0.11,10.0.0.12]
```

A user costs about 200 bytes while it thinks and an 8 KB buffer while it waits, so memory is
not what limits one load box; source ports are. A local address has about 28,000 of them per
target address and port, which `-ports` (several target ports, ranges like `8000-8015` work)
and `-src` (several local addresses, bound with `IP_BIND_ADDRESS_NO_PORT`) multiply. The fd
limit is raised to the hard limit; set that high enough for the users of one process.

Measured locally against this submission, with server, cload and a Postgres with 64
connections on one 32-thread Ryzen 5950X (two ports, gzip, `-src 127.0.0.1,...,127.0.0.40`):

| users   | think-scale | req/s  | result                       | cload           | server              | Postgres   |
|---------|-------------|--------|------------------------------|-----------------|---------------------|------------|
| 5,000   | 1           | 495    | pass, p99 2.7 ms             | 7 MB            |                     |            |
| 500,000 | 2.5         | 21,000 | pass, p99 4.1 ms, 0 fails    | 0.8 cores, 54 MB | 1.9 cores, 1.05 GB | 4.1 cores  |
| 1,000,000 | 2.5       | 42,000 | pass, p99 9.5 ms, 0.02 % 503 | 1.9 cores, 102 MB | 4.6 cores, 1.76 GB | 9.7 cores |
| 1,000,000 | 2.5       | 42,000 | pass, p99 4.5 ms, max 105 ms, 0 fails (4 cores, loop-driven libpq, 4096-deep queue) | 1.9 cores | 1.7 cores (user 0.6, sys 1.1), 1.68 GB | |
| 1,000,000 | 2.5       | 42,000 | pass, p99 86 ms, 0.35 % 503 (4 cores, loop-driven libpq, 1024-deep queue) | 1.9 cores | 1.9 cores (user 0.6, sys 1.3), 1.66 GB | |
| 1,000,000 | 2.5       | 42,000 | fail, 5.9 % 503 (4 cores, old worker pool) | 1.9 cores | 3.4 cores (user 1.0, sys 2.3) | |
| 100,000 | 0.25        | 42,000 | pass, p99 5.4 ms (4 cores, loop-driven libpq) | 1.7 cores | 1.7 cores (user 0.6, sys 1.1) | |
| 100,000 | 0.25        | 42,000 | pass, p99 7 to 35 ms, 503 bursts (4 cores, old worker pool) | 1.9 cores | 3.3 cores (user 1.0, sys 2.3) | |
| 100,000 | 0.35        | 30,000 | pass, p99 6.8 ms             | 1.3 cores       | 3.5 cores           | 8.1 cores  |
| 100,000 | 0.25        | 42,000 | pass, p99 7.3 ms             | 1.7 cores       | 4.3 cores           | 8.7 cores  |
| 100,000 | 0.2         | 53,000 | pass, p99 47 ms              | 2.6 cores       | 6.0 cores           | 13.6 cores |
| 100,000 | 0.15        | 70,000 | 2 % 503 in the ramp, 0 in the hold | 3.5 cores | 7.0 cores           | 16.1 cores |

The 5,000-user run and an mload run with the same flags on the same server agreed on
req/s (495 vs 496), p95 (1.6 vs 1.5 ms) and p99 (2.7 vs 2.5 ms). The one-million run needed
the fd hard limit raised past the 524,288 that systemd gives a session by default
(`fs.nr_open` and `DefaultLimitNOFILE` in systemd's system.conf and user.conf, then a
reboot); one million users is then one cload and one server process, and two million
loopback sockets took about 11 GB of kernel memory. Above 50,000 req/s the local Postgres
is what saturates; the server's queue then answers 503 instead of waiting. An autovacuum
of the likes table doubled Postgres's CPU for a minute and produced 503s at 30,000 req/s.
The two 4-core rows pin the server with `taskset -c 0-3` (`SERVER_THREADS=4`) and cload to
other cores: the worker-pool version spent 1.4 cores in its 64 libpq threads (the blocking
send and recv per query, the wake-up of the loop), which moving libpq onto the loops removed. The 503s that remain on 4 cores come in bursts of
a few seconds while the local Postgres checkpoints (128 MB of shared buffers, 1 GB of WAL
between checkpoints; the challenge's database box has 8 GB and 16 GB): the loop's queue fills
and the server answers 503 rather than wait. With the queue at 4096 requests per loop instead
of 1024 the same run had no error at all and a p99 of 4.5 ms: the stalls became latency, the
worst 5-second window at 32 ms.

The port search of a connect() gets expensive when a (source address, target port) pair is
more than about half full: with 12 source addresses and 500,000 users the generator burnt
12 cores in the kernel and the run collapsed, with 40 it ran on one core.
