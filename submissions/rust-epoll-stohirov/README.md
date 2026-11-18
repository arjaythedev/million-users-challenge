# rust-epoll-stohirov

Rust 1.94.0, no framework, no Postgres driver; the only crate is `libc` 0.2.177 (pinned in `Cargo.lock`).

```bash
sudo bash install.sh && bash build.sh
DATABASE_URL=postgres://... JWT_SECRET=... HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 bash start.sh
```

- One epoll loop per allowed CPU, `SO_REUSEPORT` listeners on both ports, connections steered to the loop on their
  CPU (`SO_INCOMING_CPU`), edge-triggered clients, 4 ms passes.
- Hand-written HTTP/1.1 and Postgres wire client: prepared statements, reads batched under one Sync, group-commit
  writes answered after commit, 48 read + 32 write connections in all.
- Own gzip (fixed Huffman, 16 KB hash table, PCLMUL CRC-32), own HS256 check, 32-byte connection slots on huge pages.
- `TCP_DEFER_ACCEPT`, delayed ACKs, thin-stream timeouts; built without AVX-512.

`test/run.sh`: 42/42. Locally (2 CPUs, ~44,600 req/s): 12.0 µs of server CPU per request, p99 14 ms.
