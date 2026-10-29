#include "db.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Jobs a pool queues per connection while all of them are busy. Beyond it a request is
 * answered 503 at once. With 32 connections per loop this is the loop's slots: a stall of the
 * database shows as latency first and as 503s only when it lasts. */
#define QUEUE_PER_CONN 128
/* How long to keep dialling a database that does not answer yet, at startup. */
#define CONNECT_ATTEMPTS 60
#define CONNECT_PAUSE_MS 500

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
    const char *values[] = {url, "disable", "5", "c-iouring-einhornimmond", NULL};

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

/* Statements one connection may have in flight: sent and not yet answered. */
#define DEPTH 32
/* Statements a pass puts on one connection before it takes the next: the batch one send
 * carries and one implicit transaction commits. */
#define BATCH 8

struct db_conn {
    db_pool *pool;
    PGconn *pg;
    int fd;  /* the descriptor the host watches for this connection; -1 while none */
    int tag; /* the host's */
    /* In flight, in the order they were sent: those whose results are still to come, then
     * those whose results are in and wait for the sync that commits their batch. */
    db_job *sent_head, *sent_tail;
    db_job *done_head, *done_tail;
    int inflight;  /* sent + done */
    int unsynced;  /* sent since the last sync: the batch under construction */
    int open;       /* a result was taken and its closing NULL is still to come */
    int after_sync; /* the last result was the sync's */
    int failed;    /* a statement of the batch being received errored: it rolled back */
    int dead;      /* not usable: reconnecting, or waiting to try again */
    int resetting; /* a PQresetPoll is in progress */
};

struct db_pool {
    db_done_fn done;
    const db_host *host;
    void *arg;
    db_conn *conns;
    int count;
    db_job *head, *tail; /* waiting for a connection */
    int queued, capacity;
    int dead_count;
    time_t complained; /* the second of the last complaint */
};

static void silence(void *arg, const char *message)
{
    (void)arg;
    (void)message;
}

/* Says what went wrong with the database, at most once a second per pool: the journal gets
 * the reason, not a line per failed request. */
static void complain(db_pool *p, const char *what, const char *why)
{
    const time_t now = time(NULL);

    if (now == p->complained)
        return;
    p->complained = now;
    fprintf(stderr, "database: %s: %.*s\n", what, (int)strcspn(why, "\n"), why);
}

static void attach(db_conn *c)
{
    const int fd = PQsocket(c->pg);
    db_pool *p = c->pool;

    if (c->fd == fd)
        return;
    if (c->fd >= 0)
        p->host->unwatch(p->arg, c, c->fd);
    c->fd = fd;
    p->host->watch_read(p->arg, c, fd);
}

static void detach(db_conn *c)
{
    db_pool *p = c->pool;

    if (c->fd >= 0)
        p->host->unwatch(p->arg, c, c->fd);
    c->fd = -1;
}

/* --- the queue ---------------------------------------------------------------------------- */

static void enqueue(db_pool *p, db_job *job)
{
    job->next = NULL;
    if (p->tail != NULL)
        p->tail->next = job;
    else
        p->head = job;
    p->tail = job;
    p->queued++;
}

/* Puts a chain of jobs (in order, `next`-linked, NULL-terminated) ahead of the queue. */
static void enqueue_front(db_pool *p, db_job *first, db_job *last, int n)
{
    last->next = p->head;
    p->head = first;
    if (p->tail == NULL)
        p->tail = last;
    p->queued += n;
}

static db_job *dequeue(db_pool *p)
{
    db_job *job = p->head;

    p->head = job->next;
    if (p->head == NULL)
        p->tail = NULL;
    p->queued--;
    return job;
}

/* The connection a job goes on. A pass puts its statements on few connections, `BATCH`
 * each, so that one send carries all of them and one sync commits them; among the connections
 * still filling their batch, the one with the least left from the passes before, so that no
 * connection runs far ahead of the others and a statement waits for a whole batch's worth of
 * results. NULL when every one of them is full. */
