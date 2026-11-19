# Loop-level batching, 2026-11-19

The rig held 1,440,000 users on four cores (`cad2505`). It failed near 1,560,000 with the CPU 52% busy, at the app box's network card limit, so on four cores packets per request set the score. This commit changes how a loop batches its database work, as described in the README: one read batch and one write batch per gather window, holding everything queued, with 64 read sessions and 2 write sessions per loop available for the batches that start while earlier ones are still out. Before, every free session took a share of the queue, and on four cores those shares were small.

## Four cores, every user over the network

820,000 users from the database host, 180-second hold; the lab's virtual NIC drops packets near 240,000 packets per second, so all three show the same p95 of about 220 ms from retransmissions.

| Build | Packets per second | Packets per request (in + out) | ACKs sent besides responses | PostgreSQL packets per request | CPU per request | p99 in the hold |
|---|---:|---:|---:|---:|---:|---:|
| as filed (`cad2505`) | 204,000 | 5.97 | 1.15 | 0.82 | 47.0 µs | 236 ms |
| this commit | 186,000 | 5.42 | 1.03 | 0.40 | 45.2 µs | 232 ms |
| `rust-uring-theCatWisel` | 188,000 | 5.48 | 1.02 | 0.46 | 49.8 µs | 225 ms |

## Two cores at 1,000,000 users

The two-core setup below, with 180-second holds. The filed build and this commit ran back to back in two rounds, in alternating order, followed by the two entries that lead the board on two cores. Every run passed.

| Build | p99 whole run / hold | Worst 5 s in the hold (p95) | CPU per request | ACKs sent besides responses | PostgreSQL packets per request |
|---|---:|---:|---:|---:|---:|
| as filed, round 1 | 251 / 130 ms | 162 ms | 40.3 µs | 1.40 | 0.50 |
| this commit, round 1 | 114 / 118 ms | 96 ms | 37.8 µs | 1.37 | 0.28 |
| this commit, round 2 | 210 / 234 ms | 223 ms | 39.8 µs | 1.50 | 0.30 |
| as filed, round 2 | 120 / 133 ms | 128 ms | 41.1 µs | 1.48 | 0.49 |
| `rust-uring-theCatWisel` | 95 / 108 ms | 87 ms | 38.3 µs | 1.22 | 0.36 |
| `c-epoll-unorig-username` | 113 / 128 ms | 140 ms | 39.2 µs | 1.26 | 0.34 |

Tails at this load vary from run to run: in four earlier runs the same day, the filed build's p99 in the hold ranged from 102 to 133 ms.

# Lab measurements, 2026-11-15

These are development measurements, not scores, and the rig has not run this entry. The lab's app box has the rig's CPU model (Xeon Platinum 8260), but it is a busier VM with its own firewall rules, half the rig's RAM (so half as many TCP hash buckets) and part of the load generated on the same box, so absolute numbers are lower than the rig's. The comparisons below run both servers under identical load.

## Two cores, next to the leading entry

The server is confined to two of the app box's eight cores (`AllowedCPUs`), with the NIC's interrupts and receive processing on the same two. 120,000 users come over the network from the database host; the rest come from a generator on the other six cores, in its own network namespace (its own TCP hash table) over a veth pair, with receive processing for each side kept on that side's cores and timers kept on the CPU that armed them. PostgreSQL 18.6 runs on the other host. `mload`'s user model at think scale 2.5 with 15-second keep-alive probes, 3,300 connections per second of ramp, then a 60-second hold. Pass means p95 < 500 ms, p99 < 1 s and under 1% errors over the whole run.

| Users | Build | Result | Hold req/s | p95 / p99 (whole run) | Errors | Cores busy | CPU per request |
|---:|---|---|---:|---:|---:|---:|---:|
| 850,000 | `c-ammarserver-AmmarkoV` (main) | fail | 32,315 | 2,240 / 5,065 ms | 0 | 85.7% | 53.0 µs |
| 1,050,000 | `c-ammarserver-AmmarkoV` (main) | fail | 32,372 | 16,226 / 28,611 ms | 0.03% (timeouts) | 85.7% | 52.9 µs |
| 950,000 | this entry as first filed | pass | 38,038 | 84 / 251 ms | 0 | 87.4% | 45.9 µs |
| 1,000,000 | this entry as first filed | fail | 41,417 | 1,150 / 1,646 ms | 0 | 91.2% | 44.1 µs |
| 950,000 | this entry | pass | 38,179 | 188 / 602 ms | 0 | 78.3% | 41.0 µs |
| 1,050,000 | this entry | pass | 42,787 | 138 / 345 ms | 0 | 89.0% | 41.6 µs |
| 1,100,000 | this entry | fail | 44,801 | 2,354 / 3,205 ms | 0 | 91.2% | 40.7 µs |

