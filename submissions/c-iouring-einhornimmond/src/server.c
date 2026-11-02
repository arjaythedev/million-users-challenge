/*
 * The HTTP side: one io_uring loop per core, each on its own thread with its own ring, its own
 * listening sockets under SO_REUSEPORT (one per port the API is served on) and its own share
 * of the database connections.
 *
 *   accept    multishot, into the ring's table of direct descriptors
 *   receive   multishot, into buffers lent from a ring of provided buffers
 *   database  the libpq sockets are watched by the same ring (poll); a request waiting for
 *             its rows is a slot on its loop, answered when they are in
 *
 * The loop enters the kernel once per pass: that one call hands over every response of the
 * pass before and brings back every request that arrived since. Under load it lets requests
 * collect for a moment first, so the cost of the call is shared by all of them.
 *
 * Nothing crosses a thread. Responses are rendered on the loop into a buffer of its own,
 * gzip'd there when the client asked for it and the body is worth it, and copied once behind
 * their head to be sent.
 */
#include "server.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "picohttpparser.h"

#include "arnm/arena.h"
#include "arnm/json_reader.h"

#include "db.h"
#include "gz.h"
#include "jwt.h"
#include "parse_id.h"
#include "uring.h"

/* Requests one loop may have parked at the database. Beyond it a request is answered 503.
 * Sized to ride out a database that stalls for a second or two (a checkpoint) at a loop's share
 * of 42,000 requests per second: 4096 slots of 2.3 KB are 9 MB per loop. */
#define SLOTS 4096
/* How long a keep-alive connection may sit idle. The spec asks for 75 s; a user of the load
 * profile is quiet for 37.5 s at most, and an idle connection costs nothing but its socket. */
#define IDLE_TIMEOUT_S 150
/* How long a request that arrived in part, or a response the client does not take, may make no
 * progress. */
#define STALL_TIMEOUT_S 30
#define SWEEP_S 5
#define MAX_HEAD (16 * 1024)
#define MAX_BODY (64 * 1024)
#define MAX_HEADERS 32
/* Bytes one connection may have waiting: a request that is still arriving, or requests sent
 * ahead of the answer to the one before. */
#define MAX_PENDING (MAX_HEAD + MAX_BODY + 4096)
/* The most connections one loop's table is made for, whatever the descriptor limit says. */
#define MAX_CONNS_PER_LOOP (1u << 20)
#define LISTENERS 2

#define SQ_ENTRIES 4096
#define CQ_ENTRIES 16384
/* Buffers lent to receives. A request fits one; a longer one arrives in several. */
#define RECV_BUFFERS 4096
#define RECV_BUFFER_SIZE 2048
/* Under load the loop waits for this many completions or this long, whichever comes first.
 * Measured: without the wait a request costs a little more, with a longer one no less. */
#define WAIT_COMPLETIONS 32
#define WAIT_NS 1000000
#define IDLE_WAIT_NS 1000000000

/* A rendered feed: 20 posts of at most 2000 bytes, every byte escaped at worst. */
#define JSON_CAP (20u * (6u * DB_BODY_MAX + 1200u) + 256u)
/* Bodies below this go out as they are: a post object of 300 bytes shrinks by a quarter and
 * costs a deflate; a feed shrinks to a quarter. */
#define GZIP_MIN 1024
/* The arena a request body is parsed in: the parser's copy of the input and its values. */
#define PARSE_ARENA (8u * MAX_BODY + 4096u)
/* The status line and the headers of a response. */
#define HEAD_MAX 160

/* One per descriptor in the ring's table, at the descriptor's index. */
typedef struct conn {
    /* Input that could not be served when it arrived: the start of a request, or requests sent
     * ahead. NULL while there is none, which is nearly always. */
    char *in;
    uint32_t in_len, in_cap;
    /* What is left of the response on its way out, and the buffer it lies in (NULL for a
     * literal), freed when it has gone. */
    const char *out;
    char *out_buf;
    uint32_t out_len;
    uint32_t last_active;
    bool open : 1;
    bool receiving : 1;   /* the multishot receive is armed */
    bool sending : 1;     /* a send is in the ring */
    bool parked : 1;      /* a request of this connection is at the database */
    bool continued : 1;   /* 100 Continue was sent for the request that is arriving */
    bool close_after : 1; /* close once the response is out */
    bool closing : 1;     /* no more requests; closed when nothing of it is in flight */
    bool close_sent : 1;
} conn;

typedef struct slot {
    db_job job;
    conn *conn;
    bool gzip; /* the client takes gzip */
    size_t author_len;
    char author[JWT_NAME_MAX];
    /* A create's trimmed body: the job's `body` points here. */
    char body[DB_BODY_MAX];
    struct slot *next_free;
} slot;

/* A database connection as the ring sees it: its descriptor, and a generation that tells a
 * completion for an earlier descriptor of the same connection from a current one. */
typedef struct dbwatch {
    db_conn *conn;
    int fd;
    uint32_t gen;
} dbwatch;

typedef struct loop {
    uring ring;
    conn *conns;
    uint32_t max_conns;
    uint32_t now;    /* seconds, read once per pass */
    uint64_t now_ms; /* milliseconds, the same clock */
    int listen_fds[LISTENERS];
    bool accepting[LISTENERS];
    int listener_count;
    db_pool *db;
    PGconn **pg; /* handed over at start, owned by the pool from then on */
    int pg_count;
    dbwatch *dbw;
    int dbw_count;
    slot *slots, *free_slots;
    gz_ctx *gz;
    char *json, *gzbuf;
    uint8_t *parse_buf;
    uint32_t swept;
    uint64_t ticked_ms;
    pthread_t thread;
    int index;
} loop;

static struct timespec started;
static __thread loop *self;

/* --- the ring ----------------------------------------------------------------------------- */

enum { OP_ACCEPT = 1, OP_RECV, OP_SEND, OP_CLOSE, OP_CANCEL, OP_DB_READ, OP_DB_WRITE };

