#include "db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Jobs a pool queues per connection while all of them are busy. Beyond it a request is
 * answered 503 at once. With 32 connections per loop this is the loop's slots: a stall of the
 * database shows as latency first and as 503s only when it lasts. */
#define QUEUE_PER_CONN 128
/* How long to keep dialling a database that does not answer yet, at startup. */
#define CONNECT_ATTEMPTS 60
#define CONNECT_PAUSE_MS 500
/* How long after a failed attempt a connection the server dropped is dialled again. The
 * dialling itself is non-blocking (PQresetStart/PQresetPoll on the loop). */
#define REVIVE_MS 1000

/* --- the statements, as SPEC.md prescribes them ------------------------------------------- */

#define POST_SELECT                                                                              \
    "SELECT p.id, p.body, p.created_at, u.username, "                                           \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "                 \
    "FROM posts p JOIN users u ON u.id = p.user_id"

static const struct {
    const char *name, *sql;
    int params;
    Oid types[2];
} statements[] = {
    {"feed", POST_SELECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", 0, {0, 0}},
    {"post", POST_SELECT " WHERE p.id = $1", 1, {20, 0}},
    {"create", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", 2, {20, 25}},
    {"like", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", 2, {20, 20}},
};

/* --- connecting --------------------------------------------------------------------------- */

static int prepare_all(PGconn *conn)
{
    for (size_t i = 0; i < sizeof statements / sizeof statements[0]; i++) {
        PGresult *r = PQprepare(conn, statements[i].name, statements[i].sql, statements[i].params,
                                statements[i].types);
        const int ok = r != NULL && PQresultStatus(r) == PGRES_COMMAND_OK;

        if (!ok)
            fprintf(stderr, "prepare %s: %s", statements[i].name, PQerrorMessage(conn));
        PQclear(r);
        if (!ok)
            return -1;
    }
    return 0;
}

/* Connects, retrying while the server is not there yet. NULL when the server answered that it
 * will not have one more connection, or when it keeps not answering; *refused tells which. */
static PGconn *dial(const char *url, int *refused)
{
    const char *keys[] = {"dbname", "sslmode", "connect_timeout", "application_name", NULL};
    const char *values[] = {url, "disable", "5", "c-h2o-einhornimmond", NULL};

    *refused = 0;
    for (int attempt = 0; attempt < CONNECT_ATTEMPTS; attempt++) {
        PGconn *conn = PQconnectdbParams(keys, values, 1);

        if (conn != NULL && PQstatus(conn) == CONNECTION_OK) {
            if (strcmp(PQparameterStatus(conn, "integer_datetimes"), "on") != 0) {
                fprintf(stderr, "this server has integer_datetimes off; timestamps would be misread\n");
                PQfinish(conn);
                *refused = 1;
                return NULL;
            }
            if (prepare_all(conn) != 0) {
                PQfinish(conn);
                *refused = 1;
                return NULL;
            }
            return conn;
        }
        if (conn != NULL) {
            const char *message = PQerrorMessage(conn);
            /* A FATAL is the server's own word: too many connections, or a database or a role
             * that does not exist. It answered, and the answer is no -- dialling again would
             * get the same. Everything else (refused, unreachable, timed out) is a server that
             * is not there yet. The text is localized, the severity tag is not. */
            *refused = strstr(message, "FATAL") != NULL;
            if (*refused || attempt == CONNECT_ATTEMPTS - 1)
                fprintf(stderr, "database: %s", message);
            PQfinish(conn);
            if (*refused)
                return NULL;
        }
        usleep(CONNECT_PAUSE_MS * 1000);
    }
    return NULL;
}

int db_dial(const char *url, int count, PGconn **out)
{
    int n = 0, refused;

    while (n < count && (out[n] = dial(url, &refused)) != NULL)
        n++;
    return n;
}

/* --- the pool ----------------------------------------------------------------------------- */

typedef struct db_conn {
    db_pool *pool;
    PGconn *pg;
    h2o_socket_t *sock; /* on a dup of libpq's fd: h2o closes its own, libpq its own */
    int sock_fd;        /* libpq's fd the dup was taken of; it changes while reconnecting */
    db_job *job;        /* the statement in flight, NULL when idle or dead */
    struct db_conn *next_idle;
    int dead;           /* not usable: reconnecting, or waiting to try again */
    int resetting;      /* a PQresetPoll is in progress */
} db_conn;

struct db_pool {
    h2o_loop_t *loop;
    db_done_fn done;
    db_conn *conns;
    int count;
    db_conn *idle;
    db_job *head, *tail; /* waiting for a connection */
    int queued, capacity;
    h2o_timer_t revive;
    int dead_count;
};

static void on_readable(h2o_socket_t *sock, const char *err);
static void start(db_conn *c, db_job *job);

static void silence(void *arg, const char *message)
{
    (void)arg;
    (void)message;
}

/* Watches libpq's current fd with @p on_ready; a watch on an earlier fd is dropped. */
static void attach(db_conn *c, h2o_socket_cb on_ready)
{
    const int fd = PQsocket(c->pg);

    if (c->sock != NULL && c->sock_fd != fd) {
        h2o_socket_close(c->sock);
        c->sock = NULL;
    }
    if (c->sock == NULL) {
        const int copy = dup(fd);

        if (copy < 0)
            abort();
        c->sock = h2o_evloop_socket_create(c->pool->loop, copy, H2O_SOCKET_FLAG_DONT_READ);
        c->sock->data = c;
        c->sock_fd = fd;
    }
    h2o_socket_read_stop(c->sock);
    h2o_socket_read_start(c->sock, on_ready);
}

static void push_idle(db_conn *c)
{
    db_pool *p = c->pool;

    if (p->head != NULL) {
        db_job *job = p->head;

        p->head = job->next;
        if (p->head == NULL)
            p->tail = NULL;
        p->queued--;
        start(c, job);
        return;
    }
    c->next_idle = p->idle;
    p->idle = c;
}

/* The job on @p c is over, one way or the other: give it back, then take the next. */
static void finish(db_conn *c)
{
    db_job *job = c->job;

    c->job = NULL;
    if (!c->dead)
        push_idle(c);
    c->pool->done(job);
}

static void revive(h2o_timer_t *timer);
static void reset_begin(db_conn *c);

/* The session is gone. Whatever was in flight is lost: a read could be repeated, but whether
 * a write committed before the connection went is exactly what cannot be known, so neither is. */
static void kill_conn(db_conn *c, const char *why)
{
    db_pool *p = c->pool;

    if (c->dead)
        return;
    c->dead = 1;
    p->dead_count++;
    if (c->sock != NULL) {
        h2o_socket_close(c->sock);
        c->sock = NULL;
    }
    if (c->job != NULL) {
        db_job *job = c->job;

        job->status = job->kind == JOB_PING ? 503 : 500;
        snprintf(job->error, sizeof job->error, "%s", why);
        finish(c);
    }
    reset_begin(c);
}

/* --- reconnecting, without blocking the loop ---------------------------------------------- */

static void reset_step(db_conn *c);

static void on_reset_ready(h2o_socket_t *sock, const char *err)
{
    db_conn *c = sock->data;

    (void)err;
    reset_step(c);
}

static void on_reset_writable(h2o_socket_t *sock, const char *err)
{
    db_conn *c = sock->data;

    (void)err;
    reset_step(c);
}

/* Reconnecting failed or cannot start: try again later. */
static void reset_later(db_conn *c)
{
    db_pool *p = c->pool;

    c->resetting = 0;
    if (c->sock != NULL) {
        h2o_socket_close(c->sock);
        c->sock = NULL;
    }
    if (!h2o_timer_is_linked(&p->revive))
        h2o_timer_link(p->loop, REVIVE_MS, &p->revive);
}

static void reset_step(db_conn *c)
{
    switch (PQresetPoll(c->pg)) {
    case PGRES_POLLING_READING:
        attach(c, on_reset_ready);
        return;
    case PGRES_POLLING_WRITING:
        attach(c, on_reset_ready);
        h2o_socket_notify_write(c->sock, on_reset_writable);
        return;
    case PGRES_POLLING_OK:
        /* Four round trips, blocking, a few milliseconds once per lost session. */
        if (prepare_all(c->pg) != 0 || PQsetnonblocking(c->pg, 1) != 0) {
            reset_later(c);
            return;
        }
        c->resetting = 0;
        c->dead = 0;
        c->pool->dead_count--;
        attach(c, on_readable);
        push_idle(c);
        return;
    default:
        reset_later(c);
        return;
    }
}

static void reset_begin(db_conn *c)
{
    if (c->resetting)
        return;
    if (!PQresetStart(c->pg)) {
        reset_later(c);
        return;
    }
    c->resetting = 1;
    reset_step(c);
}

static void revive(h2o_timer_t *timer)
{
    db_pool *p = H2O_STRUCT_FROM_MEMBER(db_pool, revive, timer);

    for (int i = 0; i < p->count; i++)
        if (p->conns[i].dead && !p->conns[i].resetting)
            reset_begin(&p->conns[i]);
}

/* --- sending ------------------------------------------------------------------------------ */

static void on_writable(h2o_socket_t *sock, const char *err);

static void flush(db_conn *c)
{
    switch (PQflush(c->pg)) {
    case 0: return;
    case 1: h2o_socket_notify_write(c->sock, on_writable); return;
    default: kill_conn(c, "connection lost while sending"); return;
    }
}

static void on_writable(h2o_socket_t *sock, const char *err)
{
    db_conn *c = sock->data;

    if (err != NULL) {
        kill_conn(c, err);
        return;
    }
    flush(c);
}

static void start(db_conn *c, db_job *job)
{
    char user_id[24], post_id[24];
    int sent;

    c->job = job;
    job->status = 500;
    job->result = NULL;
    snprintf(user_id, sizeof user_id, "%lld", (long long)job->user_id);
    snprintf(post_id, sizeof post_id, "%lld", (long long)job->post_id);
    switch (job->kind) {
    case JOB_FEED:
        sent = PQsendQueryPrepared(c->pg, "feed", 0, NULL, NULL, NULL, 1);
        break;
    case JOB_POST: {
        const char *values[1] = {post_id};
        sent = PQsendQueryPrepared(c->pg, "post", 1, values, NULL, NULL, 1);
        break;
    }
    case JOB_CREATE: {
        const char *values[2] = {user_id, job->body};
        const int lengths[2] = {0, (int)job->body_len};
        const int formats[2] = {0, 1};
        sent = PQsendQueryPrepared(c->pg, "create", 2, values, lengths, formats, 1);
        break;
    }
    case JOB_LIKE: {
        const char *values[2] = {user_id, post_id};
        sent = PQsendQueryPrepared(c->pg, "like", 2, values, NULL, NULL, 0);
        break;
    }
    default:
        sent = PQsendQuery(c->pg, "SELECT 1");
        break;
    }
    if (!sent) {
        kill_conn(c, PQerrorMessage(c->pg));
        return;
    }
    flush(c);
}

/* --- receiving ---------------------------------------------------------------------------- */

static void take_result(db_conn *c, PGresult *r)
{
    db_job *job = c->job;

    if (job == NULL || job->result != NULL) {
        PQclear(r);
        return;
    }
    switch (job->kind) {
    case JOB_FEED:
    case JOB_POST:
        if (PQresultStatus(r) == PGRES_TUPLES_OK) {
            if (PQntuples(r) == 0 && job->kind == JOB_POST) {
                job->status = 404;
            } else {
                job->status = 200;
                job->result = r;
                return;
            }
        }
        break;
    case JOB_CREATE:
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) {
            job->post_id = db_int64(PQgetvalue(r, 0, 0));
            job->created_micros = db_int64(PQgetvalue(r, 0, 1));
            job->status = 201;
        }
        break;
    case JOB_LIKE:
        if (PQresultStatus(r) == PGRES_COMMAND_OK) {
            const char *tuples = PQcmdTuples(r);

            job->status = tuples != NULL && tuples[0] == '1' ? 201 : 200;
        } else {
            const char *state = PQresultErrorField(r, PG_DIAG_SQLSTATE);

            /* The foreign key on post_id: no such post. */
            job->status = state != NULL && strcmp(state, "23503") == 0 ? 404 : 500;
        }
        break;
    default:
        if (PQresultStatus(r) == PGRES_TUPLES_OK) {
            job->status = 200;
        } else {
            job->status = 503;
            snprintf(job->error, sizeof job->error, "%s", PQresultErrorMessage(r));
            job->error[strcspn(job->error, "\n")] = '\0';
        }
        break;
    }
    PQclear(r);
}

static void on_readable(h2o_socket_t *sock, const char *err)
{
    db_conn *c = sock->data;

    if (err != NULL) {
        kill_conn(c, err);
        return;
    }
    if (!PQconsumeInput(c->pg)) {
        kill_conn(c, PQerrorMessage(c->pg));
        return;
    }
    while (!PQisBusy(c->pg)) {
        PGresult *r = PQgetResult(c->pg);

        if (r == NULL) {
            /* The statement is complete -- or the server spoke while nothing was in flight. */
            if (c->job != NULL)
                finish(c);
            else if (PQstatus(c->pg) != CONNECTION_OK)
                kill_conn(c, "connection lost");
            return;
        }
        take_result(c, r);
    }
}

/* --- the interface ------------------------------------------------------------------------ */

db_pool *db_pool_create(h2o_loop_t *loop, PGconn **conns, int count, db_done_fn done)
{
    db_pool *p = calloc(1, sizeof *p);

    if (p == NULL || (p->conns = calloc((size_t)count, sizeof *p->conns)) == NULL)
        abort();
    p->loop = loop;
    p->done = done;
    p->count = count;
    p->capacity = count * QUEUE_PER_CONN;
    h2o_timer_init(&p->revive, revive);
    for (int i = 0; i < count; i++) {
        db_conn *c = &p->conns[i];

        c->pool = p;
        c->pg = conns[i];
        /* A FATAL the server sends on its own -- it is going down, or an administrator ended
         * the session -- would otherwise be printed; it is handled where the socket closes. */
        PQsetNoticeProcessor(c->pg, silence, NULL);
        if (PQsetnonblocking(c->pg, 1) != 0)
            abort();
        attach(c, on_readable);
        c->next_idle = p->idle;
        p->idle = c;
    }
    return p;
}

int db_submit(db_pool *p, db_job *job)
{
    db_conn *c = p->idle;

    job->next = NULL;
    if (c != NULL) {
        p->idle = c->next_idle;
        start(c, job);
        return 0;
    }
    if (p->queued >= p->capacity)
        return -1;
    if (p->tail != NULL)
        p->tail->next = job;
    else
        p->head = job;
    p->tail = job;
    p->queued++;
    return 0;
}
