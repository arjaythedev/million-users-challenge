# cload: the challenge's load generator in C

`cload` runs the same user loop as `bench/mload` (feed, think 3-7 s, one post from the feed,
think 3-8 s, like 15 %, write 2 %, idle 5-15 s, every wait times `-think-scale`) and writes a
result file with mload's schema, so `mload -merge` combines runs of both tools and the pass
criteria are the same: p95 < 500 ms, p99 < 1 s, failed requests < 1 %. A 5,000-user run and
an mload run with the same flags agreed on req/s, p95 and p99.

It costs about 250 bytes per idle user instead of mload's 30 KB, so one process runs a million
users on about 100 MB and two cores: one epoll loop per thread, a min-heap of users per
thread, one keep-alive connection per user, no allocation while a user thinks. It handles
several target hosts and ports, a per-request timeout, one retry of a GET on a keep-alive
connection the server closed, gzip (feeds are inflated to pick a post), chunked and
`Connection: close` bodies, per-endpoint and connect histograms, a 5-second timeline and the
JSON result.

```
make
./cload -target 10.0.0.2 -ports 80,81 -users 250000 -ramp 10m -hold 5m -gzip -think-scale 2.5 \
        -tokens ../../../seed/tokens.json -out run.json [-offset 250000] [-src 10.0.0.11,10.0.0.12]
```

Source ports are what limit one load box: a local address has about 28,000 per target address
and port, which `-ports` (ranges like `8000-8015` work) and `-src` (several local addresses,
bound with `IP_BIND_ADDRESS_NO_PORT`) multiply. Keep each (source address, port) pair under
half full, or the kernel's port search gets expensive: a million users over two ports want
about 60 source addresses. The fd limit is raised to the hard limit; set that high enough for
the users of one process (systemd's `DefaultLimitNOFILE` and `fs.nr_open`).

## The measurement in README.md

Server, cload and Postgres on one Ryzen 5950X (16 cores, 32 threads): the server pinned with
`taskset` to 16, 8 or 4 cores and `SERVER_THREADS` set to the same, cload on the 16 SMT
siblings with `-src 127.0.0.1,...,127.0.0.60`, a Postgres 15 with 64 connections and 128 MB of
shared buffers unpinned. A million users at `-think-scale 2.5` are 42,000 requests a second,
the challenge's profile; the hold ran 5 minutes after a 7-minute ramp.

| cores | result | server | Postgres | RSS |
|---:|---|---|---|---|
| 16 | pass, p99 7.4 ms, max 157 ms, 0 errors | 1.84 cores (user 0.75, sys 1.09) | 4.1 cores | 1.9 GB |
| 8 | pass, p99 8.3 ms, max 119 ms, 0 errors | 1.90 cores (user 0.79, sys 1.11) | 4.7 cores | 1.8 GB |
| 4 | pass, p99 8.7 ms, max 112 ms, 0 errors | 1.51 cores (user 0.64, sys 0.87) | 3.8 cores | 1.8 GB |

The local Postgres, not the server, is what limits this box: above about 50,000 requests a
second it saturates and the server's queues answer 503. Its checkpoints (128 MB of shared
buffers, 1 GB of WAL between them; the challenge's database box has 8 GB and 16 GB) show as
the latency spikes of a few dozen milliseconds in the holds.