#define USER_DATA(op, gen, index) ((uint64_t)(op) << 56 | (uint64_t)((gen) & 0xFFFFFF) << 32 | (uint64_t)(index))
#define OP_OF(user_data) ((int)((user_data) >> 56))
#define GEN_OF(user_data) ((uint32_t)((user_data) >> 32) & 0xFFFFFF)
#define INDEX_OF(user_data) ((uint32_t)(user_data))

static struct io_uring_sqe *sqe_for(int op, uint32_t gen, uint32_t index)
{
    struct io_uring_sqe *sqe = uring_sqe(&self->ring);

    if (sqe == NULL) {
        perror("io_uring_enter");
        exit(1);
    }
    sqe->user_data = USER_DATA(op, gen, index);
    return sqe;
}

static inline uint32_t index_of(const conn *c)
{
    return (uint32_t)(c - self->conns);
}

static void arm_accept(int listener)
{
    struct io_uring_sqe *sqe = sqe_for(OP_ACCEPT, 0, (uint32_t)listener);

    sqe->opcode = IORING_OP_ACCEPT;
    sqe->fd = self->listen_fds[listener];
    sqe->ioprio = IORING_ACCEPT_MULTISHOT;
    sqe->file_index = IORING_FILE_INDEX_ALLOC;
    self->accepting[listener] = true;
}

static void arm_accept_all(void)
{
    for (int i = 0; i < self->listener_count; i++)
        if (!self->accepting[i])
            arm_accept(i);
}

static void arm_recv(conn *c)
{
    struct io_uring_sqe *sqe = sqe_for(OP_RECV, 0, index_of(c));

    sqe->opcode = IORING_OP_RECV;
    sqe->fd = (int)index_of(c);
    sqe->flags = IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
    sqe->ioprio = IORING_RECV_MULTISHOT;
    sqe->buf_group = 0;
    c->receiving = true;
}

static void submit_send(conn *c)
{
    struct io_uring_sqe *sqe = sqe_for(OP_SEND, 0, index_of(c));

    sqe->opcode = IORING_OP_SEND;
    sqe->fd = (int)index_of(c);
    sqe->flags = IOSQE_FIXED_FILE;
    sqe->addr = (uint64_t)(uintptr_t)c->out;
    sqe->len = c->out_len;
    sqe->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
}

static void cancel(uint64_t user_data)
{
    struct io_uring_sqe *sqe = sqe_for(OP_CANCEL, 0, 0);

    sqe->opcode = IORING_OP_ASYNC_CANCEL;
    sqe->fd = -1;
    sqe->addr = user_data;
}

/* --- the database's descriptors ----------------------------------------------------------- */

static void arm_db_read(dbwatch *w)
{
    struct io_uring_sqe *sqe = sqe_for(OP_DB_READ, w->gen, (uint32_t)(w - self->dbw));

    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = w->fd;
    sqe->poll32_events = POLLIN;
    /* One shot, armed again after each completion: a fresh poll reports what is readable now,
     * so bytes libpq left in the socket are met on the next pass. A multishot poll would say
     * nothing until the server sent more. */
}

static dbwatch *watch_of(db_conn *c)
{
    int i = db_conn_tag(c);

    if (i < 0) {
        i = self->dbw_count++;
        db_conn_set_tag(c, i);
        self->dbw[i].conn = c;
        self->dbw[i].fd = -1;
    }
    return &self->dbw[i];
}

static void db_watch_read(void *arg, db_conn *c, int fd)
{
    dbwatch *w = watch_of(c);

    (void)arg;
    w->fd = fd;
    w->gen++;
    arm_db_read(w);
}

static void db_watch_write(void *arg, db_conn *c, int fd)
{
    dbwatch *w = watch_of(c);
    struct io_uring_sqe *sqe = sqe_for(OP_DB_WRITE, w->gen, (uint32_t)(w - self->dbw));

    (void)arg;
    sqe->opcode = IORING_OP_POLL_ADD;
    sqe->fd = fd;
    sqe->poll32_events = POLLOUT;
}

static void db_unwatch(void *arg, db_conn *c, int fd)
{
    dbwatch *w = watch_of(c);

    (void)arg;
    (void)fd;
    cancel(USER_DATA(OP_DB_READ, w->gen, (uint32_t)(w - self->dbw)));
    /* Whatever still completes for the old descriptor is not for the new one. */
    w->gen++;
    w->fd = -1;
}

static const db_host db_host_ops = {db_watch_read, db_watch_write, db_unwatch};

static void on_db_read(const struct io_uring_cqe *cqe)
{
    dbwatch *w = &self->dbw[INDEX_OF(cqe->user_data)];

    if (GEN_OF(cqe->user_data) != (w->gen & 0xFFFFFF))
        return;
    if (cqe->res != -ECANCELED)
        db_on_readable(w->conn, cqe->res < 0 || (cqe->res & (POLLHUP | POLLRDHUP | POLLERR)) != 0);
    /* Armed again, unless the descriptor was replaced meanwhile. */
    if (GEN_OF(cqe->user_data) == (w->gen & 0xFFFFFF) && w->fd >= 0)
        arm_db_read(w);
}

static void on_db_write(const struct io_uring_cqe *cqe)
{
    dbwatch *w = &self->dbw[INDEX_OF(cqe->user_data)];

    if (GEN_OF(cqe->user_data) == (w->gen & 0xFFFFFF) && cqe->res != -ECANCELED)
        db_on_writable(w->conn);
}

/* --- connections -------------------------------------------------------------------------- */

static inline bool busy(const conn *c)
{
    return c->sending || c->parked;
}

/* The descriptor is closed when nothing of the connection is in the ring or at the database
 * any more -- so no completion can arrive for an index that already belongs to someone else. */
static void close_if_idle(conn *c)
{
    struct io_uring_sqe *sqe;

    if (!c->closing || c->receiving || c->sending || c->parked || c->close_sent)
        return;
    sqe = sqe_for(OP_CLOSE, 0, index_of(c));
    sqe->opcode = IORING_OP_CLOSE;
    sqe->file_index = index_of(c) + 1;
    c->close_sent = true;
}

/* No more requests from this connection. The receive is called back; a response on its way out
 * is called back as well when @p drop says the client is not taking it. */