static db_conn *pick(db_pool *p)
{
    db_conn *best = NULL, *least = NULL;

    for (int i = 0; i < p->count; i++) {
        db_conn *c = &p->conns[i];

        if (c->dead || c->inflight >= DEPTH)
            continue;
        if (c->unsynced < BATCH) {
            if (best == NULL || c->inflight - c->unsynced < best->inflight - best->unsynced)
                best = c;
        } else if (least == NULL || c->inflight < least->inflight) {
            least = c;
        }
    }
    return best != NULL ? best : least;
}

static void start(db_conn *c, db_job *job);

static void pump(db_pool *p)
{
    while (p->head != NULL) {
        db_conn *c = pick(p);

        if (c == NULL)
            return;
        start(c, dequeue(p));
    }
}

/* --- sending ------------------------------------------------------------------------------ */

static void kill_conn(db_conn *c, const char *why);

static void flush(db_conn *c)
{
    db_pool *p = c->pool;

    switch (PQflush(c->pg)) {
    case 0: return;
    case 1: p->host->watch_write(p->arg, c, c->fd); return;
    default: kill_conn(c, "connection lost while sending"); return;
    }
}

static void start(db_conn *c, db_job *job)
{
    char user_id[24], post_id[24];
    int sent;

    job->status = 500;
    job->result = NULL;
    job->errored = 0;
    job->next = NULL;
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
        sent = PQsendQueryParams(c->pg, "SELECT 1", 0, NULL, NULL, NULL, NULL, 0);
        break;
    }
    if (!sent) {
        /* The job was never on the wire; it fails with the connection. */
        job->status = job->kind == JOB_PING ? 503 : 500;
        snprintf(job->error, sizeof job->error, "%s", PQerrorMessage(c->pg));
        kill_conn(c, PQerrorMessage(c->pg));
        c->pool->done(job);
        return;
    }
    if (c->sent_tail != NULL)
        c->sent_tail->next = job;
    else
        c->sent_head = job;
    c->sent_tail = job;
    c->inflight++;
    c->unsynced++;
}

/* Closes the batch on @p c: the sync commits it on the server, and its results come back
 * followed by the sync's own. */
static void sync_conn(db_conn *c)
{
    if (!PQpipelineSync(c->pg)) {
        kill_conn(c, PQerrorMessage(c->pg));
        return;
    }
    c->unsynced = 0;
    flush(c);
}

void db_pool_flush(db_pool *p)
{
    for (int i = 0; i < p->count; i++) {
        db_conn *c = &p->conns[i];

        if (!c->dead && c->unsynced != 0)
            sync_conn(c);
    }
}

void db_on_writable(db_conn *c);

/* --- losing a session --------------------------------------------------------------------- */

static void reset_begin(db_conn *c);

/* Everything in flight on @p c, failed; the lists are empty and the job chain returned. */
static db_job *fail_all(db_conn *c, const char *why)
{
    db_job *first = c->sent_head;

    if (c->sent_tail != NULL)
        c->sent_tail->next = c->done_head;
    else
        first = c->done_head;
    c->sent_head = c->sent_tail = c->done_head = c->done_tail = NULL;
    c->inflight = c->unsynced = c->open = c->after_sync = c->failed = 0;
    for (db_job *job = first; job != NULL; job = job->next) {
        job->status = job->kind == JOB_PING ? 503 : 500;
        snprintf(job->error, sizeof job->error, "%s", why);
        if (job->result != NULL) {
            PQclear(job->result);
            job->result = NULL;
        }
    }
    return first;
}

/* The session is gone. Whatever was in flight is lost: a read could be repeated, but whether
 * a write committed before the connection went is exactly what cannot be known, so neither is. */
static void kill_conn(db_conn *c, const char *why)
{
    db_pool *p = c->pool;
    db_job *failed;

    if (c->dead)
        return;
    c->dead = 1;
    p->dead_count++;
    complain(p, "connection lost", why);
    detach(c);
    failed = fail_all(c, why);
    reset_begin(c);
    while (failed != NULL) {
        db_job *job = failed;

        failed = job->next;
        p->done(job);
    }
}

static void reset_later(db_conn *c)
{
    c->resetting = 0;
    detach(c);
}

