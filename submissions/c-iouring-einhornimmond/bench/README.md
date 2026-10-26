# cload: the challenge's load generator in C

The same `cload` as in `c-h2o-einhornimmond/bench`, described in
[its README](../../c-h2o-einhornimmond/bench/README.md): mload's user loop and result schema
(works with `mload -merge`), about 250 bytes per idle user, a million users in one process on
about 100 MB and two cores.

```
make
./cload -target 10.0.0.2 -ports 80,81 -users 250000 -ramp 10m -hold 5m -gzip -think-scale 2.5 \
        -tokens ../../../seed/tokens.json -out run.json [-offset 250000] [-src 10.0.0.11,10.0.0.12]
```

## The measurement in README.md

Server, cload and Postgres on one Ryzen 5950X (16 cores, 32 threads): the server pinned with
`taskset` to 16, 8 or 4 cores and `SERVER_THREADS` set to the same, cload on the 16 SMT
siblings with `-src 127.0.0.1,...,127.0.0.60`, a Postgres 15 with 64 connections and 128 MB of
shared buffers unpinned. A million users at `-think-scale 2.5` are 42,000 requests a second,
the challenge's profile; the hold ran 5 minutes after a 7-minute ramp. CPU over 200 s of the
hold. The h2o rows are the same runs against `c-h2o-einhornimmond`.

| cores | server | result | server CPU | Postgres | RSS |
|---:|---|---|---|---|---|
| 16 | io_uring | pass, p99 30 ms, max 413 ms, 0.009 % 503 | 1.78 cores (user 0.60, sys 1.18) | 5.3 cores | 431 MB |
| 16 | h2o | pass, p99 7.4 ms, max 157 ms, 0 errors | 1.84 cores (user 0.75, sys 1.09) | 4.1 cores | 1.9 GB |
| 8 | io_uring | pass, p99 9.0 ms, max 114 ms, 0 errors | 1.36 cores (user 0.48, sys 0.88) | 4.0 cores | 238 MB |
| 8 | h2o | pass, p99 8.3 ms, max 119 ms, 0 errors | 1.90 cores (user 0.79, sys 1.11) | 4.7 cores | 1.8 GB |
| 4 | io_uring | pass, p99 9.2 ms, max 83 ms, 0 errors | 1.12 cores (user 0.40, sys 0.72) | 3.4 cores | 150 MB |
| 4 | h2o | pass, p99 8.7 ms, max 112 ms, 0 errors | 1.51 cores (user 0.64, sys 0.87) | 3.8 cores | 1.8 GB |

The worst 5-second window of the io_uring holds had a p99 of 286 ms (16 cores, the Postgres
stall that also made the 503s), 41 ms (8) and 15 ms (4).

The local Postgres, not the server, is what limits this box: above about 50,000 requests a
second it saturates and the server's queues answer 503.