static void close_conn(conn *c, bool drop)
{
    if (!c->closing) {
        c->closing = true;
        if (c->receiving)
            cancel(USER_DATA(OP_RECV, 0, index_of(c)));
    }
    if (drop && c->sending)
        cancel(USER_DATA(OP_SEND, 0, index_of(c)));
    close_if_idle(c);
}

static void on_closed(conn *c)
{
    free(c->in);
    free(c->out_buf);
    memset(c, 0, sizeof *c);
    /* A full table stopped the accepts; there is room again. */
    arm_accept_all();
}

/* --- replies ------------------------------------------------------------------------------ */

static const char *reason_of(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

static uint32_t write_head(char *dst, int status, uint32_t len, bool gzipped)
{
    return (uint32_t)snprintf(dst, HEAD_MAX,
                              "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n%s\r\n",
                              status, reason_of(status), len, gzipped ? "Content-Encoding: gzip\r\n" : "");
}

/* @p text must stay where it is until it has been sent: a literal, or @p buf, which is freed
 * then. */
static void send_start(conn *c, const char *text, uint32_t len, char *buf)
{
    c->out = text;
    c->out_len = len;
    c->out_buf = buf;
    c->sending = true;
    c->last_active = self->now;
    submit_send(c);
}

/* A response that is the same every time: put together when it is first needed, sent from
 * where it lies ever after. */
typedef struct {
    uint32_t len;
    char text[HEAD_MAX + 64];
} literal;

static void reply_literal(conn *c, int status, literal *l, const char *json, uint32_t len)
{
    if (l->len == 0) {
        char text[sizeof l->text];
        const uint32_t head = write_head(text, status, len, false);

        memcpy(text + head, json, len);
        /* Every loop may get here at once; each writes the same bytes. */
        memcpy(l->text, text, head + len);
        __atomic_store_n(&l->len, head + len, __ATOMIC_RELEASE);
    }
    send_start(c, l->text, l->len, NULL);
}

#define REPLY_ERROR(c, status, message)                                                            \
    do {                                                                                           \
        static literal l;                                                                          \
        reply_literal(c, status, &l, "{\"error\":\"" message "\"}",                                \
                      sizeof("{\"error\":\"" message "\"}") - 1);                                  \
    } while (0)

/* Sends the loop's rendered document: gzip'd if the client takes it and it is worth it. The one
 * copy of a response: head and body leave in one piece, from a buffer that is freed when they
 * have. */
static void reply_json(conn *c, int status, size_t len, bool gzip)
{
    const char *body = self->json;
    bool gzipped = false;
    char *buf;
    uint32_t head;

    if (len >= GZIP_MIN && gzip) {
        const size_t n = gz_compress(self->gz, self->json, len, self->gzbuf, len + 1024, self->now_ms);

        if (n != 0) {
            body = self->gzbuf;
            len = n;
            gzipped = true;
        }
    }
    if ((buf = malloc(HEAD_MAX + len)) == NULL) {
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    head = write_head(buf, status, (uint32_t)len, gzipped);
    memcpy(buf + head, body, len);
    send_start(c, buf, head + (uint32_t)len, buf);
}

/* --- rendering ---------------------------------------------------------------------------- */

typedef struct {
    char *p, *end;
} out;

static void put(out *o, const char *s, size_t n)
{
    if ((size_t)(o->end - o->p) >= n) {
        memcpy(o->p, s, n);
        o->p += n;
    } else {
        o->p = o->end + 1; /* overflow: the document is not sent */
    }
}

#define PUT(o, lit) put(o, lit, sizeof(lit) - 1)

static void put_int(out *o, int64_t v)
{
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);

    put(o, buf, (size_t)n);
}

/* A JSON string: quotes, backslashes and control characters escaped, everything else as it
 * is. The spec leaves <>&/ and non-ASCII alone, and the clients send plain ASCII. */
static void put_string(out *o, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t run = 0;

    PUT(o, "\"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];

        if (c >= 0x20 && c != '"' && c != '\\') {
            run++;
            continue;
        }
        put(o, s + i - run, run);
        run = 0;
        switch (c) {
        case '"': PUT(o, "\\\""); break;
        case '\\': PUT(o, "\\\\"); break;
        case '\n': PUT(o, "\\n"); break;
        case '\r': PUT(o, "\\r"); break;
        case '\t': PUT(o, "\\t"); break;
        case '\b': PUT(o, "\\b"); break;
        case '\f': PUT(o, "\\f"); break;
        default: {
            const char u[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            put(o, u, 6);
        }
        }
    }
    put(o, s + n - run, run);
    PUT(o, "\"");
}

/* A TIMESTAMPTZ as the API shows it: UTC, milliseconds truncated, Z. @p micros is what the
 * binary protocol carries, microseconds since 2000-01-01. */
static void put_timestamp(out *o, int64_t micros)
{
    /* Howard Hinnant's civil_from_days, with the epoch moved from 1970 to 2000. */
    int64_t secs = micros / 1000000, frac = micros % 1000000;
    if (frac < 0) {
        frac += 1000000;
        secs--;
    }
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    const int64_t z = days + 10957 + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "\"%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ\"",
                     (long long)(y + (m <= 2)), m, d, (unsigned)(rem / 3600), (unsigned)(rem / 60 % 60),
                     (unsigned)(rem % 60), (unsigned)(frac / 1000));

    put(o, buf, (size_t)n);
}

/* One post object from row @p i of a FEED or POST result. */
static void put_post(out *o, PGresult *r, int i)
{
    PUT(o, "{\"id\":");
    put_int(o, db_int64(PQgetvalue(r, i, COL_ID)));
    PUT(o, ",\"body\":");
    put_string(o, PQgetvalue(r, i, COL_BODY), (size_t)PQgetlength(r, i, COL_BODY));
    PUT(o, ",\"created_at\":");
    put_timestamp(o, db_int64(PQgetvalue(r, i, COL_CREATED)));
    PUT(o, ",\"author\":");
    put_string(o, PQgetvalue(r, i, COL_AUTHOR), (size_t)PQgetlength(r, i, COL_AUTHOR));
    PUT(o, ",\"like_count\":");
    put_int(o, db_int32(PQgetvalue(r, i, COL_LIKES)));
    PUT(o, "}");
}

static out begin(void)
{
    out o = {self->json, self->json + JSON_CAP};

    return o;
}

/* Sends what was rendered since begin(), or a 500 if it did not fit. */
static void finish(conn *c, int status, out *o, bool gzip)
{
    if (o->p > o->end) {
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    reply_json(c, status, (size_t)(o->p - self->json), gzip);
}

/* --- parked requests ---------------------------------------------------------------------- */

static slot *park(conn *c, bool gzip)
{
    slot *s = self->free_slots;

    if (s == NULL)
        return NULL;
    self->free_slots = s->next_free;
    s->conn = c;
    s->gzip = gzip;
    memset(&s->job, 0, sizeof s->job);
    c->parked = true;
    return s;
}

static void unpark(slot *s)
{
    s->conn = NULL;
    s->next_free = self->free_slots;
    self->free_slots = s;
}

/* Parks the request and sends its job; answers 503 itself when it cannot. */
static void submit(conn *c, slot *s)
{
    if (db_submit(self->db, &s->job) != 0) {
        c->parked = false;
        unpark(s);
        REPLY_ERROR(c, 503, "internal server error");
    }
}

static void answer(slot *s)
{
    conn *c = s->conn;
    const db_job *job = &s->job;
    const bool gzip = s->gzip;
    out o;

    switch (job->kind) {
    case JOB_FEED:
        if (job->status != 200)
            break;
        o = begin();
        PUT(&o, "{\"posts\":[");
        for (int i = 0, n = PQntuples(job->result); i < n; i++) {
            if (i != 0)
                PUT(&o, ",");
            put_post(&o, job->result, i);
        }
        PUT(&o, "]}");
        finish(c, 200, &o, gzip);
        return;
    case JOB_POST:
        if (job->status == 404) {
            REPLY_ERROR(c, 404, "post not found");
            return;
        }
        if (job->status != 200)
            break;
        o = begin();
        PUT(&o, "{\"post\":");
        put_post(&o, job->result, 0);
        PUT(&o, "}");
        finish(c, 200, &o, gzip);
        return;
    case JOB_CREATE:
        if (job->status != 201)
            break;
        o = begin();
        PUT(&o, "{\"post\":{\"id\":");
        put_int(&o, job->post_id);
        PUT(&o, ",\"body\":");
        put_string(&o, s->body, job->body_len);
        PUT(&o, ",\"created_at\":");
        put_timestamp(&o, job->created_micros);
        PUT(&o, ",\"author\":");
        put_string(&o, s->author, s->author_len);
        PUT(&o, ",\"like_count\":0}}");
        finish(c, 201, &o, gzip);
        return;
    case JOB_LIKE:
        if (job->status == 404) {
            REPLY_ERROR(c, 404, "post not found");
            return;
        }
        if (job->status != 200 && job->status != 201)
            break;
        o = begin();
        PUT(&o, "{\"liked\":true,\"already_liked\":");
        if (job->status == 200)
            PUT(&o, "true");
        else
            PUT(&o, "false");
        PUT(&o, ",\"post_id\":");
        put_int(&o, job->post_id);
        PUT(&o, "}");
        finish(c, job->status, &o, gzip);
        return;
    case JOB_PING:
        o = begin();
        if (job->status == 200) {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            PUT(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            put_int(&o, now.tv_sec - started.tv_sec);
            PUT(&o, "}");
            finish(c, 200, &o, gzip);
        } else {
            PUT(&o, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
            put_string(&o, job->error, strlen(job->error));
            PUT(&o, "}");
            finish(c, 503, &o, gzip);
        }
        return;
    default:
        break;
    }
    REPLY_ERROR(c, 500, "internal server error");
}

/* On the loop thread, once the job's statement has returned. */
static void on_job_done(db_job *job)
{
    slot *s = (slot *)((char *)job - offsetof(slot, job));
    conn *c = s->conn;

    c->parked = false;
    /* The client may have gone meanwhile; a write committed all the same. */
    if (!c->closing)
        answer(s);
    if (s->job.result != NULL) {
        PQclear(s->job.result);
        s->job.result = NULL;
    }
    unpark(s);
    close_if_idle(c);
}

/* --- handlers ----------------------------------------------------------------------------- */

/* Bytes of whitespace at @p p, as String.prototype.trim() counts it; 0 if there is none. */
static size_t space_at(const unsigned char *p, const unsigned char *end)
{
    if (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        return 1;
    if (end - p >= 2 && p[0] == 0xC2 && p[1] == 0xA0)
        return 2;
    if (end - p >= 3 && (p[0] & 0xF0) == 0xE0) {
        const uint32_t u = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);

        if (u == 0x1680 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 ||
            u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF)
            return 3;
    }
    return 0;
}

static void trim(const unsigned char **start, const unsigned char **end)
{
    const unsigned char *p = *start, *e = *end;
    size_t n;

    while (p < e && (n = space_at(p, e)) != 0)
        p += n;
    for (;;) {
        /* The last character starts one to three bytes before the end. */
        if (e - p >= 1 && space_at(e - 1, e) == 1) e -= 1;
        else if (e - p >= 2 && space_at(e - 2, e) == 2) e -= 2;
        else if (e - p >= 3 && space_at(e - 3, e) == 3) e -= 3;
        else break;
    }
    *start = p;
    *end = e;
}

/* One request, as the parser found it in the connection's input. Nothing here outlives the
 * handler: what a parked request needs, the handler that parks it copies. */
typedef struct {
    const char *path, *auth, *body;
    size_t path_len, auth_len, body_len;
    bool get, post, gzip;
} request;

static int authenticate(conn *c, const request *req, int64_t *uid, char *name, size_t *name_len)
{
    switch (jwt_verify(req->auth, req->auth_len, uid, name, name_len)) {
    case JWT_OK: return 0;
    case JWT_MISSING: REPLY_ERROR(c, 401, "missing bearer token"); break;
    case JWT_INVALID: REPLY_ERROR(c, 401, "invalid or expired token"); break;
    default: REPLY_ERROR(c, 401, "invalid token payload"); break;
    }
    return -1;
}

static void read_job(conn *c, const request *req, int kind, int64_t id)
{
    slot *s = park(c, req->gzip);

    if (s == NULL) {
        REPLY_ERROR(c, 503, "internal server error");
        return;
    }
    s->job.kind = kind;
    s->job.post_id = id;
    submit(c, s);
}

static void post_like(conn *c, const request *req, int64_t uid, int64_t id)
{
    slot *s = park(c, req->gzip);

    if (s == NULL) {
        REPLY_ERROR(c, 503, "internal server error");
        return;
    }
    s->job.kind = JOB_LIKE;
    s->job.user_id = uid;
    s->job.post_id = id;
    submit(c, s);
}

/*
 * The request body is parsed in the loop's arena (the parser copies its input there), the
 * trimmed `body` is copied into the slot, and nothing of the request is needed after that.
 */
static void post_create(conn *c, const request *req, int64_t uid, const char *name, size_t name_len)
{
    arnm arena = {0};
    arnm_json_reader reader;
    arnm_json_value *root = NULL;
    arnm_memory_block body = {0};
    arnm_json_field fields[1] = {ARNM_JSON_FIELD_STRING("body", &body)};
    uint64_t found = 0;
    const unsigned char *p, *e;
    arnm_result parsed;
    size_t chars = 0;
    slot *s;

    if (req->body_len == 0) {
        REPLY_ERROR(c, 400, "malformed JSON body");
        return;
    }
    if (arnm_init_arena_borrow(&arena, self->parse_buf, PARSE_ARENA) != ARNM_SUCCESS ||
        arnm_json_reader_init(&reader, &arena) != ARNM_SUCCESS) {
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    parsed = arnm_json_reader_parse(&reader, req->body, (uint32_t)req->body_len, false, &root);
    if (parsed == ARNM_ERROR_OUT_OF_MEMORY) {
        REPLY_ERROR(c, 500, "internal server error");
        return;
    }
    if (parsed != ARNM_SUCCESS) {
        REPLY_ERROR(c, 400, "malformed JSON body");
        return;
    }
    /* One field, so the table matches at the first key of the body the clients send. */
    arnm_json_read_object(root, fields, 1, &found);
    p = body.data;
    e = body.data + body.size;
    if (found == 1)
        trim(&p, &e);
    for (const unsigned char *q = p; found == 1 && q < e; q++)
        chars += (*q & 0xC0) != 0x80;

    if (found != 1 || p == e) {
        REPLY_ERROR(c, 400, "body is required");
    } else if (chars > 500 || (size_t)(e - p) > DB_BODY_MAX) {
        REPLY_ERROR(c, 400, "body must be at most 500 characters");
    } else if ((s = park(c, req->gzip)) == NULL) {
        REPLY_ERROR(c, 503, "internal server error");
    } else {
        s->job.kind = JOB_CREATE;
        s->job.user_id = uid;
        s->job.body = s->body;
        s->job.body_len = (uint32_t)(e - p);
        memcpy(s->body, p, (size_t)(e - p));
        memcpy(s->author, name, name_len);
        s->author_len = name_len;
        submit(c, s);
    }
}

#define POSTS_SLASH_LENGTH 7

static void route(conn *c, const request *req)
{
    const char *path = req->path;
    const size_t len = req->path_len;

    if (req->get) {
        if (len == 5 && memcmp(path, "/feed", 5) == 0) {
            read_job(c, req, JOB_FEED, 0);
            return;
        } else if (len >= POSTS_SLASH_LENGTH && memcmp(path, "/posts/", 7) == 0) {
            const int64_t id = parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH);

            if (id > 0) {
                read_job(c, req, JOB_POST, id);
            } else {
                const char *slash = memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH);

                if (slash != NULL) REPLY_ERROR(c, 404, "not found");
                else if (id == -1) REPLY_ERROR(c, 404, "post not found");
                else REPLY_ERROR(c, 400, "invalid post id");
            }
            return;
        } else if (len == 7 && memcmp(path, "/health", 7) == 0) {
            read_job(c, req, JOB_PING, 0);
            return;
        }
    } else if (req->post && len >= 6 && memcmp(path, "/posts", 6) == 0) {
        char name[JWT_NAME_MAX];
        size_t name_len;
        int64_t uid;

        if (len == 6) {
            if (authenticate(c, req, &uid, name, &name_len) == 0)
                post_create(c, req, uid, name, name_len);
            return;
        } else if (len > POSTS_SLASH_LENGTH && path[6] == '/' && memcmp(path + len - 5, "/like", 5) == 0) {
            const int64_t id = len > POSTS_SLASH_LENGTH + 5
                                   ? parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH - 5)
                                   : 0;

            if (id <= 0) {
                const char *slash = memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH);

                if (slash != path + len - 5) {
                    REPLY_ERROR(c, 404, "not found");
                    return;
                }
            }
            /* Auth first, then the id: the spec says so. */
            if (authenticate(c, req, &uid, name, &name_len) == 0) {
                if (id > 0) post_like(c, req, uid, id);
                else if (id == 0) REPLY_ERROR(c, 400, "invalid post id");
                else REPLY_ERROR(c, 404, "post not found");
            }
            return;
        }
    }
    REPLY_ERROR(c, 404, "not found");
}

/* --- requests ----------------------------------------------------------------------------- */

static inline bool header_is(const struct phr_header *h, const char *name, size_t len)
{
    return h->name_len == len && strncasecmp(h->name, name, len) == 0;
}

static inline bool value_is(const struct phr_header *h, const char *value, size_t len)
{
    return h->value_len == len && strncasecmp(h->value, value, len) == 0;
}

#define HEADER_IS(h, name) header_is(h, name, sizeof(name) - 1)
#define VALUE_IS(h, value) value_is(h, value, sizeof(value) - 1)

/* Whether "gzip" is among the codings of an Accept-Encoding value: a comma-separated list of
 * tokens, each possibly followed by ";q=...". */
static bool accepts_gzip(const char *v, size_t n)
{
    const char *end = v + n;

    while (v < end) {
        const char *tok, *tok_end;

        while (v < end && (*v == ' ' || *v == '\t' || *v == ','))
            v++;
        tok = v;
        while (v < end && *v != ',' && *v != ';' && *v != ' ' && *v != '\t')
            v++;
        tok_end = v;
        while (v < end && *v != ',')
            v++;
        if (tok_end - tok == 4 && strncasecmp(tok, "gzip", 4) == 0)
            return true;
    }
    return false;
}

/*
 * Serves the request at the start of @p buf if all of it is there.
 *
 * > 0  its length: it was answered, or parked at the database
 *   0  it has not arrived completely yet
 *  -1  it cannot be served; the answer says so and the connection closes behind it
 */
static int serve_one(conn *c, const char *buf, uint32_t len)
{
    static const char go_on[] = "HTTP/1.1 100 Continue\r\n\r\n";
    struct phr_header headers[MAX_HEADERS];
    size_t header_count = MAX_HEADERS, method_len;
    const char *method, *query;
    int minor, head;
    int64_t content_length = 0;
    bool keep_alive, close = false, keep = false, expects = false, chunked = false;
    request req = {0};

    head = phr_parse_request(buf, len, &method, &method_len, &req.path, &req.path_len, &minor, headers,
                             &header_count, 0);
    if (head == -2 && len < MAX_HEAD)
        return 0;
    if (head < 0) {
        c->close_after = true;
        REPLY_ERROR(c, 400, "bad request");
        return -1;
    }
    for (size_t i = 0; i < header_count; i++) {
        const struct phr_header *h = &headers[i];

        if (HEADER_IS(h, "content-length")) {
            content_length = parse_id(h->value, h->value_len);
            /* parse_id reads 0 for what is no number, and for "0". */
            if (content_length < 0 || (content_length == 0 && !VALUE_IS(h, "0")))
                content_length = -1;
        } else if (HEADER_IS(h, "authorization")) {
            req.auth = h->value;
            req.auth_len = h->value_len;
        } else if (HEADER_IS(h, "accept-encoding")) {
            req.gzip = accepts_gzip(h->value, h->value_len);
        } else if (HEADER_IS(h, "connection")) {
            close = VALUE_IS(h, "close");
            keep = VALUE_IS(h, "keep-alive");
        } else if (HEADER_IS(h, "expect")) {
            expects = VALUE_IS(h, "100-continue");
        } else if (HEADER_IS(h, "transfer-encoding")) {
            chunked = true;
        }
    }
    if (chunked || content_length < 0 || content_length > MAX_BODY) {
        c->close_after = true;
        if (content_length > MAX_BODY)
            REPLY_ERROR(c, 413, "body too large");
        else
            REPLY_ERROR(c, 400, "bad request");
        return -1;
    }
    if (len - (uint32_t)head < (uint32_t)content_length) {
        if (expects && !c->continued) {
            c->continued = true;
            send_start(c, go_on, sizeof go_on - 1, NULL);
        }
        return 0;
    }
    c->continued = false;
    keep_alive = minor >= 1 ? !close : keep;
    c->close_after = !keep_alive;

    req.get = method_len == 3 && memcmp(method, "GET", 3) == 0;
    req.post = method_len == 4 && memcmp(method, "POST", 4) == 0;
    if ((query = memchr(req.path, '?', req.path_len)) != NULL)
        req.path_len = (size_t)(query - req.path);
    req.body = buf + head;
    req.body_len = (size_t)content_length;
    route(c, &req);
    return head + (int)content_length;
}

/* Serves requests from @p buf one after the other for as long as the connection is free to
 * answer: a response is sent, or rows are in, before the next request is looked at. Returns
 * the bytes that were served. */
static uint32_t serve(conn *c, const char *buf, uint32_t len)
{
    uint32_t done = 0;

    while (done < len && !busy(c) && !c->closing && !c->close_after) {
        const int used = serve_one(c, buf + done, len - done);

        if (used <= 0)
            break;
        done += (uint32_t)used;
    }
    return done;
}

/* Keeps input for later. False when the connection has more waiting than it may: it is closed. */
static bool keep_input(conn *c, const char *data, uint32_t len)
{
    if (c->in_len + len > c->in_cap) {
        uint32_t cap = c->in_cap != 0 ? c->in_cap : 1024;
        char *grown;

        while (cap < c->in_len + len)
            cap *= 2;
        if (cap > MAX_PENDING || (grown = realloc(c->in, cap)) == NULL) {
            close_conn(c, true);
            return false;
        }
        c->in = grown;
        c->in_cap = cap;
    }
    memcpy(c->in + c->in_len, data, len);
    c->in_len += len;
    return true;
}

/* Serves what was kept, now that the connection is free again. */
static void drain(conn *c)
{
    const uint32_t done = serve(c, c->in, c->in_len);

    if (c->closing)
        return;
    c->in_len -= done;
    if (c->in_len != 0) {
        memmove(c->in, c->in + done, c->in_len);
    } else {
        free(c->in);
        c->in = NULL;
        c->in_cap = 0;
    }
}

/* Nearly always a whole request on a connection that is waiting for one, and then it is served
 * from the buffer it arrived in and nothing is copied. */
static void on_input(conn *c, const char *data, uint32_t len)
{
    c->last_active = self->now;
    if (c->in_len == 0 && !busy(c)) {
        const uint32_t done = serve(c, data, len);

        if (done < len && !c->closing && !c->close_after)
            (void)keep_input(c, data + done, len - done);
    } else if (keep_input(c, data, len) && !busy(c)) {
        drain(c);
    }
}

/* --- completions -------------------------------------------------------------------------- */

static void on_accept(const struct io_uring_cqe *cqe)
{
    const int listener = (int)INDEX_OF(cqe->user_data);

    if (!(cqe->flags & IORING_CQE_F_MORE))
        self->accepting[listener] = false;
    if (cqe->res >= 0 && (uint32_t)cqe->res < self->max_conns) {
        conn *c = &self->conns[cqe->res];

        c->open = true;
        c->last_active = self->now;
        arm_recv(c);
    }
    /* Out of room: every connection the table holds is open. The accept is armed again when one
     * of them has closed, or at the next sweep; new users wait in the backlog until then. */
    if (!self->accepting[listener] && cqe->res != -ENFILE && cqe->res != -EMFILE && cqe->res != -ENOBUFS &&
        cqe->res != -ENOMEM)
        arm_accept(listener);
}

static void on_recv(conn *c, const struct io_uring_cqe *cqe)
{
    if (!(cqe->flags & IORING_CQE_F_MORE))
        c->receiving = false;
    if (cqe->flags & IORING_CQE_F_BUFFER) {
        const unsigned id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        const char *data = (const char *)self->ring.buf_base + (size_t)id * self->ring.buf_size;

        if (cqe->res > 0 && !c->closing)
            on_input(c, data, (uint32_t)cqe->res);
        /* Whatever of it is still needed has been copied. */
        uring_buffer_return(&self->ring, id);
    }
    if (cqe->res <= 0 && cqe->res != -ENOBUFS) {
        /* The client closed, or the connection broke, or the receive was called back. */
        c->closing = true;
        close_if_idle(c);
    } else if (!c->receiving && !c->closing) {
        arm_recv(c);
    }
}

static void on_sent(conn *c, const struct io_uring_cqe *cqe)
{
    if (cqe->res > 0 && (uint32_t)cqe->res < c->out_len && !c->closing) {
        /* A part of it went out; the rest follows. */
        c->out += cqe->res;
        c->out_len -= (uint32_t)cqe->res;
        submit_send(c);
        return;
    }
    c->sending = false;
    free(c->out_buf);
    c->out_buf = NULL;
    if (cqe->res <= 0 || c->close_after)
        close_conn(c, false);
    else if (c->in_len != 0 && !c->closing)
        drain(c);
    close_if_idle(c);
}

static unsigned reap(void)
{
    uring *r = &self->ring;
    unsigned head = *r->cq_head, count = 0;

    for (;;) {
        const unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);

        if (head == tail)
            break;
        for (; head != tail; head++, count++) {
            const struct io_uring_cqe cqe = r->cqes[head & r->cq_mask];

            __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
            switch (OP_OF(cqe.user_data)) {
            case OP_RECV: on_recv(&self->conns[INDEX_OF(cqe.user_data)], &cqe); break;
            case OP_SEND: on_sent(&self->conns[INDEX_OF(cqe.user_data)], &cqe); break;
            case OP_ACCEPT: on_accept(&cqe); break;
            case OP_CLOSE: on_closed(&self->conns[INDEX_OF(cqe.user_data)]); break;
            case OP_DB_READ: on_db_read(&cqe); break;
            case OP_DB_WRITE: on_db_write(&cqe); break;
            default: break;
            }
        }
    }
    return count;
}