static void reset_step(db_conn *c)
{
    db_pool *p = c->pool;
    const PostgresPollingStatusType st = PQresetPoll(c->pg);

    switch (st) {
    case PGRES_POLLING_READING:
        attach(c);
        return;
    case PGRES_POLLING_WRITING:
        attach(c);
        p->host->watch_write(p->arg, c, c->fd);
        return;
    case PGRES_POLLING_OK:
        /* Four round trips, blocking, a few milliseconds once per lost session. */
        if (prepare_all(c->pg) != 0 || PQsetnonblocking(c->pg, 1) != 0 || !PQenterPipelineMode(c->pg)) {
            reset_later(c);
            return;
        }
        c->resetting = 0;
        c->dead = 0;
        p->dead_count--;
        attach(c);
        pump(p);
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

void db_pool_tick(db_pool *p)
{
    for (int i = 0; i < p->count && p->dead_count > 0; i++)
        if (p->conns[i].dead && !p->conns[i].resetting)
            reset_begin(&p->conns[i]);
}

void db_on_writable(db_conn *c)
{
    if (c->resetting)
        reset_step(c);
    else if (!c->dead)
        flush(c);
}

/* --- receiving ---------------------------------------------------------------------------- */

static void take_result(db_conn *c, PGresult *r)
{
    db_job *job = c->sent_head;
    const ExecStatusType st = PQresultStatus(r);

    if (job == NULL || job->result != NULL) {
        PQclear(r);
        return;
    }
    if (st == PGRES_PIPELINE_ABORTED) {
        /* Skipped: a statement before it in the batch errored. Sent again after the sync. */
        c->failed = 1;
        PQclear(r);
        return;
    }
    if (st == PGRES_FATAL_ERROR || st == PGRES_NONFATAL_ERROR) {
        const char *state = PQresultErrorField(r, PG_DIAG_SQLSTATE);

        c->failed = 1;
        if (state != NULL && (strcmp(state, "40P01") == 0 || strcmp(state, "40001") == 0)) {
            /* A deadlock or a serialization failure: the server's choice of victim, nothing
             * the statement did. It is sent again with the rest of the batch. */
            PQclear(r);
            return;
        }
        /* Its own doing; the rest of the batch rolls back with it and is sent again. */
        job->errored = 1;
        if (state == NULL || strcmp(state, "23503") != 0)
            complain(c->pool, "statement failed", PQresultErrorMessage(r));
    }
    switch (job->kind) {
    case JOB_FEED:
    case JOB_POST:
        if (st == PGRES_TUPLES_OK) {
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
        if (st == PGRES_TUPLES_OK && PQntuples(r) == 1) {
            job->post_id = db_int64(PQgetvalue(r, 0, 0));
            job->created_micros = db_int64(PQgetvalue(r, 0, 1));
            job->status = 201;
        }
        break;
    case JOB_LIKE:
        if (st == PGRES_COMMAND_OK) {
            const char *tuples = PQcmdTuples(r);

            job->status = tuples != NULL && tuples[0] == '1' ? 201 : 200;
        } else {
            const char *state = PQresultErrorField(r, PG_DIAG_SQLSTATE);

            /* The foreign key on post_id: no such post. */
            job->status = state != NULL && strcmp(state, "23503") == 0 ? 404 : 500;
        }
        break;
    default:
        if (st == PGRES_TUPLES_OK) {
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

/* The statement at the head of the sent list has all its results: it waits for the sync. */
static void close_statement(db_conn *c)
{
    db_job *job = c->sent_head;

    if (job == NULL)
        return;
    c->sent_head = job->next;
    if (c->sent_head == NULL)
        c->sent_tail = NULL;
    job->next = NULL;
    if (c->done_tail != NULL)
        c->done_tail->next = job;
    else
        c->done_head = job;
    c->done_tail = job;
}

/* The sync is in: the batch is committed -- or, when a statement of it errored, rolled back:
 * then that statement is answered and the others, which did nothing, are sent once more. */
static void deliver_batch(db_conn *c)
{
    db_pool *p = c->pool;
    db_job *job = c->done_head;
    db_job *again_head = NULL, *again_tail = NULL;
    db_job *done_head = NULL, *done_tail = NULL;
    const int failed = c->failed;
    int again = 0;

    c->done_head = c->done_tail = NULL;
    c->failed = 0;
    while (job != NULL) {
        db_job *next = job->next;
        db_job **head, **tail;

        job->next = NULL;
        c->inflight--;
        if (failed && !job->errored) {
            if (job->result != NULL) {
                PQclear(job->result);
                job->result = NULL;
            }
            head = &again_head, tail = &again_tail;
            again++;
        } else {
            head = &done_head, tail = &done_tail;
        }
        if (*tail != NULL)
            (*tail)->next = job;
        else
            *head = job;
        *tail = job;
        job = next;
    }
    if (again != 0)
        enqueue_front(p, again_head, again_tail, again);
    pump(p);
    while (done_head != NULL) {
        job = done_head;
        done_head = job->next;
        job->next = NULL;
        p->done(job);
    }
}

void db_on_readable(db_conn *c, int hangup)
{
    if (c->resetting) {
        reset_step(c);
        return;
    }
    if (c->dead)
        return;
    if (!PQconsumeInput(c->pg)) {
        kill_conn(c, PQerrorMessage(c->pg));
        return;
    }
    /* A statement's results are followed by a NULL that closes it. The sync's result is not
     * (libpq 14 to 16; later ones may add a NULL, which is let pass). */
    while (!PQisBusy(c->pg)) {
        PGresult *r = PQgetResult(c->pg);

        if (r == NULL) {
            if (c->open) {
                c->open = 0;
                close_statement(c);
                continue;
            }
            if (c->after_sync) {
                c->after_sync = 0;
                continue;
            }
            /* Nothing in flight and nothing said: the server spoke on its own. */
            if (PQstatus(c->pg) != CONNECTION_OK)
                kill_conn(c, "connection lost");
            break;
        }
        c->after_sync = 0;
        if (PQresultStatus(r) == PGRES_PIPELINE_SYNC) {
            PQclear(r);
            c->after_sync = 1;
            deliver_batch(c);
            if (c->dead)
                return;
        } else {
            c->open = 1;
            take_result(c, r);
        }
    }
    /* A hangup that came with the last bytes (the server's FATAL, then its close): the read
     * above took the bytes, this one meets the end. The host will not say it again -- it
     * reports edges, not levels. */
    if (hangup && !c->dead && !c->resetting && !PQconsumeInput(c->pg))
        kill_conn(c, PQerrorMessage(c->pg));
}

/* --- the interface ------------------------------------------------------------------------ */

db_pool *db_pool_create(PGconn **conns, int count, db_done_fn done, const db_host *host, void *arg)
{
    db_pool *p = calloc(1, sizeof *p);

    if (p == NULL || (p->conns = calloc((size_t)count, sizeof *p->conns)) == NULL)
        abort();
    p->done = done;
    p->host = host;
    p->arg = arg;
    p->count = count;
    p->capacity = count * QUEUE_PER_CONN;
    for (int i = 0; i < count; i++) {
        db_conn *c = &p->conns[i];

        c->pool = p;
        c->pg = conns[i];
        c->fd = -1;
        c->tag = -1;
        /* A FATAL the server sends on its own -- it is going down, or an administrator ended
         * the session -- would otherwise be printed; it is handled where the socket closes. */
        PQsetNoticeProcessor(c->pg, silence, NULL);
        if (PQsetnonblocking(c->pg, 1) != 0 || !PQenterPipelineMode(c->pg))
            abort();
        attach(c);
    }
    return p;
}

int db_submit(db_pool *p, db_job *job)
{
    if (p->head == NULL) {
        db_conn *c = pick(p);

        if (c != NULL) {
            start(c, job);
            return 0;
        }
    }
    if (p->queued >= p->capacity)
        return -1;
    enqueue(p, job);
    return 0;
}

int db_conn_tag(const db_conn *c)
{
    return c->tag;
}

void db_conn_set_tag(db_conn *c, int tag)
{
    c->tag = tag;
}
