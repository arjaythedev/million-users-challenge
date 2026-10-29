/*
 * The HTTP side: h2o's evloop, one loop per core, each with its own listening sockets under
 * SO_REUSEPORT -- one per port the API is served on -- gradido-unicorn's thread-per-loop model.
 *
 * Every request that touches the database is parked on its loop and its statement sent on one
 * of the loop's own connections (db.c), which sit in the same epoll as the clients. The loop
 * goes on serving its other connections, and answers the parked request when the rows are in.
 * Nothing crosses a thread: a parked request is a slot on its loop, and a tracker in the
 * request's pool tells the slot when the client has gone.
 *
 * Responses are rendered on the loop into a buffer of its own, gzip'd there when the client
 * asked for it and the body is worth it, and copied once into the request's pool to be sent.
 */
#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "h2o.h"

#include "arnm/arena.h"
#include "arnm/json_reader.h"

#include "db.h"
#include "gz.h"
#include "jwt.h"
#include "parse_id.h"

/* Requests one loop may have parked at the database. Beyond it a request is answered 503.
 * Sized to ride out a database that stalls for a second or two (a checkpoint) at a loop's share
 * of 42,000 requests per second: 4096 slots of 2.3 KB are 9 MB per loop. */
#define SLOTS 4096
/* How long a keep-alive connection may sit idle. The spec asks for 75 s; a user of the load
 * profile is quiet for 37.5 s at most, and an idle connection costs nothing but its socket. */
#define IDLE_TIMEOUT_MS 150000
#define MAX_BODY (64 * 1024)
/* A rendered feed: 20 posts of at most 2000 bytes, every byte escaped at worst. */
#define JSON_CAP (20u * (6u * DB_BODY_MAX + 1200u) + 256u)
/* Bodies below this go out as they are: a post object of 300 bytes shrinks by a quarter and
 * costs a deflate; a feed shrinks to a quarter. */
#define GZIP_MIN 1024
/* The arena a request body is parsed in: the parser's copy of the input and its values. */
#define PARSE_ARENA (8u * MAX_BODY + 4096u)
#define LISTENERS 2

struct loop;

typedef struct slot {
    db_job job;
    struct loop *loop;
    /* NULL once the client has gone; the job still runs and the slot still comes back. */
    h2o_req_t *req;
    struct tracker *tracker;
    size_t author_len;
    char author[JWT_NAME_MAX];
    /* A create's trimmed body: the job's `body` points here. */
    char body[DB_BODY_MAX];
    struct slot *next_free;
} slot;

/* Lives in the request's pool, so h2o disposes it with the request -- which is how a parked
 * request that lost its client is noticed. */
typedef struct tracker {
    slot *slot;
} tracker;

/* A database connection's socket in the loop: h2o's, on a dup of libpq's descriptor (h2o closes
 * its own, libpq its own). */
typedef struct dbwatch {
    db_conn *conn;
    h2o_socket_t *sock;
    int fd; /* the descriptor the dup was taken of; -1 while none */
    struct loop *loop;
} dbwatch;

typedef struct loop {
    h2o_context_t ctx;
    h2o_accept_ctx_t accept_ctx;
    db_pool *db;
    dbwatch *dbw;
    int dbw_count;
    h2o_socket_t *listeners[LISTENERS];
    int listen_fds[LISTENERS];
    int listener_count;
    h2o_timer_t accept_timer;
    h2o_timer_t db_timer; /* once a second: the pool dials again what it lost */
    h2o_timer_t db_flush_timer; /* the batches of the last millisecond go out */
    slot *slots, *free_slots;
    gz_ctx *gz;
    char *json, *gzbuf;
    uint8_t *parse_buf;
    pthread_t thread;
} loop;

static h2o_globalconf_t config;
static struct timespec started;
static __thread loop *self;

/* --- replies ------------------------------------------------------------------------------ */

static const char *reason_of(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

/* @p body must stay valid until the response is on the wire: a literal, or request-pool memory. */
static void reply_borrowed(h2o_req_t *req, int status, const char *body, size_t len, int gzipped)
{
    static h2o_generator_t generator = {NULL, NULL};
    h2o_iovec_t buf = h2o_iovec_init(body, len);

    req->res.status = status;
    req->res.reason = reason_of(status);
    req->res.content_length = len;
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL,
                   H2O_STRLIT("application/json"));
    if (gzipped)
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_ENCODING, NULL,
                       H2O_STRLIT("gzip"));
    h2o_start_response(req, &generator);
    h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
}