The 950,000-user run of the current code used the earlier read settings (16 read sessions, 5 ms window); the 1,050,000 and 1,100,000 runs use the current defaults (32 sessions, 2 ms).

What changed since the first filing:

- **Clients on direct descriptors.** Accepted sockets go straight into the ring's own file table, so a receive or send skips the descriptor table lookup and the file reference count.
- **One cache line per client.** The client record shrank to 64 bytes and comes from a per-loop pool aligned to cache lines; with a million clients nearly every completion touches a cold record.
- **JSON escaping sixteen bytes at a time**, with room for a whole row reserved once. User time per request fell from 8.0 to 6.2 µs.
- **Lower database queueing.** 32 read sessions and a 2 ms read window. Below about 40 ms a response carries the ACK of its request; past it the kernel's delayed-ACK timer sends a separate ACK and the next request on that connection is acknowledged at once, so slow responses cost packets and CPU and push latency further. Shorter queues keep more responses under that line: delayed-ACK timer firings per 1,000 requests fell from 252 to 108 at 950,000 users.

On these two cores the leading entry tops out near 32,000 requests per second and does not hold 850,000 users; this entry holds 1,050,000.

## Earlier lab setup (generator on loopback in the app's namespace)

These runs predate the namespace and timer changes above: the generator's sockets shared the app's TCP hash table and some of its timers ran on the app's cores, which cost both servers capacity.

To compare against a known rig result, the current first-place entry (`c-ammarserver-AmmarkoV`, 1,250,000 users on the rig's two cores) was built from `main` and run unchanged in the same lab under the same load.

Setup: the app box has 8 cores. The server is confined to two of them (`AllowedCPUs`), and the NIC's interrupts and receive processing are pinned to the same two. PostgreSQL 18.6 runs on a second host. A share of the users comes over the network from that host and the rest over loopback from a generator on the app box's other cores. Loopback packets are processed on the CPU of the socket that reads them (RFS), so the server's receive work stays on its two cores. A loopback user cost the server 6% less CPU than a network user (40.0 vs 42.6 µs per request at 400,000 users). Every run uses `mload`'s user model at think scale 2.5 with 15-second keep-alive probes, a ramp of 3,300 connections per second and a 90-second hold. Pass means p95 < 500 ms, p99 < 1 s and under 1% errors over the whole run, as in `mload`.

Two cores (earlier setup):

| Users | Entry | Result | Hold req/s | p95 / p99 (whole run) | Errors | Cores busy | CPU per request |
|---:|---|---|---:|---:|---:|---:|---:|
| 750,000 | `c-ammarserver-AmmarkoV` | pass | 30,500 | 131 / 614 ms | 0 | 82.5% | 54.1 µs |
| 850,000 | `c-ammarserver-AmmarkoV` | fail | 29,316 | 8,843 / 17,746 ms | 0.0007% | 85.3% | 58.2 µs |
| 900,000 | this entry | pass | 36,707 | 286 / 627 ms | 0 | 86.2% | 47.0 µs |
| 1,000,000 | this entry | fail | 40,595 | 2,931 / 3,684 ms | 0 | 94.6% | 46.6 µs |
| 1,250,000 | `c-ammarserver-AmmarkoV` | fail | 29,291 | 35,260 / 59,747 ms | 1.64% | 93.9% | 64.1 µs |
| 1,250,000 | this entry | fail | 35,848 | 14,116 / 15,439 ms | 0 | 93.5% | 52.2 µs |

On these cores the leading entry holds between 750,000 and 850,000 users and this entry between 900,000 and 1,000,000, about a fifth more. Overloaded at 1,250,000 users, this entry still served 22% more requests per second at 19% less CPU per request. With the code as first filed and a 5 ms window, raising `READ_SESSIONS` to 32 did not help at 1,000,000 users (97% busy): that limit was the CPU.

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

