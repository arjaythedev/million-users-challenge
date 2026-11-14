# C++ io_uring — theargcoder

High-performance C++23 implementation of the Million Users Challenge.

- **Language:** C++23 (`GCC`, `-O3 -march=native -mtune=native -flto`)
- **Networking / I/O:** Raw Linux `io_uring` using `liburing`, multishot accept/receive operations, and preallocated RX/TX memory pools via UMEM (mmap).
- **Database:** PostgreSQL through `libpqxx`, worker-local connections, prepared statements, and pipelined query execution.
- **Authentication:** JWT HS256 verification using OpenSSL with precomputed per-worker HMAC states.
- **HTTP:** Direct application networking, without Nginx or a reverse proxy, serving `PORT` and `EXTRA_PORT`.

## Design

### Network layer — io_uring

The networking layer uses a single `io_uring` event loop to manage socket operations and packet lifecycles.

- Multishot `ACCEPT` handles incoming connections on both listening ports.
- Multishot `RECV` receives data using kernel-provided RX buffers.
- TX responses are submitted asynchronously through `io_uring`.
- Independent RX and TX memory pools prevent receive-buffer ownership from interfering with active transmissions.
- Buffer descriptors are recycled only after their respective ownership and completion conditions are satisfied.
- Partial TCP sends are handled by advancing the TX buffer cursor and resubmitting the remaining bytes.
- Transient send failures such as `EAGAIN` and `EINTR` are handled through descriptor resubmission.

The current configuration uses 16,384 RX frames and 16,384 TX frames, each with a 20 KiB buffer.

### Concurrency and threading

The application separates networking, HTTP request processing, and database execution.

- A dedicated networking event loop manages socket I/O and buffer reclamation.
- HTTP workers parse requests, perform authentication, validate endpoints, and dispatch database operations.
- Database workers maintain independent PostgreSQL connections and process requests from their corresponding worker queues.

SPSC queues transfer requests between processing stages. Queue indices use atomic variables with acquire/release memory ordering and cache-line alignment through `std::hardware_destructive_interference_size` to reduce false sharing.

Worker synchronization uses `std::atomic::wait` and `notify_one`, avoiding continuous polling while workers are idle.

Worker wakeups are represented by `std::bitset<MAX_SUPPORTED_WORKERS>`, eliminating the undefined behavior highlighted by benchmark agent (thanks).

### Scalable worker pool

The worker count is selected using the virtual CPU affinity available to the process.

CPU detection first uses `sched_getaffinity()`, respecting process and container CPU restrictions, and falls back to the number of online processors.

| Detected virtual CPUs | HTTP workers | Database workers |
|---|---:|---:|
| Fewer than 8 | 48 | 48 |
| 8–15 | 96 | 96 |
| 16 or more | 192 | 192 |

The implementation supports a maximum worker capacity of 512. The current automatic allocation policy selects up to 192 workers.

Each HTTP worker owns its own request-processing state and JWT verifier. Each database worker owns its own PostgreSQL connection.

### Request parsing and authentication

HTTP request headers are parsed using `picohttpparser`, operating directly on received packet data and avoiding unnecessary intermediate allocations.

The application supports the challenge's HTTP endpoints and performs method, path, authorization, and request-body validation before dispatching database operations.

JWT authentication uses HS256 and validates signatures and expiration for authenticated requests.

**Optimized HMAC verification:**

- Each HTTP worker initializes its own HMAC verifier during startup.
- SHA-256 inner and outer HMAC pad states are precomputed once per verifier.
- Request verification copies the precomputed digest states into worker-private working contexts.
- The signing input is hashed without reinitializing the HMAC key or reprocessing the secret.
- Signature comparison uses `CRYPTO_memcmp` for constant-time comparison.
- JWT decoding, header validation, and claims extraction use bounded caller-owned buffers.

This removes repeated HMAC key initialization from the authenticated request path while preserving independent verifier state across HTTP workers.

### Database pipeline

Database requests are distributed to worker-local SPSC queues.

Each database worker:

1. Maintains an independent PostgreSQL connection.
2. Initializes its prepared statements once.
3. Collects queued requests into bounded batches.
4. Executes batchable operations using `libpqxx` pipeline mode.
5. Retrieves results in their original request order.
6. Commits transactions before constructing successful responses.
7. Returns completed responses to the networking layer.

The maximum query pipeline depth is 256 requests per batch.

Queue draining keeps the peek and dequeue positions synchronized, ensuring requests are not silently skipped or consumed from a subsequent batch.

Like operations use `INSERT ... ON CONFLICT DO NOTHING`, distinguishing newly created likes from repeated likes.

### Queue sizing and memory management

Per-worker queues are deliberately bounded to avoid excessive memory consumption at higher worker counts.

| Queue | Capacity per worker |
|---|---:|
| HTTP request queue | 4,096 entries |
| Database request queue | 4,096 entries |

Both queue capacities are one quarter of the 16,384-entry network ring size.

Queue storage is allocated during initialization rather than dynamically growing during request processing.