/* Closes what has been idle for too long, and what has stopped half way. */
static void sweep(void)
{
    for (uint32_t i = 0; i < self->max_conns; i++) {
        conn *c = &self->conns[i];
        uint32_t idle;

        if (!c->open || c->closing || c->parked)
            continue;
        idle = self->now - c->last_active;
        if (idle >= IDLE_TIMEOUT_S || ((c->in_len != 0 || c->sending) && idle >= STALL_TIMEOUT_S))
            close_conn(c, true);
    }
    arm_accept_all();
}

static void read_clock(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    self->now = (uint32_t)ts.tv_sec;
    self->now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* --- the loops ---------------------------------------------------------------------------- */

static int listen_on(const struct sockaddr_in *addr)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    /* TCP_NODELAY is inherited by the sockets accepted here. */
    if (fd == -1 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) != 0 ||
        bind(fd, (const struct sockaddr *)addr, sizeof *addr) != 0 || listen(fd, SOMAXCONN) != 0) {
        fprintf(stderr, "cannot listen on port %u: %s\n", (unsigned)ntohs(addr->sin_port), strerror(errno));
        return -1;
    }
    return fd;
}

static void *loop_main(void *arg)
{
    loop *l = arg;
    bool loaded = false;

    self = l;
    /* The ring belongs to the thread that made it (IORING_SETUP_SINGLE_ISSUER): made here. */
    if (uring_init(&l->ring, SQ_ENTRIES, CQ_ENTRIES) != 0) {
        fprintf(stderr, "io_uring_setup (%u entries, cq %u; Linux 6.1 or later is needed): %s\n", SQ_ENTRIES,
                CQ_ENTRIES, strerror(errno));
        exit(1);
    }
    if (uring_register_table(&l->ring, l->max_conns) != 0) {
        fprintf(stderr, "io_uring_register(FILES2, %u sparse slots): %s\n", l->max_conns, strerror(errno));
        exit(1);
    }
    if (uring_register_buffers(&l->ring, RECV_BUFFERS, RECV_BUFFER_SIZE, 0) != 0) {
        fprintf(stderr, "io_uring_register(PBUF_RING, %u buffers of %u bytes): %s\n", RECV_BUFFERS, RECV_BUFFER_SIZE,
                strerror(errno));
        exit(1);
    }
    if ((l->conns = calloc(l->max_conns, sizeof *l->conns)) == NULL || (l->slots = calloc(SLOTS, sizeof *l->slots)) == NULL ||
        (l->dbw = calloc((size_t)l->pg_count, sizeof *l->dbw)) == NULL || (l->gz = gz_create()) == NULL ||
        (l->json = malloc(JSON_CAP)) == NULL || (l->gzbuf = malloc(JSON_CAP + 1024)) == NULL ||
        (l->parse_buf = aligned_alloc(16, PARSE_ARENA)) == NULL)
        abort();
    for (int k = 0; k < SLOTS; k++) {
        l->slots[k].next_free = l->free_slots;
        l->free_slots = &l->slots[k];
    }
    read_clock();
    l->swept = l->now;
    l->ticked_ms = l->now_ms;
    l->db = db_pool_create(l->pg, l->pg_count, on_job_done, &db_host_ops, l);
    arm_accept_all();

    for (;;) {
        /* The statements of the pass go out, batched per connection. */
        db_pool_flush(l->db);
        /* Idle, the loop sleeps until something arrives and serves it at once. Once there was
         * work, it lets completions collect: a pass then meets every request of those
         * milliseconds, and one call into the kernel carries all of their answers. */
        if (uring_enter(&l->ring, loaded ? WAIT_COMPLETIONS : 1, loaded ? WAIT_NS : IDLE_WAIT_NS) != 0) {
            perror("io_uring_enter");
            exit(1);
        }
        read_clock();
        loaded = reap() != 0;
        if (l->now - l->swept >= SWEEP_S) {
            l->swept = l->now;
            sweep();
        }
        if (l->now_ms - l->ticked_ms >= 1000) {
            l->ticked_ms = l->now_ms;
            db_pool_tick(l->db);
        }
    }
}

