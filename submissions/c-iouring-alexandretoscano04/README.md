# C / io_uring feed server

C17, GCC 13, Linux io_uring (raw syscalls against the kernel's `<linux/io_uring.h>`), Ubuntu 24.04.

Dependencies: **libpq 18.6** (connection setup and statement preparation only), **yyjson 0.10.0**,
**libdeflate 1.23**, system **OpenSSL 3**. Downloads are version-pinned and SHA256-verified; libpq and libdeflate
are statically linked.

## Build and run

```sh
sudo bash install.sh
bash build.sh
DATABASE_URL='postgres://feed:password@db-host:5432/feed' \
JWT_SECRET='YOUR_SECRET' HOST=0.0.0.0 PORT=80 EXTRA_PORT=81 \
bash start.sh
```

The server stays in the foreground and serves both ports. One worker process per available CPU (respecting the
cgroup quota), each with its own `SO_REUSEPORT` listeners, ring and database connections.

## Design

- **io_uring transport.** Multishot accept straight into a registered file table, multishot receive into shared
  provided buffers (whole requests are parsed in place), and one `SEND` per response with the HTTP header written
  into reserved headroom in front of the body. Sends, receives and database I/O for a whole loop pass go to the
  kernel in a single `io_uring_enter`.
- **Own PostgreSQL wire protocol.** libpq logs in (SCRAM) and prepares the five statements; the socket then moves
  onto the ring. Queries are written as binary Bind/Execute messages, and DataRows are serialized directly into
  the JSON response without libpq results or Describe round trips.
- **Batching, one query per request.** Each loop pass fills one read connection with up to 64 queries under a
  single Sync. Writes use group commit: a write group goes only to an idle connection, so all writes waiting
  behind a commit become one transaction. If a statement fails (for example a like on a missing post), Postgres
  rolls the group back; that request gets its error and the others run again, each in its own transaction.
  Responses are sent only after the commit's ReadyForQuery.
- **Allocation-free hot path.** Pooled 8 KB response buffers, requests and connection state; 24-byte idle
  connection slots.
- Fresh HS256 signature and `exp` check on every authenticated request. No response, result or token caching. No
  host settings or schema changes. gzip is available (`GZIP_MIN`) but off by default: it costs more CPU than it
  saves on a fast link.

Optional tuning variables: `WORKERS`, `DB_READ_CONNECTIONS`, `DB_WRITE_CONNECTIONS` (512 total at most),
`DB_BATCH`, `DB_FLUSH_US`, `GZIP_MIN`.

Tests: the 42 official checks, `integration.py` (7 extended checks) and `auth_codec_test.c`, also under ASan/UBSan.
Build output is excluded by `.gitignore`.