### Event loop and packet lifecycle

The networking event loop owns the `io_uring` instance and coordinates:

- Incoming connections and multishot accept operations.
- Receive operations and RX buffer selection.
- HTTP worker dispatch.
- TX descriptor submission.
- Send completion processing.
- RX buffer reclamation and reprovisioning.
- TX descriptor recycling.

Atomic ownership queues transfer completed or available descriptors between networking and processing threads.

The hot path avoids mutex-based queue synchronization.

### Direct JSON serialization

JSON responses are serialized directly into preallocated TX memory.

The JSON writer appends fields to the destination buffer without constructing intermediate JSON strings or objects.

Post bodies, usernames, timestamps, integer values, and error responses are serialized directly from their validated inputs or PostgreSQL results.

This minimizes allocation overhead and avoids an additional response-string copy.

### HTTP keep-alive

The application serves HTTP/1.1 responses with persistent-connection headers.

TCP send completion handling accounts for partial writes, ensuring that an incompletely transmitted response is not treated as fully sent.

## Optimizations and rationale

| Optimization | Rationale |
|---|---|
| Raw `io_uring` with multishot accept and receive | Reduces syscall and submission overhead under high concurrency. |
| Preallocated RX/TX memory pools | Reduces buffer allocation churn and establishes explicit buffer ownership. |
| Separate RX and TX descriptor pools | Prevents premature buffer reuse and transmission races. |
| Worker-local SPSC queues | Avoids mutex contention between request-processing stages. |
| Cache-line-aligned atomic indices | Reduces false sharing and cache-line bouncing. |
| Atomic wait/notify synchronization | Avoids unnecessary CPU consumption while workers are idle. |
| Bitset-based worker wakeups | Supports worker IDs beyond 31 without undefined integer-shift behavior. |
| Reduced per-worker queue capacity | Limits memory consumption while retaining bounded request buffering. |
| Per-worker precomputed JWT HMAC states | Removes repeated secret processing and HMAC initialization from the request path. |
| Worker-private OpenSSL digest contexts | Prevents concurrent mutation of verifier state. |
| Constant-time signature comparison | Avoids ordinary early-exit signature comparisons. |
| `picohttpparser` | Parses HTTP headers without unnecessary intermediate allocations. |
| PostgreSQL prepared statements | Avoids repeatedly preparing the same database operations. |
| Bounded `libpqxx` pipeline execution | Batches requests while preserving request-to-result ordering. |
| Direct JSON serialization | Avoids intermediate JSON allocations and response copies. |
| Partial-send recovery | Handles incomplete nonblocking TCP writes without discarding unsent response data. |
| Atomic descriptor reclamation | Ensures RX and TX buffers are recycled through their respective ownership paths. |

## Performance validation

The server has been evaluated using an isolated Linux network-namespace topology.

The test environment separates application traffic from database traffic using independent virtual Ethernet links:

- **Load-generator network:** A dedicated virtual Ethernet connection between the load-generator namespace and the server namespace.
- **Database network:** A separate virtual Ethernet connection between the PostgreSQL namespace and the server.
- **Network emulation:** Linux `tc netem` introduces configurable latency, jitter, and packet loss.
- **Load generation:** The project's `mload` benchmark tool simulates concurrent users across both application ports.
- **Isolation:** PostgreSQL traffic does not share the load-generator virtual network.

### Recorded local benchmark

| Metric | Result |
|---|---:|
| Simulated users | 50,000 |
| Sustained throughput | ~49,000 req/s |
| Peak steady-state throughput | ~49,700 req/s |
| p95 latency | ~109 ms |
| p99 latency | ~138 ms |
| Reported errors | 0.000% |
| Test duration | 120 seconds |

These results represent the recorded local virtual-network benchmark configuration. Your milage may vary

## Build

The installation script targets Ubuntu 24.04.

Install system dependencies:

```bash
sudo ./install.sh
```

Build the optimized binary:

```bash
./build.sh
```

The resulting executable is:

```text
build/.http_server
```

## Run

Configure the required environment variables:

```bash
export DATABASE_URL="postgres://user:pass@host:5432/dbname"
export JWT_SECRET="your_jwt_secret"
export PORT=80
export EXTRA_PORT=81
export HOST=0.0.0.0
```

Launch the server:

```bash
./start.sh
```

Binding to privileged ports below 1024 on Linux requires root privileges or the `CAP_NET_BIND_SERVICE` capability.

## Dependencies

The installation script targets Ubuntu 24.04 and installs:

- GCC and G++
- CMake
- OpenSSL
- libpqxx
- PostgreSQL client libraries
- liburing
- pkg-config

## Challenge compliance

- No external cache service.
- No cross-request response or query-result caching.
- JWT signature and expiration validation on every authenticated request.
- Bearer-token authentication.
- PostgreSQL-backed request processing.
- Successful write responses are sent after transaction commits.
- No database schema modifications.
- Both required listening ports are served directly by the application.
- Database connections are worker-local and should be configured within the challenge environment's connection limits.

## License

MIT
