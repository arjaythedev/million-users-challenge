# Findings and decisions

What was decided, what it was measured with, and what was tried and dropped. The settings themselves are listed in
[README.md](README.md). [RESEARCH.md](RESEARCH.md) is the survey of the other servers it started from.

The server is a port of our [multicore submission](https://github.com/arjaythedev/multicore-server-challenge/pull/77)
(same stack: Ember, skunk, Cats Effect) to this challenge's API and rules. Its FINDINGS.md has the measurements that
carry over (session pool, session settings, telemetry off, listen backlog, idle timeout).

## What is different here

| | Multicore | Million users | Consequence |
|---|---|---|---|
| Postgres | same box | its own 16-core box | all cores serve HTTP: one Cats Effect compute thread per core |
| Connections | ~10,000 users | up to 1,000,000 keep-alive connections, no Nginx (rule 11) | heap per idle connection decides the score |
| Ports | one | 80 and 81 at the same time | two Ember servers in one process, sharing the app and the sessions |
| Load | think time x0.2 locally | a page every ~24 s per user | many idle connections, few requests per connection |
| GC | Parallel won (CPU-bound score) | live set of many GB that keeps changing | generational ZGC |

## The design constraint: heap per idle connection

**Method.** 20,000 connections (2 addresses x 2 ports x 5,000), each idle after one request. `jcmd
GC.class_histogram` (it runs a full GC) before and after, divided by the number of connections. JDK 27, ZGC.

| After one request to | Live heap per idle connection |
|---|---|
| `/health` | 38.1 KB |
| `/feed` (gzipped) | 38.1 KB |

The same after both, so no response is kept. The histogram splits it like this:

| What | Per connection |
|---|---|
| `byte[]`: fs2's socket read buffer (8 KB, allocated with the socket, kept while it is open) | 8.6 KB |
| Cats Effect: 10 fibers, their stacks and suspended `IO` continuations | 13.8 KB |
| fs2: 21 scopes, 18 interrupt contexts, suspended `Pull` continuations | 10.0 KB |
| atomics, `Some`, `Chain`, http4s, the rest | 6.4 KB |

**Consequence.** 38 KB x 1,000,000 is ~38 GB, more than the box's 32 GB. The 24 GB heap holds at most ~630,000 idle
connections before any request garbage. The expected limit is **~500,000-600,000 users, set by heap, not by CPU**.

**What did not change it.** Ember has no setting for its per-connection structure: each connection is an inner
stream of `parJoin` inside interruptible fs2 scopes, and fs2 always allocates the 8 KB read buffer. fs2 no longer
lets a server plug in its own socket type. The settings that keep it from growing are kept: no idle timeout (a
finite one races a timer against every read), an 8 KB read size (Ember's default is 256 KB), and fiber tracing off.

**What would change it.** A connection loop that holds no buffer and very few fibers while the connection is idle:
for example one fiber per connection that waits until the socket is readable and only then takes a buffer. That can
keep http4s's `HttpApp`, but it replaces Ember's connection handling. It is a different server, not a setting, and is
not built here.

## Decisions

| Decision | Why | Evidence |
|---|---|---|
| Generational ZGC, heap 75% of RAM, pre-touched | Gigabytes of connection state that keep changing; the score is on latency percentiles | Our [GC showcase](https://github.com/sideeffffect/multicore-server-challenge/blob/gc-showcase/experiments/gc-showcase/README.md) on the same stack with 100,000 idle-heavy connections (~3.5 GB of connection state): Parallel had full GCs up to 1.7 s (p99.9 2.1 s), G1 pauses up to 155 ms, ZGC p99 28 ms and p99.9 110 ms at ~40% more CPU than Parallel. Here the live set is ~7x larger |
| 25% of RAM left outside the heap | The kernel needs ~3 GB for a million TCP sockets, plus the JVM's own memory | Postgres is on the other box, so nothing else needs it |
| Two Ember servers in one process | Rule 11: ports 80 and 81 at the same time | They share the routes and the session pools, so one set of 160 database sessions serves both |
| 128 read + 32 write skunk sessions (160 of the 512 allowed) | One statement at a time per skunk session; a burst of reads must not queue the writes | Writes are ~5% of requests |
| Own session pool (`Queue` of long-lived sessions) | skunk's `Session.pooled` runs `UNLISTEN *` and `RESET ALL` on every return: two extra round trips per request | Carried over from multicore, where it was measured |
| One statement per request, in autocommit, SPEC.md SQL verbatim | Rule 3 asks for the SPEC.md queries, one per request | No rewrites, unlike multicore |
| No batching of reads or writes | Merging reads (multicore's `Batcher`) changes the SQL; ~2,000 inserts/s at a million users don't need group commit, and Postgres already shares one WAL flush among commits that arrive together | Rule 3 allows batching writes; not needed at this write rate |
| gzip level 1, bodies of 1 KB or more | The feed is 3.6 KB as JSON and 0.75 KB gzipped; at ~42,000 req/s that matters for the network | `Gzip.scala` compresses the whole body in one call with a reused per-thread `Deflater` and keeps `Content-Length`. http4s's `GZip` middleware streams each body through new 32 KB buffers with chunked encoding |
| Listen backlog 65,535 | When the accept queue overflows during the ramp, each dropped SYN costs the client a 1-3 s retransmit | The box sets `somaxconn` to 65,535; fs2 binds with Java's default of 50 |

## Build and start (scala-cli)

- **scala-cli instead of sbt.** One native binary instead of sbt's launcher and its own Scala, a single
  `project.scala` with `//> using` directives instead of `build.sbt` + `project/`. `build.sh` compiles with
  `scala-cli compile --server=false` (no Bloop server left running) and writes the class path to
  `target/classpath.txt`.
- **`start.sh` starts the server with `scala-cli --power run --server=false --offline --scala 3.10.0 --java -cp
  "$(cat target/classpath.txt)"`.** scala-cli replaces itself with the JVM (checked: the same PID), so the server
  keeps its PID. Measured and rejected on the way:

| Tried | Result |
|---|---|
| `run` with the sources as inputs | With `--server=false`, scala-cli compiles again on every start: ~7 s on a laptop, much more on a slow box |
| `run` with a packaged jar as input | scala-cli 1.18.0 compiled the `.jar` as if it were Scala source and failed. An assembly jar would also be 55 MB and can break `META-INF/services` files |
| JVM flags in `JDK_JAVA_OPTIONS` or `JAVA_TOOL_OPTIONS` | scala-cli reads them as build options, or passes them to its compiler JVM too |
| `--offline` without `--power` | Rejected as an unknown argument when power mode isn't on in the global config (CI) |
| `--offline` without `--scala` | `run` loads the compiler of scala-cli's default Scala (3.9.0) even with nothing to compile; it isn't in a fresh cache, and `--offline` can't fetch it |

  With `--offline`, starting the server never uses the network (checked with `strace`: no IP connections). The
  final version was tested with empty coursier and scala-cli caches and an empty scala-cli config, like CI.
- **The JDK from `install.sh` wins over `JAVA_HOME`.** GitHub runners set `JAVA_HOME` to JDK 17; our first CI run on
  the $12 challenge compiled with it and failed. `build.sh` and `start.sh` use `/opt/jdk-27` when it exists.
- **JDK `27+35`, not `27.0.1`.** Temurin published 27.0.1 two days before this was written, x64 only and with an
  unusual version string. The first release has both architectures.

## Local numbers

A laptop (16 logical CPUs shared with Postgres 18 in Docker and the load generator), `bench/mload -gzip -think-scale
2.5`, both ports, 30 s ramp, the server in a 12 GB cgroup (9 GB heap):

| Users | Hold | req/s | p95 | p99 | Errors |
|---|---|---|---|---|---|
| 20,000 | 60 s | 840 | 1.2 ms | 1.9 ms | 0 |
| 40,000 | 90 s | 1,630 | 1.2 ms | 2.0 ms | 0 |

These only show that the server works: a laptop runs out of source ports long before the server runs out of anything.
The test suite passes 42 of 42.

## Open

- **A million connections** needs the connection loop described above. Candidates from [RESEARCH.md](RESEARCH.md):
  http4s on Netty (`http4s-netty-server`, same routes; Netty entries report ~4.5 KB per connection, not measured with
  the http4s adapter), our own loop on Cats Effect's `Selector`, or a lazily allocated read buffer in fs2 itself
  (8.6 of the 38 KB).
- **Ember is the most memory-hungry stack here**: 38 KB per idle connection, against 72 B to 28 KB for the other
  entries and examples (RESEARCH.md). The lean ones hold no buffer while a connection is idle.
- **Not measured yet:** heap per in-flight request at the limit (the Java example died of in-flight requests and full
  GCs at ~360,000 users, not of idle connections), and whether a lower `MAX_CONNECTIONS` should keep the process
  alive past its heap limit (a crash is not restarted).
- **The box itself** was not available. CPU per request at ~42,000 req/s on 16 cores, and how ZGC behaves with
  ~20 GB of connection state, are not measured.