/* The CPU quota of this process's cgroup (cgroup v2: `docker --cpus`, systemd's CPUQuota),
 * rounded up to whole cores; 0 when there is none. */
static int cgroup_cpu_limit(void)
{
    char line[512], path[600];
    const char *group = "";
    FILE *f;
    long quota, period;
    int limit = 0;

    if ((f = fopen("/proc/self/cgroup", "r")) != NULL) {
        while (fgets(line, sizeof line, f) != NULL)
            if (strncmp(line, "0::", 3) == 0) {
                line[strcspn(line, "\n")] = '\0';
                snprintf(path, sizeof path, "/sys/fs/cgroup%s/cpu.max", line + 3);
                group = path;
            }
        fclose(f);
    }
    if (group[0] == '\0')
        group = "/sys/fs/cgroup/cpu.max";
    if ((f = fopen(group, "r")) == NULL)
        return 0;
    if (fscanf(f, "%ld %ld", &quota, &period) == 2 && quota > 0 && period > 0)
        limit = (int)((quota + period - 1) / period);
    fclose(f);
    return limit;
}

/* The cores this process may run on: the affinity mask (a cpuset, taskset, or a VM's share),
 * and under a CPU quota the quota -- not the machine's count, which is what sysconf answers
 * inside a container. */
static int cores_available(void)
{
    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    const int limit = cgroup_cpu_limit();
    cpu_set_t set;

    if (sched_getaffinity(0, sizeof set, &set) == 0 && CPU_COUNT(&set) > 0)
        cores = CPU_COUNT(&set);
    if (limit > 0 && limit < cores)
        cores = limit;
    return cores;
}