#define REPLY_LIT(req, status, json) reply_borrowed(req, status, json, sizeof(json) - 1, 0)
#define REPLY_ERROR(req, status, message) REPLY_LIT(req, status, "{\"error\":\"" message "\"}")

static int accepts_gzip(h2o_req_t *req)
{
    const ssize_t i = h2o_find_header(&req->headers, H2O_TOKEN_ACCEPT_ENCODING, -1);

    if (i == -1)
        return 0;
    return h2o_contains_token(req->headers.entries[i].value.base, req->headers.entries[i].value.len,
                              H2O_STRLIT("gzip"), ',');
}

/* Sends the loop's rendered document: gzip'd if the client takes it and it is worth it. */
static void reply_json(h2o_req_t *req, int status, size_t len)
{
    const char *body = self->json;
    int gzipped = 0;

    if (len >= GZIP_MIN && accepts_gzip(req)) {
        const size_t n = gz_compress(self->gz, self->json, len, self->gzbuf, len + 1024, h2o_now(self->ctx.loop));

        if (n != 0) {
            body = self->gzbuf;
            len = n;
            gzipped = 1;
        }
    }
    reply_borrowed(req, status, h2o_strdup(&req->pool, body, len).base, len, gzipped);
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
static void finish(h2o_req_t *req, int status, out *o)
{
    if (o->p > o->end) {
        REPLY_ERROR(req, 500, "internal server error");
        return;
    }
    reply_json(req, status, (size_t)(o->p - self->json));
}

/* --- parked requests ---------------------------------------------------------------------- */

static void on_tracker_dispose(void *p)
{
    tracker *t = p;

    if (t->slot != NULL)
        t->slot->req = NULL;
}

static slot *park(h2o_req_t *req)
{
    slot *s = self->free_slots;

    if (s == NULL)
        return NULL;
    self->free_slots = s->next_free;
    s->req = req;
    s->tracker = h2o_mem_alloc_shared(&req->pool, sizeof *s->tracker, on_tracker_dispose);
    s->tracker->slot = s;
    memset(&s->job, 0, sizeof s->job);
    return s;
}

static void unpark(slot *s)
{
    if (s->tracker != NULL)
        s->tracker->slot = NULL;
    s->req = NULL;
    s->tracker = NULL;
    s->next_free = self->free_slots;
    self->free_slots = s;
}

/* Parks the request and queues its job; answers 503 itself when it cannot. */
static void submit(h2o_req_t *req, slot *s)
{
    if (db_submit(self->db, &s->job) != 0) {
        unpark(s);
        REPLY_ERROR(req, 503, "internal server error");
    }
}

static void answer(slot *s)
{
    h2o_req_t *req = s->req;
    const db_job *job = &s->job;
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
        finish(req, 200, &o);
        return;
    case JOB_POST:
        if (job->status == 404) {
            REPLY_ERROR(req, 404, "post not found");
            return;
        }
        if (job->status != 200)
            break;
        o = begin();
        PUT(&o, "{\"post\":");
        put_post(&o, job->result, 0);
        PUT(&o, "}");
        finish(req, 200, &o);
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
        finish(req, 201, &o);
        return;
    case JOB_LIKE:
        if (job->status == 404) {
            REPLY_ERROR(req, 404, "post not found");
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
        finish(req, job->status, &o);
        return;
    case JOB_PING:
        o = begin();
        if (job->status == 200) {
            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);
            PUT(&o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            put_int(&o, now.tv_sec - started.tv_sec);
            PUT(&o, "}");
            finish(req, 200, &o);
        } else {
            PUT(&o, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
            put_string(&o, job->error, strlen(job->error));
            PUT(&o, "}");
            finish(req, 503, &o);
        }
        return;
    default:
        break;
    }
    REPLY_ERROR(req, 500, "internal server error");
}

/* On the loop thread, once the job's statement has returned. */
/* --- the database's descriptors ----------------------------------------------------------- */

static dbwatch *watch_of(loop *l, db_conn *c)
{
    int i = db_conn_tag(c);

    if (i < 0) {
        i = l->dbw_count++;
        db_conn_set_tag(c, i);
        l->dbw[i].conn = c;
        l->dbw[i].fd = -1;
        l->dbw[i].loop = l;
    }
    return &l->dbw[i];
}

static void on_db_readable(h2o_socket_t *sock, const char *err)
{
    dbwatch *w = sock->data;

    db_on_readable(w->conn, err != NULL);
}

static void on_db_writable(h2o_socket_t *sock, const char *err)
{
    dbwatch *w = sock->data;

    if (err != NULL)
        db_on_readable(w->conn, 1);
    else
        db_on_writable(w->conn);
}

static void db_unwatch(void *arg, db_conn *c, int fd)
{
    dbwatch *w = watch_of(arg, c);

    (void)fd;
    if (w->sock != NULL) {
        h2o_socket_close(w->sock);
        w->sock = NULL;
    }
    w->fd = -1;
}

static void db_watch_read(void *arg, db_conn *c, int fd)
{
    loop *l = arg;
    dbwatch *w = watch_of(l, c);

    if (w->sock != NULL && w->fd != fd)
        db_unwatch(arg, c, w->fd);
    if (w->sock == NULL) {
        const int copy = dup(fd);

        if (copy < 0)
            abort();
        w->sock = h2o_evloop_socket_create(l->ctx.loop, copy, H2O_SOCKET_FLAG_DONT_READ);
        w->sock->data = w;
        w->fd = fd;
    }
    h2o_socket_read_stop(w->sock);
    h2o_socket_read_start(w->sock, on_db_readable);
}

static void db_watch_write(void *arg, db_conn *c, int fd)
{
    dbwatch *w = watch_of(arg, c);

    (void)fd;
    h2o_socket_notify_write(w->sock, on_db_writable);
}

static const db_host db_host_ops = {db_watch_read, db_watch_write, db_unwatch};

/* How long the statements of an iteration wait for company before their sync goes out. Without
 * the wait a pass of h2o's loop rarely holds more than one statement per connection, and the
 * pipeline saves nothing; with it the batches form, and a request costs a millisecond more. */
#define DB_FLUSH_MS 1

static void db_flush(h2o_timer_t *timer)
{
    loop *l = H2O_STRUCT_FROM_MEMBER(loop, db_flush_timer, timer);

    db_pool_flush(l->db);
}

static void db_tick(h2o_timer_t *timer)
{
    loop *l = H2O_STRUCT_FROM_MEMBER(loop, db_timer, timer);

    db_pool_tick(l->db);
    h2o_timer_link(l->ctx.loop, 1000, &l->db_timer);
}

static void on_job_done(db_job *job)
{
    slot *s = H2O_STRUCT_FROM_MEMBER(slot, job, job);

    if (s->req != NULL)
        answer(s);
    if (s->job.result != NULL) {
        PQclear(s->job.result);
        s->job.result = NULL;
    }
    unpark(s);
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

static int authenticate(h2o_req_t *req, int64_t *uid, char *name, size_t *name_len)
{
    const ssize_t i = h2o_find_header(&req->headers, H2O_TOKEN_AUTHORIZATION, -1);
    const char *auth = i != -1 ? req->headers.entries[i].value.base : NULL;
    const size_t auth_len = i != -1 ? req->headers.entries[i].value.len : 0;

    switch (jwt_verify(auth, auth_len, uid, name, name_len)) {
    case JWT_OK: return 0;
    case JWT_MISSING: REPLY_ERROR(req, 401, "missing bearer token"); break;
    case JWT_INVALID: REPLY_ERROR(req, 401, "invalid or expired token"); break;
    default: REPLY_ERROR(req, 401, "invalid token payload"); break;
    }
    return -1;
}

static void read_job(h2o_req_t *req, int kind, int64_t id)
{
    slot *s = park(req);

    if (s == NULL) {
        REPLY_ERROR(req, 503, "internal server error");
        return;
    }
    s->job.kind = kind;
    s->job.post_id = id;
    submit(req, s);
}

static void post_like(h2o_req_t *req, int64_t uid, int64_t id)
{
    slot *s = park(req);

    if (s == NULL) {
        REPLY_ERROR(req, 503, "internal server error");
        return;
    }
    s->job.kind = JOB_LIKE;
    s->job.user_id = uid;
    s->job.post_id = id;
    submit(req, s);
}

/*
 * The request body is parsed in the loop's arena (the parser copies its input there), the
 * trimmed `body` is copied into the slot, and nothing of the request is needed after that.
 */
static void post_create(h2o_req_t *req, int64_t uid, const char *name, size_t name_len)
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

    if (req->entity.len == 0) {
        REPLY_ERROR(req, 400, "malformed JSON body");
        return;
    }
    if (arnm_init_arena_borrow(&arena, self->parse_buf, PARSE_ARENA) != ARNM_SUCCESS ||
        arnm_json_reader_init(&reader, &arena) != ARNM_SUCCESS) {
        REPLY_ERROR(req, 500, "internal server error");
        return;
    }
    parsed = arnm_json_reader_parse(&reader, req->entity.base, (uint32_t)req->entity.len, false, &root);
    if (parsed == ARNM_ERROR_OUT_OF_MEMORY) {
        REPLY_ERROR(req, 500, "internal server error");
        return;
    }
    if (parsed != ARNM_SUCCESS) {
        REPLY_ERROR(req, 400, "malformed JSON body");
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
        REPLY_ERROR(req, 400, "body is required");
    } else if (chars > 500 || (size_t)(e - p) > DB_BODY_MAX) {
        REPLY_ERROR(req, 400, "body must be at most 500 characters");
    } else if ((s = park(req)) == NULL) {
        REPLY_ERROR(req, 503, "internal server error");
    } else {
        s->job.kind = JOB_CREATE;
        s->job.user_id = uid;
        s->job.body = s->body;
        s->job.body_len = (uint32_t)(e - p);
        memcpy(s->body, p, (size_t)(e - p));
        memcpy(s->author, name, name_len);
        s->author_len = name_len;
        submit(req, s);
    }
}

#define POSTS_SLASH_LENGTH 7

static int on_request(h2o_handler_t *handler, h2o_req_t *req)
{
    const char *path = req->path_normalized.base;
    const size_t len = req->path_normalized.len;
    const int get = h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET"));
    const int post = h2o_memis(req->method.base, req->method.len, H2O_STRLIT("POST"));

    (void)handler;
    if (get) {
        if (len == 5 && memcmp(path, "/feed", 5) == 0) {
            read_job(req, JOB_FEED, 0);
            return 0;
        } else if (len >= POSTS_SLASH_LENGTH && memcmp(path, "/posts/", 7) == 0) {
            const int64_t id = parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH);

            if (id > 0) {
                read_job(req, JOB_POST, id);
            } else {
                const char *slash = memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH);

                if (slash != NULL) REPLY_ERROR(req, 404, "not found");
                else if (id == -1) REPLY_ERROR(req, 404, "post not found");
                else REPLY_ERROR(req, 400, "invalid post id");
            }
            return 0;
        } else if (len == 7 && memcmp(path, "/health", 7) == 0) {
            read_job(req, JOB_PING, 0);
            return 0;
        }
    } else if (post && len >= 6 && memcmp(path, "/posts", 6) == 0) {
        char name[JWT_NAME_MAX];
        size_t name_len;
        int64_t uid;

        if (len == 6) {
            if (authenticate(req, &uid, name, &name_len) == 0)
                post_create(req, uid, name, name_len);
            return 0;
        } else if (len > POSTS_SLASH_LENGTH && path[6] == '/' && memcmp(path + len - 5, "/like", 5) == 0) {
            const int64_t id = len > POSTS_SLASH_LENGTH + 5
                                   ? parse_id(path + POSTS_SLASH_LENGTH, len - POSTS_SLASH_LENGTH - 5)
                                   : 0;

            if (id <= 0) {
                const char *slash = memchr(path + POSTS_SLASH_LENGTH, '/', len - POSTS_SLASH_LENGTH);

                if (slash != path + len - 5) {
                    REPLY_ERROR(req, 404, "not found");
                    return 0;
                }
            }
            /* Auth first, then the id: the spec says so. */
            if (authenticate(req, &uid, name, &name_len) == 0) {
                if (id > 0) post_like(req, uid, id);
                else if (id == 0) REPLY_ERROR(req, 400, "invalid post id");
                else REPLY_ERROR(req, 404, "post not found");
            }
            return 0;
        }
    }

    REPLY_ERROR(req, 404, "not found");
    return 0;
}

