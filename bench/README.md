# The load generator

`mload` is what I score with. One simulated user is one goroutine with its own keep-alive connection, doing
what a person scrolling the feed does:

    GET /feed -> think 3-7 s -> GET /posts/:id (one from the feed) -> think 3-8 s
      -> like it (15%) -> write a post (2%) -> idle 5-15 s -> repeat

Scoring runs use `-think-scale 2.5` (every think and idle time × 2.5, so a user loads a page about every 24 s)
and `-gzip` (`Accept-Encoding: gzip`, like a browser). At a million users that's about **42,000 requests per
second**, about 5% of them writes. Users ramp in over 60 s or more (at most ~3,300 new connections per second),
then the hold runs for 5 minutes. Every request in the run counts toward the result.

**Pass**: **p95 < 250 ms**, **p99 < 500 ms**, **p99.9 < 1,000 ms**, **p99.99 < 2,000 ms**, failed requests **< 0.1%**
(status >= 400, or no response within 60 s). The latency percentiles are over successful requests only. p99.999 is
reported, not judged.

Why the tail limits: a stall in the server (a stop-the-world GC pause, a blocked event loop, a lock convoy) delays
every request in flight at once. With a million users that's tens of thousands of requests per second of stall, yet
a few such stalls fit under a p99 limit: 1% of a 10-minute run is 6 s. p99.99 allows 0.01%, 60 ms, so a single stall
longer than ~2 s fails, while a few unrelated slow requests (a retransmitted SYN, a network hiccup) don't. The
result files keep the full histogram, so `mload -merge` re-judges old runs without running them again.

Why 0.1% errors, and successful requests only in the latency: at a million users (~42,000 req/s), 1% errors is 420
failed requests every second of the run. And when failed requests count in the latency too, a server can answer an
instant 503 instead of a slow 200: it spends error budget and lowers its percentiles at the same time. Now a failed
request only counts as an error, and 0.1% (99.9% of requests succeed) leaves little room for shedding load.

Why p95 < 250 ms and p99 < 500 ms: each limit is half the next one (250 ms, 500 ms, 1 s, 2 s), so each one checks
something. With p99 < 1 s next to p99.9 < 1 s, the p99 limit would never decide a run, since p99 <= p99.9.

## Build

```bash
cd bench/mload && go build -o mload .      # Go >= 1.27
```

## Load test your own server

```bash
bash seed/make-seed.sh                      # for seed/tokens.json
./bench/mload/mload -target 127.0.0.1 -ports 3000,3001 -users 5000 -ramp 60s -hold 2m \
  -gzip -think-scale 2.5 -tokens seed/tokens.json -out run.json
```

It prints one line per 10 s while it runs and a pass/fail summary at the end. A laptop runs out of source ports
(and file descriptors) long before a real server runs out of anything: past ~25k users per target IP and port,
use more IPs (`-target a,b,c`) or more machines.

## Several load generators

Each machine runs its share of the users with its own `-offset` (so they use different tokens) and the same
`-start` (unix ms, so they ramp together), then merge:

```bash
# on every load generator, e.g. 3 machines x 333,334 users against 4 server IPs and both ports
mload -target 10.0.0.2,10.0.0.3,10.0.0.4,10.0.0.5 -ports 80,81 -users 333334 -offset 0 \
      -start 1767225600000 -ramp 5m -hold 5m -gzip -think-scale 2.5 -tokens tokens.json -out lg1.json
# then, anywhere
mload -merge -out total.json lg1.json lg2.json lg3.json
```
