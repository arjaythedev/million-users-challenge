/*
 * PostgreSQL, on the loop: every loop owns its share of the DB_POOL_SIZE connections and
 * drives them non-blocking from its own epoll, the libpq sockets registered in the h2o evloop
 * beside the clients. A request parks on its loop, its statement goes out on an idle
 * connection, and the row-ready callback answers it -- on the same thread, with no queue, no
 * worker and no context switch in between.
 *
 * Every statement is the one SPEC.md prescribes, prepared once per connection. Results stay in
 * the PGresult libpq produced: the loop reads the rows into the response and clears it.
 */
#ifndef DB_H
#define DB_H

#include <stdint.h>

#include <libpq-fe.h>

#include "h2o.h"

enum { JOB_FEED = 1, JOB_POST, JOB_CREATE, JOB_LIKE, JOB_PING };

/* 500 characters of at most 4 bytes each. */
#define DB_BODY_MAX 2000

typedef struct db_job {
    struct db_job *next;
    int kind;
    int64_t user_id;
    int64_t post_id; /* in: the post to read or like. out: the id of a created post */
    uint32_t body_len;
    /* Borrowed: whoever submits the job keeps these bytes in place until `done` ran. */
    const char *body;
    /* out */
    int status;       /* 200, 201, 404, 500 or 503 */
    PGresult *result; /* FEED, POST: the rows, binary. The loop clears it. NULL otherwise. */
    int64_t created_micros; /* CREATE: created_at, microseconds since 2000-01-01 UTC */
    char error[256];        /* PING: why the database did not answer */
} db_job;

/** Called on the loop thread for every job once its statement has returned (and, for a write,
 *  committed), or once it is known that it will not. */
typedef void (*db_done_fn)(db_job *job);

typedef struct db_pool db_pool;

/**
 * Opens up to @p count connections to @p url, one after the other, into @p out. A server that
 * refuses one because it has too many is not an error: the pool is what it took. Returns how
 * many are open; 0 is fatal for the caller.
 */
int db_dial(const char *url, int count, PGconn **out);

/** Puts @p count open connections under @p loop. Called on the loop's thread before it runs,
 *  or on it. */
db_pool *db_pool_create(h2o_loop_t *loop, PGconn **conns, int count, db_done_fn done);

/** Sends @p job on an idle connection of @p pool, or queues it. On the loop's thread only. 0,
 *  or -1 when the queue is full (the job is not taken and `done` will not run). */
int db_submit(db_pool *pool, db_job *job);

/* Column layout of the rows a FEED or POST result carries, binary: id int8, body text,
 * created_at timestamptz (int8, microseconds since 2000-01-01), username text, like_count int4. */
enum { COL_ID = 0, COL_BODY, COL_CREATED, COL_AUTHOR, COL_LIKES };

static inline int64_t db_int64(const char *p)
{
    uint64_t v = 0;

    for (int i = 0; i < 8; i++)
        v = v << 8 | (unsigned char)p[i];
    return (int64_t)v;
}

static inline int32_t db_int32(const char *p)
{
    uint32_t v = 0;

    for (int i = 0; i < 4; i++)
        v = v << 8 | (unsigned char)p[i];
    return (int32_t)v;
}

#endif
