#pragma once
#include "server.h"
#include "pg.h"
#include <stdatomic.h>

typedef struct {
    uint32_t gen;
    uint8_t open, busy, close_after, closing; // closing: io_uring, waiting for the ring to let go of the fd
    uint8_t pending, recv_armed, pad[2];      // io_uring: sends in flight, multishot recv active
    uint32_t inlen, incap;
    uint8_t *inbuf;        // incomplete or pipelined request bytes
    uint32_t wlen, wcap;
    uint8_t *wbuf;         // response bytes the socket has not taken yet
} conn_t;

typedef struct { _Atomic uint64_t wakeups, events, accepts, closes, reqs, dbreqs, batches, sleeps; } stats_t;

typedef struct loop {
    int id, cpu, ep;
    int lfd[8], nl;
    conn_t *conns; size_t nconn_cap; int nconn;
    uint8_t *scratch;
    buf_t out, body_tmp;
    verifier_t ver;
    deflater_t gz;
    pgconn_t *pgr, *pgw; int nr, nw;
    pgconn_t *iter_read;
    stats_t st;
    bool paused; int64_t pause_until_ms;
    bool uring;            // this loop runs on io_uring (uring.c)
    void *ring;            // struct io_uring *
} loop_t;

enum { TAG_CLIENT = 0, TAG_LISTENER = 1, TAG_PG = 2 };
#define SCRATCH_SIZE (64 * 1024)
#define MAX_INBUF (MAX_HEADER_BYTES + MAX_BODY_BYTES)

void loop_init(loop_t *l, int id, int cpu, const int *lfds, int nl, const uint8_t *sample, size_t sample_len);
void *loop_run(void *arg);

// shared between the epoll and io_uring backends (loop.c)
conn_t *conn_at(loop_t *l, int fd);
void loop_ingest(loop_t *l, int fd, conn_t *c, const uint8_t *data, size_t n); // bytes read from a client
void loop_conn_reset(loop_t *l, int fd);                                        // the fd is closed: forget the connection
void loop_flush_jobs(loop_t *l);
void loop_reconnect_dead(loop_t *l);
void loop_pg_connect_all(loop_t *l);
void loop_pin(loop_t *l);
// io_uring backend (uring.c)
bool uring_init(loop_t *l);
void uring_run(loop_t *l);
void uring_send(loop_t *l, int fd, conn_t *c, const uint8_t *b, size_t n);
void uring_close(loop_t *l, int fd);
void uring_pg_poll(loop_t *l, pgconn_t *p, bool out);
