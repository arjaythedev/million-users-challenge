# C++ io_uring — theargcoder

High-performance C++23 implementation of the Million Users Challenge.

* **Language:** C++23 (`gcc`, `-O3 -march=native -mtune=native -flto`)
* **Framework / IO:** none; raw Linux `io_uring` (`liburing`) with provided-buffer rings and shared userspace memory pool (UMEM)
* **Database Driver:** PostgreSQL via `libpqxx` with prepared statements and pipelined execution
* **Authentication:** JWT HS256 verification using OpenSSL
* **Front:** direct (no Nginx/proxy), serving `PORT` and `EXTRA_PORT` directly

## Design

* **Network Layer (`io_uring`):** Leverages multishot `ACCEPT` and multishot `RECV` operations combined with an `io_uring` provided-buffer ring. RX and TX buffers reside in a shared UMEM region with strict separation of ownership to prevent buffer reuse race conditions.
* **Concurrency & Threading:** Built around single-producer/single-consumer (SPSC) lock-free queues passing packets between network event loops and worker threads. Queue indices use atomic variables with acquire/release memory ordering, aligned to `std::hardware_destructive_interference_size` to completely eliminate false sharing. Thread wakeups use `std::atomic::wait` (futexes) for sub-millisecond responsiveness without idle busy-polling.
* **Request Parsing & Auth:** HTTP requests are parsed in-place from fixed-size buffer regions using `picohttpparser` with zero allocations. POST requests undergo zero-copy JWT HS256 signature and expiration validation via OpenSSL.
* **Database Pipeline:** Authenticated requests are routed round-robin to database worker threads via SPSC queues. Workers batch and execute queries using `libpqxx` prepared statements in pipeline mode, ensuring maximum throughput and minimal round-trip overhead.
* **Thread Allocations:** Allocates `nproc - 2` threads for DBReaders, `1` thread for iouring, `1` thread for http parsing and query queueing.
* **Zero-Copy Serialization:** JSON responses are serialized directly into pre-allocated TX packet buffers within UMEM, bypassing intermediate allocations. TX descriptors are reclaimed in submission order via atomic tracking to handle out-of-order send completions safely.
* **Keep-Alive:** Persistent TCP connections and HTTP keep-alive are fully honored; connections remain open indefinitely unless malformed requests or protocol violations occur.

## Optimizations, and why

| Optimization | Why |
| --- | --- |
| Raw `io_uring` + multishot ops & provided buffers | Eliminates syscall overhead, context switches, and buffer allocation churn in high-concurrency loops. |
| Lock-free SPSC queues with atomic alignment | Avoids mutex contention and cache-line bouncing (`std::hardware_destructive_interference_size`) across worker threads. |
| `std::atomic::wait` (futex) synchronization | Combines instant thread wake-ups with zero CPU burn during idle periods. |
| Zero-copy direct JSON serialization into UMEM TX buffers | Bypasses intermediate string allocations and copy overhead during response generation. |
| `picohttpparser` with in-place header parsing | Parses HTTP headers directly out of the RX buffer with zero memory allocations. |
| `libpqxx` pipeline mode with prepared statements | Batches round trips to PostgreSQL, slashing network latency and eliminating query plan overhead. |
| Separate RX/TX buffer ownership & reclamation | Prevents race conditions where active transmit buffers are prematurely overwritten by incoming RX fills. |
| `Small Number of Threads`| utilizing a small number of threads will reduce the cost of context switches minimizing latency spikes; can come at the cost of posibible queue saturation and failing to fufill requests at a time |

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

* GCC and G++
* CMake
* OpenSSL
* libpqxx
* PostgreSQL client libraries
* liburing
* pkg-config

## Challenge compliance

* No external cache service.
* No cross-request response or query-result caching.
* JWT signature and expiration validation on requests.
* Bearer-token authentication.
* PostgreSQL-backed request processing.
* Writes are committed before successful responses are sent.
* No database schema modifications.
* Both required listening ports are served directly by the application.

## License

MIT
