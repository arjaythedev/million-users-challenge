#pragma once
#include "server.h"

// One Postgres session driven from the event loop (v3 wire protocol, SCRAM-SHA-256).
typedef struct pgconn {
    struct loop *l;
    int fd;
    bool write, alive, want_out, dirty;
    buf_t wbuf, rbuf;
    size_t rbuf_off;
    job_t *queue; uint32_t *qseq; size_t qcap, qhead, qlen;
    uint32_t sync_seq, zseq;
    int inflight, rows;
    buf_t body;
    int64_t next_retry; // monotonic seconds
    buf_t sendbuf;      // io_uring (PG_SQE=1): bytes handed to a SEND request, until its completion
    bool send_inflight;
} pgconn_t;
void pg_send_done(pgconn_t *p, int res); // io_uring: the SEND of sendbuf completed

void pg_init(pgconn_t *p, struct loop *l, bool write);
int pg_open(pgconn_t *p);            // blocking connect + auth + prepare; 0 on success
void pg_enqueue(pgconn_t *p, const job_t *j);
void pg_flush(pgconn_t *p);          // ends the batch (Sync for reads) and writes it out
void pg_on_readable(pgconn_t *p);
void pg_on_writable(pgconn_t *p);
void pg_die(pgconn_t *p, const char *why);
int pg_parse_url(pgcfg_t *c, const char *url);
