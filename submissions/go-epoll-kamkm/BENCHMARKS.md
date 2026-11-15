# Second round: two cores, 2026-11-15

The leaderboard's top entries are within 5% of each other on four cores, and their order is set by two cores. These runs confine the server to two cores of an 8-core VM with the rig's CPU model (Xeon Platinum 8260), with its NIC interrupts on the same two cores and PostgreSQL 18.6 on another host. 120,000 users come over the network and the rest from a generator on the VM's other cores, in its own network namespace so that its sockets and timers stay off the server's cores. The load follows `mload`'s user model at think scale 2.5, ramps at 3,300 connections/s, holds for 60 seconds and is judged by `mload`'s pass rule. As a reference point, the current leader (`c-ammarserver-AmmarkoV`, built unchanged from `main`) fails at 850,000 users in this setup.

| Users (2 cores) | first commit (`9ff8d53`) | this commit |
|---:|---|---|
| 850,000 | pass (p99 280 ms, 82.5% busy, 48.3 µs/req) | pass (p99 75 ms, 79.0% busy, 46.4 µs/req) |
| 950,000 | fail (p99 2.8 s, 97.5% busy) | pass (p99 156 ms; a build before the timestamp change, with 5 ms windows) |
| 1,000,000 | | passed twice (p99 545 ms and 188 ms, 45.1 µs/req), failed once (p99 1.4 s, with a packet capture running) |
| 1,050,000 | | fail (p99 5.5 s) |

With 180-second holds, closer to the rig's five minutes, this commit passes 950,000 users (p95 73 ms, p99 112 ms, 87.2% busy, 44.2 µs/req) and fails 1,000,000 (p99 2.9 s). At 1,000,000 the 60-second holds pass or fail on a burst at the end of the ramp, when about 3,300 new connections a second come on top of nearly the full request rate; a longer hold shows the server does not recover from it at that load.

What changed, in order of effect:

- **gzip with ISA-L.** A profile at 850,000 users put the portable encoder at 8.5% of the two cores, about 5 µs of the 11.6 µs of user time per request. Its time went to finding matches, not to writing bits (a rewrite of the bit writer alone was 8% slower). ISA-L's level 0 compresses a 2.9 KB feed in 2.6 µs instead of 4.9 µs, for 619 bytes instead of 506.
- **`recvfrom` and `sendto` on sockets.** `read` and `write` went through the generic file layer (`__fdget_pos`, `rw_verify_area`, file permission hooks) on every call, about 4% of the samples.
- **JSON strings scanned eight bytes at a time**, and timestamps converted with integer date arithmetic instead of `time.Time`, which computed the calendar date twice per row.
- **3 ms gather windows** (from 5 ms). Above about 90% CPU a response that takes longer than 40 ms makes the kernel send a delayed ACK and leave its ping-pong mode, which costs more packets and more CPU, and the server falls off a cliff. Shorter windows keep more responses under that limit; 1,000,000 users failed three times with 5 ms windows and passed twice with 3 ms. 2 ms windows, or 32 read sessions, cost more CPU in database packets than they saved and failed.

At 850,000 users, user time per request fell from 11.6 to 8.6 µs; the rest of the cost is the kernel's TCP work, which is the same for every entry. Three experiments did not help and are not in this commit: a raw `epoll_wait` that keeps the Go scheduler's monitor thread from taking the loop's processor away (it halved that thread's wakeups but saved under 1% of the CPU), 32 read sessions; and keeping client sockets in the kernel's delayed-ACK (ping-pong) mode with `TCP_QUICKACK=0`, on the listeners and again after each slow response. That removed the immediate ACK of first requests and of requests after a slow response (0.64 per request at 1,000,000 users), but a slow response then gets a timer-driven ACK instead, which costs about as much: at 950,000 users with 180-second holds it saved 0.1 packets per request and no CPU, and its p99 was 322 ms instead of 112 ms.

The VM adds overheads the rig does not have (firewall rules, part of the load generated on the same box), so the absolute numbers are lower than the rig's. These are development measurements; the rig has not run this entry.

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