/* --- accepting ---------------------------------------------------------------------------- */

static void on_accept(h2o_socket_t *listener, const char *err);

static void resume_accept(h2o_timer_t *timer)
{
    loop *l = H2O_STRUCT_FROM_MEMBER(loop, accept_timer, timer);

    for (int i = 0; i < l->listener_count; i++)
        if (!h2o_socket_is_reading(l->listeners[i]))
            h2o_socket_read_start(l->listeners[i], on_accept);
}

static void on_accept(h2o_socket_t *listener, const char *err)
{
    loop *l = listener->data;

    if (err != NULL)
        return;
    /* Several per wake-up: while users are arriving, one readiness event covers many of them. */
    for (int i = 0; i < 64; i++) {
        h2o_socket_t *sock = h2o_evloop_socket_accept(listener);

        if (sock == NULL) {
            /* Out of descriptors (or memory): every connection the service may hold is open.
             * The listeners would stay readable and spin the loop, so they are left alone for
             * a moment; new users wait in the backlog until others have gone. */
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                for (int k = 0; k < l->listener_count; k++)
                    if (h2o_socket_is_reading(l->listeners[k]))
                        h2o_socket_read_stop(l->listeners[k]);
                if (!h2o_timer_is_linked(&l->accept_timer))
                    h2o_timer_link(l->ctx.loop, 100, &l->accept_timer);
            }
            return;
        }
        h2o_accept(&l->accept_ctx, sock);
    }
}

