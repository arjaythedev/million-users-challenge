# CPU headroom and compression

The maintainer's [official benchmark](https://github.com/arjaythedev/million-users-challenge/pull/43#issuecomment-6567820284) applies to `117e287`. The 16-core search stopped at its one-million-user cap. The smaller-core searches held more than 1.2 million users before the app host's network ceiling stopped the search. Spare CPU does not remove a bandwidth limit.

The server has no CPU-percentage cap. Its default two HTTP loops and 16 database workers can wait for sockets or database results while CPU remains available. More workers help only when their queues are the bottleneck.

## Concurrency investigation

The app ran on the eight-vCPU Ubuntu 24.04.5 host. PostgreSQL 18.6 and the unmodified load generator shared the separate ten-vCPU Ubuntu 26.04 host. Runs used fresh seed clones and durable writes. A 24,000-user stress profile used a five-second ramp, a 25-second hold, compressed think times of 0.01, and gzip. It reproduces a latency failure with spare app CPU, rather than an official connection-capacity score.

Increasing the pool from 16 to 32, then increasing write workers from two to four, then HTTP loops from two to four did not lift throughput. Each step changed one setting. PostgreSQL used about seven CPU cores in the instrumented runs. No app thread was pinned at a full CPU in the baseline. These findings do not establish the cause of the official network ceiling; they show why increasing concurrency is not a verified improvement on this lab.

## Spending CPU to reduce outbound traffic

`GZIP_LEVEL` accepts 0 through 3. Level 1 is the default since the 2026-11-06 follow-ups in [BENCHMARKS.md](BENCHMARKS.md); it trades app CPU for smaller responses on bandwidth-limited deployments. The server lowers the level toward 0 while its CPUs stay busy and raises it again when they have room (see the README). Level 0, the level of the official runs at `117e287` and `6d93a5e`, remains available:

```sh
GZIP_LEVEL=0 ./start.sh
```

ISA-L level 1 compressed the same 5,102-byte seeded feed from 2,243 to 1,934 bytes, a 13.8% reduction in the gzip body. All four levels decoded byte-for-byte to the original body. The standalone compression probe ran 10,000 iterations per level. Level 1 used roughly twice the compression time of level 0 for this fixture; this is compression time alone, not whole-request CPU.

After building the submission, reproduce the fixture probe from the submission folder:

```sh
curl --fail --silent http://127.0.0.1:3000/feed -o /tmp/zig-feed.json
cc -O3 -std=c11 -Wall -Wextra -Werror -Ideps/isa-l-2.31.1/include \
  tests/compression_probe.c deps/isa-l-2.31.1/bin/isa-l.a -o /tmp/zig-compression-probe
/tmp/zig-compression-probe /tmp/zig-feed.json
```

In matched 8,000-user screens, level 1 used more app CPU and about 13.3% fewer sampled outbound bytes per request. NIC counters include database protocol traffic, so that normalization is approximate. Both levels passed the latency/error thresholds with zero failed requests. Level 1 also passed with the app restricted to CPUs 0 through 3. The kernel and database retained all their host CPUs.

Each load configuration ran once. The local request-rate differences do not establish a performance ranking. The official network-bound rig has not run this setting, so these results do not claim a new maximum user count.

## Validation

Both hosts built the source with Zig 0.13.0 as an unprivileged user. The 42-check API suite and repeated gzip/plain comparisons passed for the default and levels 1 through 3. Default compression and level 1 passed freshness, 96 concurrent writes, contract edges, buffer/reset checks, and HTTP transport regressions. Level 1 passed those suites with `DB_POOL=1` on both hosts. The second host also passed them with a verified partial-send/EAGAIN interposer. Invalid compression levels were rejected before startup. Pinned formatting checks passed.

The second build reused the pinned static dependency objects from the first build. Both hosts produced the same application executable hash.

Compression workspaces are allocated lazily per worker and reused across responses. Allocation failure falls back to level 0. Default concurrency, SQL, authentication, and write durability are unchanged. [compression-results.json](compression-results.json) records source/executable hashes, individual runs, CPU units, validation cases, and measurement limits. Earlier validation and benchmark files retain their original revision attribution.
