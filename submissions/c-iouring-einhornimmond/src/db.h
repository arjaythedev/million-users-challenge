/*
 * PostgreSQL, on the loop: every loop owns its share of the DB_POOL_SIZE connections and
 * drives them non-blocking from its own ring, the libpq sockets watched beside the clients. A
 * request parks on its loop, its statement goes out on a connection of the loop, and the
 * row-ready callback answers it -- on the same thread, with no queue, no worker and no context
 * switch in between.
 *
 * The connections run in libpq's pipeline mode: the statements a pass puts on one connection
 * go out in one send, closed by one sync, and come back in few reads. The sync is where the
 * server commits them (one implicit transaction per batch), so a job is done only when the
 * sync's result is in. A batch one statement of which errors rolls back: that statement is
 * answered and the others are sent once more.
 *
 * The pool knows nothing of the ring: the host watches descriptors for it (db_host) and calls
 * db_on_readable and db_on_writable when they are ready.
 *
 * Every statement is the one SPEC.md prescribes, prepared once per connection. Results stay in
 * the PGresult libpq produced: the loop reads the rows into the response and clears it.
 */
#ifndef DB_H
#define DB_H

#include <stdint.h>

#include <libpq-fe.h>

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
    int errored;      /* the pool's: the statement itself failed (not the batch around it) */
    PGresult *result; /* FEED, POST: the rows, binary. The loop clears it. NULL otherwise. */
    int64_t created_micros; /* CREATE: created_at, microseconds since 2000-01-01 UTC */
    char error[256];        /* PING: why the database did not answer */
} db_job;

/** Called on the loop thread for every job once its statement has returned (and, for a write,
 *  committed), or once it is known that it will not. */
typedef void (*db_done_fn)(db_job *job);

typedef struct db_pool db_pool;
typedef struct db_conn db_conn;

/* What the pool asks of its host. Every call comes on the host's thread, from inside a call
 * into the pool. */
typedef struct db_host {
    /** Watch @p fd for readability, and keep watching, calling db_on_readable(@p c) each time;
     *  also when it hangs up or errors. */
    void (*watch_read)(void *arg, db_conn *c, int fd);
    /** Call db_on_writable(@p c) once @p fd is writable. */
    void (*watch_write)(void *arg, db_conn *c, int fd);
    /** @p fd is no longer @p c's: stop watching it. Nothing more is called for it. */
    void (*unwatch)(void *arg, db_conn *c, int fd);
} db_host;

/**
 * Opens up to @p count connections to @p url, one after the other, into @p out. A server that
 * refuses one because it has too many is not an error: the pool is what it took. Returns how
 * many are open; 0 is fatal for the caller.
 */
int db_dial(const char *url, int count, PGconn **out);

/** Puts @p count open connections under a host. Called on the host's thread. */
db_pool *db_pool_create(PGconn **conns, int count, db_done_fn done, const db_host *host, void *arg);

/** Sends @p job on a connection of @p pool with room, or queues it. 0, or -1 when the queue is
 *  full (the job is not taken and `done` will not run). */
int db_submit(db_pool *pool, db_job *job);

/** At the end of a pass: sends the sync that closes every batch the pass built. */
void db_pool_flush(db_pool *pool);

/** The host's word: the watched descriptor of @p c is readable, or writable. @p hangup says
 *  the peer has closed or the socket errored -- the readable data may be the last of it. */
void db_on_readable(db_conn *c, int hangup);
void db_on_writable(db_conn *c);

/** About once a second: dials again what the server dropped and could not be dialled yet. */
void db_pool_tick(db_pool *pool);

/** A number the host keeps on a connection; -1 until it sets one. */
int db_conn_tag(const db_conn *c);
void db_conn_set_tag(db_conn *c, int tag);

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
