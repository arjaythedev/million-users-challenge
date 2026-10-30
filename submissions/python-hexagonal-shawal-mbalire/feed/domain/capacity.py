"""The capacity model: the arithmetic that sizes processes and database connections.

Everything the composition root decides about topology comes from here, so the reasoning lives in
one reviewable place instead of as magic numbers in a launcher script. Every input is a named
parameter, and every one of them can be overridden from the environment (see feed.runtime.config).

1. Offered load (the workload)
------------------------------
A simulated user loads one page roughly every THINK_TIME_S seconds (mload with -think-scale 2.5).
Arrivals from N independent users are close to Poisson with rate

    lambda = N / THINK_TIME_S            1,000,000 users -> ~41,700 requests/s

and the request mix per user cycle is about 47% GET /feed, 47% GET /posts/:id, 7% likes, 1% new
posts: ~92% reads, ~8% writes.

2. Workers: one event loop per core
-----------------------------------
A CPython process executes one thread of Python at a time, so a single event loop can use at most
one core. Below one loop per core, cores sit idle; above it, loops time-slice the same core and
pay context switches plus the cache misses of a cold working set, with no gain in throughput.
At 16 cores each loop must sustain lambda / 16 ~ 2,600 requests/s to hold a million users.

Each worker binds its *own* SO_REUSEPORT socket on each port, so the kernel hashes connections
across workers evenly (a shared accept queue lets whichever loop wakes first take most of them,
and the hottest loop then caps the whole box).

3. Database lanes: Little's law and head-of-line blocking
---------------------------------------------------------
A *lane* is one Postgres connection in pipeline mode: many queries in flight, answered in order.
Little's law gives the queries in flight across the box,

    L = lambda * W                       41,700/s * ~1 ms (RTT + execution) ~ 42 queries

and that load must be spread over enough Postgres backends that none is saturated, because a
backend executes its queue serially: its utilization is rho = lambda_lane * S, with S the mean
execution time, and queueing delay grows like rho / (1 - rho). The DB box has 16 cores; ~3 read
backends per DB core keeps every core busy while some backends wait on the network, which gives
TARGET_READ_LANES = 48 regardless of how many app cores there are.

Writes get their own lanes. A pipelined lane is FIFO: an INSERT that waits for its WAL flush
(synchronous_commit stays on, as the rules require) would hold every read queued behind it on
that connection, and those are the requests that dominate p95. With a separate write lane per
worker, commit latency only delays other writes, and concurrent commits from all workers still
share fsyncs inside Postgres (group commit).

All of this stays far under the challenge's hard cap of POSTGRES_CONNECTION_CAP connections:
16 workers x (3 read + 1 write) = 64.
"""
import math

THINK_TIME_S = 24.0
POSTGRES_CONNECTION_CAP = 512
TARGET_READ_LANES = 48
WRITE_LANES_PER_WORKER = 1


def offered_load(users: int, think_time_s: float = THINK_TIME_S) -> float:
    """Requests per second that `users` simulated users send."""
    return users / think_time_s


def in_flight(requests_per_s: float, latency_s: float) -> float:
    """Little's law, L = lambda * W: mean number of requests inside the system at once."""
    return requests_per_s * latency_s


def worker_count(cores: int) -> int:
    """One event loop per core (section 2)."""
    return max(1, cores)


def read_lanes_per_worker(workers: int, target_total: int = TARGET_READ_LANES,
                          write_lanes: int = WRITE_LANES_PER_WORKER,
                          cap: int = POSTGRES_CONNECTION_CAP) -> int:
    """Spread `target_total` read backends over the workers, never exceeding the connection cap."""
    wanted = math.ceil(target_total / workers)
    allowed = cap // workers - write_lanes
    if allowed < 1:
        raise ValueError(f"{workers} workers cannot each get a read and a write lane under {cap} connections")
    return max(1, min(wanted, allowed))