static int listen_on(const struct sockaddr_in *addr)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

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

    self = l;
    if ((l->gz = gz_create()) == NULL || (l->json = malloc(JSON_CAP)) == NULL ||
        (l->gzbuf = malloc(JSON_CAP + 1024)) == NULL ||
        (l->parse_buf = aligned_alloc(16, PARSE_ARENA)) == NULL)
        abort();
    /* The statements an iteration put on the database go out batched per connection, once
     * the millisecond in which more may join them is over. */
    while (h2o_evloop_run(l->ctx.loop, INT32_MAX) == 0)
        if (db_pool_pending(l->db) && !h2o_timer_is_linked(&l->db_flush_timer))
            h2o_timer_link(l->ctx.loop, DB_FLUSH_MS, &l->db_flush_timer);
    perror("h2o_evloop_run");
    exit(1);
}

int server_run(const char *host, uint16_t port, uint16_t extra_port, const char *database_url,
               int threads, int pool_size)
{
    struct sockaddr_in addrs[LISTENERS];
    int ports = 1;
    h2o_hostconf_t *hostconf;
    h2o_pathconf_t *pathconf;
    h2o_handler_t *handler;
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
        threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (threads < 1)
        threads = 1;
    clock_gettime(CLOCK_MONOTONIC, &started);

    h2o_config_init(&config);
    config.server_name = h2o_iovec_init(NULL, 0);
    config.max_request_entity_size = MAX_BODY;
    /* Both timers run while a connection waits for its next request, and the shorter one closes
     * it: h2o's 5 s I/O timeout would end every keep-alive connection of a user who reads for
     * longer than that, whatever the request timeout says. */
    config.http1.req_timeout = IDLE_TIMEOUT_MS;
    config.http1.req_io_timeout = IDLE_TIMEOUT_MS;
    config.http1.upgrade_to_http2 = 0;
    hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    /* One handler for every path: routing is five comparisons, and a path nothing matches has
     * to be answered with this API's 404 body rather than h2o's. */
    pathconf = h2o_config_register_path(hostconf, "/", 0);
    handler = h2o_create_handler(pathconf, sizeof *handler);
    handler->on_req = on_request;

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

        h2o_context_init(&l->ctx, h2o_evloop_create(), &config);
        l->accept_ctx.ctx = &l->ctx;
        l->accept_ctx.hosts = config.hosts;
        h2o_timer_init(&l->accept_timer, resume_accept);
        {
            /* The connections, dealt out as evenly as they go. */
            const int share = connected / threads + (i < connected % threads);

            if ((l->dbw = calloc((size_t)share, sizeof *l->dbw)) == NULL)
                abort();
            l->db = db_pool_create(conns + given, share, on_job_done, &db_host_ops, l);
            given += share;
            h2o_timer_init(&l->db_timer, db_tick);
            h2o_timer_init(&l->db_flush_timer, db_flush);
            h2o_timer_link(l->ctx.loop, 1000, &l->db_timer);
        }

        if ((l->slots = calloc(SLOTS, sizeof *l->slots)) == NULL)
            abort();
        for (int k = 0; k < SLOTS; k++) {
            l->slots[k].loop = l;
            l->slots[k].next_free = l->free_slots;
            l->free_slots = &l->slots[k];
        }

        for (int p = 0; p < ports; p++) {
            if ((l->listen_fds[p] = listen_on(&addrs[p])) == -1)
                return 1;
            l->listeners[p] = h2o_evloop_socket_create(l->ctx.loop, l->listen_fds[p], H2O_SOCKET_FLAG_DONT_READ);
            l->listeners[p]->data = l;
            h2o_socket_read_start(l->listeners[p], on_accept);
            l->listener_count++;
        }
    }
    {
        char extra[32] = "";

        if (ports == 2)
            snprintf(extra, sizeof extra, " and %u", (unsigned)extra_port);
        fprintf(stderr, "listening on %s:%u%s, %d loop%s, %d of %d database connections, gzip: %s\n",
                host, (unsigned)port, extra, threads, threads == 1 ? "" : "s", connected, pool_size,
                gz_backend());
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
