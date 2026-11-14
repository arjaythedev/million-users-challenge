# One loop per core with its own PostgreSQL sessions, 2026-11-14

The rig run of `zig-kamkm` (PR #43, `ac1b494`) held 1,190,000 users on four cores at 68% CPU, "sending at the app box's network ceiling". The lab shows why: that build sent 3.38 packets per request, more than a third of them to PostgreSQL, because under steady load each of its 16 database threads usually found one request in its queue and sent it alone, and every request also crossed two thread hand-offs and re-armed its socket with `epoll_ctl`.

This entry is built differently, as described in the README: one epoll loop per core that owns its clients and its PostgreSQL sessions, the wire protocol spoken directly after libpq has connected and prepared, and a 5 ms gather window per session. SQL, token checks and the response format are the same as in `zig-kamkm`, and nothing is answered before the commit's `ReadyForQuery`.

## Setup

Development lab, not the challenge rig. The app ran on a VM with four cores online (the rig's `setup-app-box.sh` network settings, RPS on the same four cores), PostgreSQL 18.6 on a second host and the load generator on a third, all in one cluster, so every database round trip crosses a real network. The generator is an epoll program that follows `mload`'s user model (same request mix, gzip, keep-alive probes, think times scaled by a factor) so that one box can hold 600,000 to 1,200,000 connections. CPU is the busy share of the four cores during the hold, from `/proc/stat` (user, system and softirq, so kernel network work is included); packets are counted by nftables on the app box.

## Same load, both entries

600,000 users at think scale 2.5 (24,100 req/s in a 90-second hold):

| Build | p50 | p95 | p99 | Cores busy | CPU per request | Packets per request (PostgreSQL share) | Packets sent per request | Database box busy |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `zig-kamkm` `ac1b494` (scored) | 0.8 ms | 61.1 ms | 211.9 ms | 64.5% | 107.0 us | 7.38 (2.08) | 3.38 | 26.7% |
| this entry | 4.0 ms | 46.7 ms | 63.6 ms | 34.7% | 57.6 us (9.8 user, 14.5 system, 33.2 softirq) | 5.88 (0.63) | 2.44 | 18.6% |

The median rises by the gather window's share (a request waits up to 5 ms for its batch); the tail falls. What remains is mostly the kernel's TCP work for the client side, which the workload fixes: a request, a response, an ACK and a keep-alive probe with its ACK per request at this think time.

## Request rates beyond the scored run

The lab VM's virtual NIC starts dropping packets near 240,000 packets per second, which a million users at think scale 2.5 already reach. To test the request rate of 1.45 million users (about 58,000 req/s in the hold) below that ceiling, the think time was shortened instead:

| Load | Hold req/s | p50 | p95 | p99 | Errors | Cores busy | CPU per request |
|---|---:|---:|---:|---:|---:|---:|---:|
| 600,000 users, think scale 1.1 | 54,970 | 4.5 ms | 30.7 ms | 49.1 ms | 0 | 57.5% | 41.9 us |
| 1,200,000 users, think scale 2.05, 60 s keep-alive probes | 60,350 | 5.1 ms | 8.3 ms | 21.1 ms | 0 | 63.6% | 42.2 us |

The second run also shows the connection count is not what costs: twice the connections at the same request rate take the same CPU per request.

## Checks

All 42 API checks and `request_buffers.py`, `feed_freshness.py`, `concurrent_writes.py`, `contract_edges.py`, `http_transport.py` and `gzip_responses.py` pass with the defaults, with `DB_POOL=1`, and under the `send_faults.c` shim (short sends and spurious `EAGAIN` on client and PostgreSQL sockets). With light load running, every backend was terminated three times and PostgreSQL was then restarted: under 1% of the requests of that 45-second run got a 500 (503 for `/health`), the sessions were replaced, and the server answered again without a restart.

These are development measurements on a lab with a weaker NIC and slower cores than the rig; they are not scores, and the rig has not run this entry.
