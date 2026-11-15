# Lab measurements, 2026-11-15

These are development measurements on a lab with slower cores and a weaker NIC than the rig. They are not scores, and the rig has not run this entry.

## Full user counts, next to the leading entry

To compare against a known rig result, the current first-place entry (`c-ammarserver-AmmarkoV`, 1,250,000 users on the rig's two cores) was built from `main` and run unchanged in the same lab under the same load.

Setup: the app box has 8 cores. The server is confined to two of them (`AllowedCPUs`), and the NIC's interrupts and receive processing are pinned to the same two. PostgreSQL 18.6 runs on a second host. A share of the users comes over the network from that host and the rest over loopback from a generator on the app box's other cores. Loopback packets are processed on the CPU of the socket that reads them (RFS), so the server's receive work stays on its two cores. A loopback user cost the server 6% less CPU than a network user (40.0 vs 42.6 µs per request at 400,000 users). Every run uses `mload`'s user model at think scale 2.5 with 15-second keep-alive probes, a ramp of 3,300 connections per second and a 90-second hold. Pass means p95 < 500 ms, p99 < 1 s and under 1% errors over the whole run, as in `mload`.

Two cores:

| Users | Entry | Result | Hold req/s | p95 / p99 (whole run) | Errors | Cores busy | CPU per request |
|---:|---|---|---:|---:|---:|---:|---:|
| 750,000 | `c-ammarserver-AmmarkoV` | pass | 30,500 | 131 / 614 ms | 0 | 82.5% | 54.1 µs |
| 850,000 | `c-ammarserver-AmmarkoV` | fail | 29,316 | 8,843 / 17,746 ms | 0.0007% | 85.3% | 58.2 µs |
| 900,000 | this entry | pass | 36,707 | 286 / 627 ms | 0 | 86.2% | 47.0 µs |
| 1,000,000 | this entry | fail | 40,595 | 2,931 / 3,684 ms | 0 | 94.6% | 46.6 µs |
| 1,250,000 | `c-ammarserver-AmmarkoV` | fail | 29,291 | 35,260 / 59,747 ms | 1.64% | 93.9% | 64.1 µs |
| 1,250,000 | this entry | fail | 35,848 | 14,116 / 15,439 ms | 0 | 93.5% | 52.2 µs |

On these cores the leading entry holds between 750,000 and 850,000 users and this entry between 900,000 and 1,000,000, about a fifth more. Overloaded at 1,250,000 users, this entry still served 22% more requests per second at 19% less CPU per request. Raising `READ_SESSIONS` to 32 did not help at 1,000,000 users (97% busy): that limit is the CPU.

Four cores (server, NIC interrupts and receive processing on four cores; 400,000 users over the network and 1,000,000 over loopback from the other four):

| Users | Entry | Result | Hold req/s | p95 / p99 (whole run) | Errors | Cores busy | CPU per request |
|---:|---|---|---:|---:|---:|---:|---:|
| 1,400,000 | `c-ammarserver-AmmarkoV` | fail | 57,894 | 591 / 1,968 ms | 0 | 83.8% | 57.9 µs |
| 1,400,000 | this entry | pass | 58,577 | 342 / 915 ms | 0 | 73.3% | 50.1 µs |

At this size the lab's database link carries about 740 Mbit/s of query results, and both entries spend part of their latency waiting on it; with 650,000 of 1,600,000 users over the network (the database host also generating them), both failed at 1,600,000.

## io_uring loops next to epoll loops

`zig-epoll-kamkm` and this entry share everything above the I/O: HTTP framing, JSON, tokens, gzip, the PostgreSQL wire code, the gather windows and the write groups. They differ only in how a loop waits for and performs I/O: epoll with one `read` and one `send` per request there, an io_uring with multishot accepts and receives, lent buffers and 2 ms passes here.

### Setup

Development lab, not the challenge rig. The app ran on a VM with two cores online (the rig's `setup-app-box.sh` network settings, RPS on the same two cores), PostgreSQL 18.6 on a second host and the load generator on a third, all in one cluster, so every database round trip crosses a real network. The generator is an epoll program that follows `mload`'s user model (same request mix, gzip, keep-alive probes, think times scaled by a factor). Memory on the generator host limited these runs to 200,000 connections, so the think time was shortened to reach the request rates of larger runs. CPU is the busy share of the two cores during a 60-second hold, from `/proc/stat` (user, system and softirq, so kernel network work is included).

### Two cores, same load, both entries

| Load | Build | Hold req/s | p50 | p95 | p99 | Cores busy | CPU per request | Server process per request |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| 100,000 users, think scale 0.5 | `zig-epoll-kamkm` | 21,139 | 4.5 ms | 6.8 ms | 7.7 ms | 39.2% | 37.1 us | 31.0 us |
| | this entry | 21,141 | 5.1 ms | 7.7 ms | 8.4 ms | 33.0% | 31.2 us | 17.9 us |
| 100,000 users, think scale 0.2 | `zig-epoll-kamkm` | 52,718 | 5.6 ms | 11.5 ms | 33.3 ms | 82.4% | 31.3 us | 29.2 us |
| | this entry | 52,686 | 7.9 ms | 12.8 ms | 16.8 ms | 68.1% | 25.8 us | 18.7 us |
| 200,000 users, think scale 0.4 | `zig-epoll-kamkm` | 51,912 | 6.8 ms | 429.4 ms | 1,403.3 ms | 85.1% | 32.8 us | 30.9 us |
| | this entry | 52,755 | 8.6 ms | 15.0 ms | 21.7 ms | 72.9% | 27.6 us | 20.9 us |

52,700 req/s is the hold request rate of about 1.25 million users. "Server process" is the CPU time charged to the server, which with `DEFER_TASKRUN` includes the TCP send and receive work the kernel does on the loop's thread; the rest is softirq work for packets and is the same for both builds. No request failed in any of these runs.

### Pass length

At 100,000 users, think scale 0.2:

| `PASS_US` | p50 | p99 | Cores busy | CPU per request |
|---:|---:|---:|---:|---:|
| 0 (wake on every completion) | 5.4 ms | 282.8 ms | 84.3% | 32.1 us |
| 1000 | 7.3 ms | 24.0 ms | 74.7% | 28.4 us |
| 2000 (default) | 7.9 ms | 16.8 ms | 68.1% | 25.8 us |
| 4000 | 12.4 ms | 29.0 ms | 69.4% | 26.4 us |

The ring alone saves nothing: woken for every completion it costs as much as epoll. The saving comes from collecting a pass's worth of completions per wakeup.

A profile of this entry at 52,700 req/s shows the server's own code at a few percent of the samples; the rest is the kernel's TCP, virtio and packet filter work for a request, its response and their ACKs.

## Checks

All 42 API checks and `request_buffers.py`, `feed_freshness.py`, `concurrent_writes.py`, `contract_edges.py`, `http_transport.py` and `gzip_responses.py` pass with the defaults and with `DB_POOL=1`. With light load running, every backend was terminated three times and PostgreSQL was then restarted: under 1% of the requests of that 45-second run got a 500 (503 for `/health`), the sessions were replaced, and the server answered again without a restart.

