# Thread-per-core loops and an own wire layer, 2026-11-14

The rig run of `go-kamkm` (PR #41, `0094729`) held 1,360,000 users on four cores at 78% CPU, and its median latency was 25–44 ms on every machine size, even at 22% CPU. Both had the same cause: the gnet event loops held every processor, so the pgx goroutines and the responders ran only when the runtime preempted a loop (about every 10 ms), and each request paid for several hand-offs between goroutines and for its own share of small database packets. Raising `GOMAXPROCS` above the core count removed the latency (p50 3.3 ms) but cost 30% more CPU in thread switches.

This entry is built differently, as described in the README: one epoll loop per core that owns its clients and its PostgreSQL sessions, an own implementation of the extended-query protocol, and a 5 ms gather window per session.

## Setup

Development lab, not the challenge rig. The app ran on a VM with four cores online (the rig's `setup-app-box.sh` network settings, RPS on the same four cores), PostgreSQL 18.6 on a second host and the load generator on a third, all in one cluster, so every database round trip crosses a real network. The generator is an epoll program that follows `mload`'s user model (same request mix, gzip, keep-alive probes, think times scaled by a factor) so that one box can hold 600,000 to 1,200,000 connections. CPU is the busy share of the four cores during the hold, from `/proc/stat` (user, system and softirq, so kernel network work is included); packets are counted by nftables on the app box.

## Same load, both entries

600,000 users at think scale 2.5 (24,100 req/s in a 90-second hold):

| Build | p50 | p95 | p99 | Cores busy | CPU per request | Packets per request (PostgreSQL share) | Database box busy |
|---|---:|---:|---:|---:|---:|---:|---:|
| `go-kamkm` `0094729` (scored) | 13.5 ms | 55.9 ms | 77.6 ms | 50.2% | 83.2 µs (29.7 user, 19.9 system, 33.5 softirq) | 6.34 (1.09) | 22.9% |
| this entry, run 1 | 4.0 ms | 47.2 ms | 63.6 ms | 35.9% | 59.5 µs (13.0 user, 14.3 system, 32.1 softirq) | 5.84 (0.59) | 19.2% |
| this entry, run 2 | 4.1 ms | 48.6 ms | 66.1 ms | 39.1% | 64.9 µs (13.1 user, 15.8 system, 35.9 softirq) | 5.85 (0.58) | 19.7% |

The two runs are hours apart on a shared lab; the kernel's share moves by about 10% between them and the server's own does not. User-space CPU more than halved; what remains is mostly the kernel's TCP work for the client side, which is fixed by the workload (a request, a response, an ACK and a keep-alive probe with its ACK per request at this think time). The app box sent 2.42 packets per request instead of 2.70.

## Request rates beyond the scored run

The lab VM's virtual NIC starts dropping packets near 240,000 packets per second, which a million users at think scale 2.5 already reach (that run passed, but with p95 at 99 ms from retransmissions). To test the request rate of 1.45 million users (about 58,000 req/s in the hold) without the lab's packet ceiling, the think time was shortened instead:

| Build | Load | Hold req/s | p50 | p95 | p99 | Errors | Cores busy | CPU per request |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| this design | 600,000 users, think scale 1.1 | 57,581 | 3.6 ms | 30.4 ms | 48.1 ms | 0 | 62.2% | 43.2 µs |
| this design | 1,200,000 users, think scale 2.05, 60 s keep-alive probes | 61,748 | 3.7 ms | 7.2 ms | 59.3 ms | 0 | 67.0% | 43.4 µs |
| this design | 1,000,000 users, think scale 2.5 | 42,255 | 4.5 ms | 99.5 ms | 177 ms | 0 | 56.3% | 53.3 µs |
| `go-kamkm` `0094729` | 600,000 users, think scale 1.1 | about 54,000 | p95 was already 129 ms when the generator host ran short of memory and its watchdog ended the run | | | | | |

The first three rows were measured on an earlier build of this design, which differs from the final one in defaults and tracing only. In the 600,000-user comparison above, run 2 is the final code; run 1 predates only the handling of a session that PostgreSQL itself ends.

A profile at 600,000 users puts about a quarter of the samples in the server (gzip is 10% of the total) and three quarters in the kernel's network stack.

## Checks

All 42 API checks, the freshness, concurrent-write and HTTP transport regressions, `go vet` and `go test -race` pass with the defaults and with `DB_POOL=1`. With light load running, every backend was terminated three times and PostgreSQL was then restarted: under 1% of the requests of that 45-second run got a 500 (503 for `/health`), the sessions were replaced, and the server answered again without a restart.

These are development measurements on a lab with a weaker NIC and slower cores than the rig; they are not scores, and the rig has not run this entry.
