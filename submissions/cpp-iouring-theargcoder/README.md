# C++ io_uring — theargcoder

High-performance C++23 implementation of the Million Users Challenge.

- **Language:** C++23 (`gcc`, `-O3 -march=native -mtune=native -flto`)
- **Framework / IO:** None; raw Linux `io_uring` (`liburing`) with provided-buffer rings and shared userspace memory pool (UMEM).
- **Database Driver:** PostgreSQL via `libpqxx` with prepared statements and pipelined execution.
- **Authentication:** JWT HS256 verification using OpenSSL.
- **Front:** Direct application networking (no Nginx or reverse proxy), serving `PORT` and `EXTRA_PORT` directly.

## Design

- **Network Layer (`io_uring`):** Uses multishot `ACCEPT` and multishot `RECV` operations with an `io_uring` provided-buffer ring. 
  RX and TX buffers reside in UMEM with strict ownership separation to prevent premature buffer reuse and transmission races.

- **Concurrency & Threading:** Uses lock-free single-producer/single-consumer (SPSC) queues to transfer packets and requests between networking and processing threads. 
  Queue indices use atomic variables with acquire/release memory ordering and cache-line alignment using `std::hardware_destructive_interference_size` 
  to reduce false sharing. Thread wakeups use `std::atomic::wait`, avoiding unnecessary busy-polling while workers are idle.

- **Scalable Worker Pool:** The number of workers (HTTP and DB) scales with the detected virtual CPU count for:
  less than 8 CPUs = 64 workers, less than 16 CPUs = 96 workers and greater than or equal to 16 CPUs = 128 workers

- **Request Parsing & Authentication:** HTTP requests are parsed in-place from fixed-size buffer regions using `picohttpparser`, 
  avoiding unnecessary allocations. JWT HS256 signatures and expiration are validated for every authenticated request using OpenSSL.

- **Database Pipeline:** Authenticated requests are distributed to database workers through SPSC queues. 
  Each worker batches queued requests and executes them using `libpqxx` prepared statements and pipeline mode. 
  Pipeline batching preserves request-to-result ordering and processes the staged queries without silently skipping queued requests.

- **Event Loop & Packet Lifecycle:** The io_uring event loop manages receive, transmit, completion, and buffer-reclamation operations. 
  RX and TX buffer ownership is kept separate, and TX descriptors are reclaimed safely after send completions. 
  Event-loop handling is designed to sustain high concurrency without introducing mutex contention into the hot path.

- **Zero-Copy Serialization:** JSON responses are serialized directly into pre-allocated TX packet buffers within UMEM, 
  avoiding intermediate response-string allocations and unnecessary copies.

- **Keep-Alive:** Persistent TCP connections and HTTP keep-alive are supported. 
  Connections remain open according to the HTTP protocol and server-side connection handling.

## Optimizations and rationale

| Optimization | Why |
| --- | --- |
| Raw `io_uring` with multishot operations and provided buffers | Reduces syscall overhead, context switches, and buffer allocation churn under high concurrency. |
| Lock-free SPSC queues with atomic alignment | Avoids mutex contention and reduces cache-line bouncing between producer and consumer threads. |
| Dynamically scaled database worker pool | Adapts processing concurrency to the available virtual CPUs, up to 128 workers. |
| `std::atomic::wait` synchronization | Allows prompt thread wakeups without continuously consuming CPU while idle. |
| Zero-copy JSON serialization into UMEM TX buffers | Avoids intermediate string allocations and response-copy overhead. |
| `picohttpparser` with in-place parsing | Parses HTTP headers directly from RX buffers without unnecessary allocations. |
| `libpqxx` pipeline mode with prepared statements | Batches database operations to reduce round-trip overhead and query preparation costs. |
| Separate RX/TX buffer ownership and reclamation | Prevents active transmit buffers from being overwritten or reused prematurely. |
| Corrected io_uring event-loop handling | Improves packet processing, completion handling, and sustained network throughput. |
| Corrected database pipeline batching | Ensures queued queries are staged and processed correctly rather than repeatedly processing or skipping queue entries. |

## Performance validation

The server was tested using an isolated Linux network-namespace topology with two separate virtual Ethernet connections:

- **Load-generator network:** A dedicated virtual Ethernet link between the load-generator namespace and the server.
- **Database network:** A separate virtual Ethernet link between the database namespace and the server, with PostgreSQL traffic isolated from load-generator traffic.
- **Network emulation:** Linux `tc netem` was used to introduce configurable network latency and jitter.
- **Load generation:** The project's `mload` benchmark tool simulated up to 50,000 concurrent users.

<text>**Observed benchmark result:**</text>

| Metric | Result |
| --- | ---: |
| Simulated users | 50,000 |
| Sustained throughput | ~49,000 req/s |
| Peak steady-state throughput | ~49,700 req/s |
| p95 latency | ~109 ms |
| p99 latency | ~138 ms |
| Reported errors | 0.000% |
| Test duration | 120 seconds |

These are results from the local virtual-network benchmark environment, not a claim of equivalent throughput on every challenge machine configuration.

## Build

Install system dependencies as root (Ubuntu 24.04 target):

```bash
sudo ./install.sh
```

Build the optimized binary:

```bash
./build.sh
```

The resulting executable is generated at:

```text
build/.http_server
```

## Run

Configure the required environment variables:

```bash
DATABASE_URL=postgres://user:pass@host:5432/dbname
JWT_SECRET=your_jwt_secret
PORT=80
EXTRA_PORT=81
HOST=0.0.0.0
```

Launch the server:

```bash
./start.sh
```

> **Note:** Binding to privileged ports below 1024 on Linux requires root privileges or the `CAP_NET_BIND_SERVICE` capability.

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
- Writes are committed before successful responses are sent.
- No database schema modifications.
- Both required listening ports are served directly by the application.
- PostgreSQL connections remain within the challenge's connection limit.

## License

MIT