static uint32_t table_size(int threads)
{
    struct rlimit limit;
    uint64_t want = MAX_CONNS_PER_LOOP;
    uint32_t size = 4096;

    /* The sockets in a loop's table do not count against the descriptor limit, but the limit
     * is what the service was given for connections: the loops share it, with room for the
     * kernel spreading them unevenly. */
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
        want = 2 * (uint64_t)limit.rlim_cur / (uint64_t)threads;
    if (want > MAX_CONNS_PER_LOOP)
        want = MAX_CONNS_PER_LOOP;
    while (size < want)
        size *= 2;
    return size;
}

int server_run(const char *host, uint16_t port, uint16_t extra_port, const char *database_url,
               int threads, int pool_size)
{
    struct sockaddr_in addrs[LISTENERS];
    int ports = 1;
    loop *loops;
    PGconn **conns;
    int connected, given = 0;

    memset(addrs, 0, sizeof addrs);
    addrs[0].sin_family = addrs[1].sin_family = AF_INET;
    addrs[0].sin_port = htons(port);
    addrs[1].sin_port = htons(extra_port);
    if (inet_pton(AF_INET, host, &addrs[0].sin_addr) != 1) {
        fprintf(stderr, "HOST must be an IPv4 address, got %s\n", host);
        return 1;
    }
    addrs[1].sin_addr = addrs[0].sin_addr;
    if (extra_port != 0 && extra_port != port)
        ports = 2;
    if (threads <= 0)
        threads = cores_available();
    if (threads < 1)
        threads = 1;
    clock_gettime(CLOCK_MONOTONIC, &started);

    /* The database first: a server that listens before it can answer is one that says it is
     * healthy when it is not. */
    if (pool_size < 1)
        pool_size = 1;
    if ((conns = calloc((size_t)pool_size, sizeof *conns)) == NULL)
        abort();
    connected = db_dial(database_url, pool_size, conns);
    if (connected == 0) {
        fprintf(stderr, "no database connection could be made\n");
        return 1;
    }
    /* A loop without a connection could answer nothing: fewer loops, then. */
    if (threads > connected)
        threads = connected;

    if ((loops = calloc((size_t)threads, sizeof *loops)) == NULL)
        abort();
    /* Every socket is bound here, before any loop serves: a taken port is one error at startup. */
    for (int i = 0; i < threads; i++) {
        loop *l = &loops[i];
        /* The connections, dealt out as evenly as they go. */
        const int share = connected / threads + (i < connected % threads);

        l->index = i;
        l->max_conns = table_size(threads);
        l->pg = conns + given;
        l->pg_count = share;
        given += share;
        for (int p = 0; p < ports; p++) {
            if ((l->listen_fds[p] = listen_on(&addrs[p])) == -1)
                return 1;
            l->listener_count++;
        }
    }
    {
        char extra[32] = "";

        if (ports == 2)
            snprintf(extra, sizeof extra, " and %u", (unsigned)extra_port);
        fprintf(stderr, "listening on %s:%u%s, %d loop%s of up to %u connections, %d of %d database connections, gzip: %s\n",
                host, (unsigned)port, extra, threads, threads == 1 ? "" : "s", loops[0].max_conns, connected,
                pool_size, gz_backend());
    }

    for (int i = 1; i < threads; i++) {
        if (pthread_create(&loops[i].thread, NULL, loop_main, &loops[i]) != 0) {
            fprintf(stderr, "cannot start loop %d\n", i);
            return 1;
        }
    }
    loop_main(&loops[0]);
    return 1;
}
