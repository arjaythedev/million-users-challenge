/*
 * _pq.Loop: one worker's event loop, in C: epoll, HTTP/1.1, and the two hot reads end to end.
 *
 * Linked into _pq because it drives the same connections. It serves the traffic that decides the
 * score (94% of requests are GET /feed and GET /posts/:id) without creating a Python object:
 *
 *   recv -> parse in place -> prepared statement on a read lane -> binary result -> JSON -> gzip -> send
 *
 * Everything else goes over the bridge to the Python use cases: writes, JWTs, /health, every error
 * and any input this file does not recognise with certainty. So the domain stays the only authority
 * on the rules, and this file only recognises the plainly valid. What it duplicates (the JSON format,
 * the shape of a valid id) is held to the Python path byte for byte by tests/test_fastloop.py.
 *
 * Rule notes: each read runs the SPEC's statement, prepared, one per request, against Postgres while
 * it is being served. Nothing here outlives a response. JSON is built here, on the app box.
 *
 * Design, from what the top entries of the challenge measured:
 *  - A connection is a 40-byte slot in a table indexed by fd. Its pages are touched only when used,
 *    and it owns a buffer only while it holds a partial request or a response the socket refused.
 *  - One pass's reads go to the least loaded lane under one Sync: one write and one wake-up per batch.
 *  - After a light pass the loop sleeps ~2 ms, so that the next wake-up serves everything that arrived
 *    meanwhile. A busy loop never sleeps, so this costs a little latency and no throughput.
 *
 * It also offers the part of the asyncio loop API that Lane uses (add_reader, call_soon and so on),
 * so the write lanes' group commit runs on it unchanged. There are no timers, because nothing needs one.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <libpq-fe.h>
#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "_pq.h"

#define MAX_EVENTS 1024
#define READ_BUF (64 * 1024)
#define MAX_HEAD 16384
#define MAX_BODY 65536
#define MAX_BUFFERED (1024 * 1024)  /* what one client may have parsed-pending: a misbehaving pipeliner */
#define MAX_BATCH 64                /* reads under one Sync */
#define MAX_DRAIN 8                 /* input buffers read per lane per event, for fairness to clients */
#define MAX_PY_FDS 1024
#define MAX_ID_DIGITS 18            /* always below MAX_POST_ID; a longer id goes to the domain's rule */
#define HEAD_ROOM 256               /* room for the response head, in front of the body */
#define MAX_TABLE (16 * 1024 * 1024)

enum { K_FREE, K_CLIENT, K_LISTENER, K_LANE, K_PY, K_WAKE };
enum { F_BUSY = 1, F_PARSING = 2, F_CLOSE_AFTER = 4, F_WANT_OUT = 8 };
enum { J_FEED = 1, J_GZIP = 2, J_CLOSE = 4 };

typedef struct {
    char *in, *out;
    uint32_t in_len, in_cap, out_len, out_off;
    uint32_t gen;  /* bumped on close: a reply or an event for an older connection on this fd is stale */
    uint8_t kind, flags;
    uint16_t aux;  /* K_LANE, K_PY: index into their tables */
} Slot;

typedef struct {
    int fd;
    uint32_t gen;
    uint8_t flags;
    char id[MAX_ID_DIGITS + 1];
} Job;

typedef struct {
    PyObject *conn;
    PGconn *pg;
    int fd;
    int up, in_pass, writing;
    uint32_t batch;      /* statements since the last Sync */
    Py_ssize_t queued;   /* Syncs sent whose result has not been read */
    Job *ring;
    uint32_t head, count, cap;
} NativeLane;

typedef struct { int fd; PyObject *reader, *writer; } PyFd;

typedef struct {
    PyObject_HEAD
    int ep, wake_fd;
    Slot *slots;
    size_t nslots;
    struct epoll_event *events;
    char *rbuf;
    char *json; size_t json_cap;
    char *gz;   size_t gz_cap;
    PyObject *soon;       /* list of callbacks to run at the end of the pass */
    PyObject *bridge, *on_lane_down;
    PyFd py[MAX_PY_FDS];
    NativeLane *lanes;
    int nlanes;
    NativeLane *current;  /* the lane taking this pass's reads */
    int *used; int nused;
    compress_fn compress; size_t gzip_min;
    PyObject *not_found, *internal;  /* bodies, rendered by the application's contract */
    long window_ns; int light_pass;
    int more_to_read;  /* a lane stopped draining early in this pass */
} Loop;

/* ------------------------------------------------------------------ epoll */

static inline uint64_t tag(int fd, uint32_t gen) { return ((uint64_t)gen << 32) | (uint32_t)fd; }

static int ep_set(Loop *L, int op, int fd, uint32_t events) {
    struct epoll_event ev = {.events = events, .data.u64 = tag(fd, L->slots[fd].gen)};
    return epoll_ctl(L->ep, op, fd, &ev);
}

static inline Slot *slot_of(Loop *L, int fd) { return &L->slots[fd]; }

static int grow_slot_buf(char **buf, uint32_t *cap, size_t need) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    char *p = realloc(*buf, c);
    if (!p) return -1;
    *buf = p; *cap = (uint32_t)c;
    return 0;
}

/* ------------------------------------------------------------------ clients */

static void client_parse(Loop *L, int fd, const char *data, size_t len);

static void client_close(Loop *L, int fd) {
    Slot *s = slot_of(L, fd);
    free(s->in);
    free(s->out);
    uint32_t gen = s->gen + 1;
    memset(s, 0, sizeof *s);
    s->gen = gen;
    close(fd);  /* also leaves the epoll set */
}

/* Sends what the socket takes, keeps the rest for EPOLLOUT. False if the connection is gone. */
static int client_send(Loop *L, int fd, const char *data, size_t len, int close_after) {
    Slot *s = slot_of(L, fd);
    size_t sent = 0;
    if (s->out_len == 0) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { client_close(L, fd); return 0; }
        } else {
            sent = (size_t)n;
        }
    }
    if (sent < len) {
        size_t rest = len - sent;
        char *p = realloc(s->out, s->out_len + rest);
        if (!p) { client_close(L, fd); return 0; }
        memcpy(p + s->out_len, data + sent, rest);
        s->out = p;
        s->out_len += (uint32_t)rest;
        if (!(s->flags & F_WANT_OUT)) {
            s->flags |= F_WANT_OUT;
            ep_set(L, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLOUT);
        }
        if (close_after) s->flags |= F_CLOSE_AFTER;
        return 1;
    }
    if (close_after) { client_close(L, fd); return 0; }
    return 1;
}

static void client_writable(Loop *L, int fd) {
    Slot *s = slot_of(L, fd);
    while (s->out_off < s->out_len) {
        ssize_t n = send(fd, s->out + s->out_off, s->out_len - s->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            client_close(L, fd);
            return;
        }
        s->out_off += (uint32_t)n;
    }
    free(s->out);
    s->out = NULL;
    s->out_len = s->out_off = 0;
    s->flags &= ~F_WANT_OUT;
    if (s->flags & F_CLOSE_AFTER) { client_close(L, fd); return; }
    ep_set(L, EPOLL_CTL_MOD, fd, EPOLLIN);
}

/* The request in flight has been answered: go on with what was pipelined behind it. */
static void client_finish(Loop *L, int fd) {
    Slot *s = slot_of(L, fd);
    s->flags &= ~F_BUSY;
    if (!(s->flags & F_PARSING) && s->in_len) client_parse(L, fd, s->in, s->in_len);
}

static int client_append(Slot *s, const char *data, size_t len) {
    if (s->in_len + len > MAX_BUFFERED) return -1;
    if (grow_slot_buf(&s->in, &s->in_cap, s->in_len + len) < 0) return -1;
    memcpy(s->in + s->in_len, data, len);
    s->in_len += (uint32_t)len;
    return 0;
}

static void client_readable(Loop *L, int fd) {
    ssize_t n = recv(fd, L->rbuf, READ_BUF, 0);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        client_close(L, fd);
        return;
    }
    if (n < 0) return;
    Slot *s = slot_of(L, fd);
    if (s->in_len || (s->flags & F_BUSY)) {
        if (client_append(s, L->rbuf, (size_t)n) < 0) { client_close(L, fd); return; }
        if (!(s->flags & F_BUSY)) client_parse(L, fd, s->in, s->in_len);
    } else {
        client_parse(L, fd, L->rbuf, (size_t)n);
    }
}

static void accept_all(Loop *L, int lfd) {
    for (int i = 0; i < 256; i++) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        if ((size_t)fd >= L->nslots) { close(fd); continue; }
        Slot *s = slot_of(L, fd);
        s->kind = K_CLIENT;
        if (ep_set(L, EPOLL_CTL_ADD, fd, EPOLLIN) < 0) { client_close(L, fd); continue; }
        client_readable(L, fd);  /* TCP_DEFER_ACCEPT: the first request is usually here already */
    }
}

/* ------------------------------------------------------------------ responses */

static const char H_JSON[] = "\r\nContent-Type: application/json\r\n";
static const char H_GZIP[] = "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n";

/* body sits at p + HEAD_ROOM in a buffer of the caller's; the head is written right in front of it. */
static void respond_native(Loop *L, int fd, const char *status, char *body, size_t n, int gzipped, int close_after) {
    char head[HEAD_ROOM];
    int h = snprintf(head, sizeof head, "HTTP/1.1 %s%s%sContent-Length: %zu%s\r\n\r\n", status, H_JSON,
                     gzipped ? H_GZIP : "", n, close_after ? "\r\nConnection: close" : "");
    char *start = body - h;
    memcpy(start, head, (size_t)h);
    client_send(L, fd, start, (size_t)h + n, close_after);
}

/* JSON in L->json + HEAD_ROOM, n bytes. Compress if it is worth it, and answer. */
static void respond_json(Loop *L, const Job *job, const char *status, size_t n) {
    char *body = L->json + HEAD_ROOM;
    int fd = job->fd;
    int close_after = job->flags & J_CLOSE;
    if ((job->flags & J_GZIP) && L->compress && n >= L->gzip_min) {
        size_t cap = n + n / 8 + 1024;
        if (cap + HEAD_ROOM > L->gz_cap) {
            char *p = realloc(L->gz, cap + HEAD_ROOM);
            if (p) { L->gz = p; L->gz_cap = cap + HEAD_ROOM; }
        }
        if (cap + HEAD_ROOM <= L->gz_cap) {
            size_t z = L->compress(body, n, L->gz + HEAD_ROOM, cap);
            if (z) {
                respond_native(L, fd, status, L->gz + HEAD_ROOM, z, 1, close_after);
                return;
            }
        }
    }
    respond_native(L, fd, status, body, n, 0, close_after);
}

static void respond_bytes(Loop *L, const Job *job, const char *status, PyObject *bytes) {
    size_t n = (size_t)PyBytes_GET_SIZE(bytes);
    memcpy(L->json + HEAD_ROOM, PyBytes_AS_STRING(bytes), n);
    respond_json(L, job, status, n);
}

/* ------------------------------------------------------------------ JSON from binary rows */

static inline char *put_u64(char *w, uint64_t v) {
    char tmp[20];
    int i = 0;
    do { tmp[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) *w++ = tmp[--i];
    return w;
}

static inline char *put_i64(char *w, int64_t v) {
    if (v < 0) { *w++ = '-'; return put_u64(w, (uint64_t)0 - (uint64_t)v); }
    return put_u64(w, (uint64_t)v);
}

/* Same escaping as _cjson.c: json.encoder.encode_basestring's, non-ASCII copied as UTF-8. */
static inline char *put_str(char *w, const char *s, size_t n) {
    *w++ = '"';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') { *w++ = (char)c; continue; }
        *w++ = '\\';
        switch (c) {
        case '"': *w++ = '"'; break;
        case '\\': *w++ = '\\'; break;
        case '\b': *w++ = 'b'; break;
        case '\f': *w++ = 'f'; break;
        case '\n': *w++ = 'n'; break;
        case '\r': *w++ = 'r'; break;
        case '\t': *w++ = 't'; break;
        default: {
            static const char hex[] = "0123456789abcdef";
            *w++ = 'u'; *w++ = '0'; *w++ = '0'; *w++ = hex[c >> 4]; *w++ = hex[c & 15];
        }
        }
    }
    *w++ = '"';
    return w;
}

static inline char *put_2(char *w, int v) { *w++ = (char)('0' + v / 10); *w++ = (char)('0' + v % 10); return w; }

/* timestamptz binary: microseconds since 2000-01-01 UTC -> "YYYY-MM-DDTHH:MM:SS.mmmZ", truncated. */
char *fastloop_put_time(char *w, long long us) {
    int64_t secs = us / 1000000, frac = us % 1000000;
    if (frac < 0) { frac += 1000000; secs--; }
    int64_t days = secs / 86400, rem = secs % 86400;
    if (rem < 0) { rem += 86400; days--; }
    /* civil_from_days, shifted to 0000-03-01 (2000-01-01 is day 730425 of that era count) */
    int64_t z = days + 730425;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    int64_t y = (int64_t)yoe + era * 400 + (m <= 2);
    w = put_2(w, (int)(y / 100));
    w = put_2(w, (int)(y % 100));
    *w++ = '-'; w = put_2(w, m);
    *w++ = '-'; w = put_2(w, d);
    *w++ = 'T'; w = put_2(w, (int)(rem / 3600));
    *w++ = ':'; w = put_2(w, (int)(rem / 60 % 60));
    *w++ = ':'; w = put_2(w, (int)(rem % 60));
    *w++ = '.';
    int ms = (int)(frac / 1000);
    *w++ = (char)('0' + ms / 100); w = put_2(w, ms % 100);
    *w++ = 'Z';
    return w;
}

static inline int64_t be_i64(const char *p) { int64_t v; memcpy(&v, p, 8); return (int64_t)be64toh((uint64_t)v); }
static inline int32_t be_i32(const char *p) { int32_t v; memcpy(&v, p, 4); return (int32_t)be32toh((uint32_t)v); }

#define LIT(w, s) (memcpy((w), (s), sizeof(s) - 1), (w) + sizeof(s) - 1)

/* Worst case for a row: every text byte escaped to 6, plus the fixed parts. */
static size_t row_bound(const PGresult *r, int row) {
    return (size_t)PQgetlength(r, row, 1) * 6 + (size_t)PQgetlength(r, row, 3) * 6 + 200;
}

static char *put_row(char *w, const PGresult *r, int row) {
    w = LIT(w, "{\"id\":");
    w = put_i64(w, be_i64(PQgetvalue(r, row, 0)));
    w = LIT(w, ",\"body\":");
    w = put_str(w, PQgetvalue(r, row, 1), (size_t)PQgetlength(r, row, 1));
    w = LIT(w, ",\"created_at\":\"");
    w = fastloop_put_time(w, be_i64(PQgetvalue(r, row, 2)));
    w = LIT(w, "\",\"author\":");
    w = put_str(w, PQgetvalue(r, row, 3), (size_t)PQgetlength(r, row, 3));
    w = LIT(w, ",\"like_count\":");
    w = put_i64(w, be_i32(PQgetvalue(r, row, 4)));
    *w++ = '}';
    return w;
}

/* The JSON for a feed or post result into L->json + HEAD_ROOM. -1: out of memory. */
static Py_ssize_t render(Loop *L, const PGresult *r, int feed) {
    int rows = PQntuples(r);
    size_t need = HEAD_ROOM + 32;
    for (int i = 0; i < rows; i++) need += row_bound(r, i);
    if (need > L->json_cap) {
        char *p = realloc(L->json, need);
        if (!p) return -1;
        L->json = p;
        L->json_cap = need;
    }
    char *start = L->json + HEAD_ROOM, *w = start;
    if (feed) {
        w = LIT(w, "{\"posts\":[");
        for (int i = 0; i < rows; i++) {
            if (i) *w++ = ',';
            w = put_row(w, r, i);
        }
        w = LIT(w, "]}");
    } else {
        w = LIT(w, "{\"post\":");
        w = put_row(w, r, 0);
        *w++ = '}';
    }
    return w - start;
}

/* ------------------------------------------------------------------ read lanes */

static void lane_fail(Loop *L, NativeLane *lane);

static int lane_push(NativeLane *lane, const Job *job) {
    if (lane->count == lane->cap) {
        uint32_t cap = lane->cap ? lane->cap * 2 : 256;
        Job *ring = malloc(cap * sizeof(Job));
        if (!ring) return -1;
        for (uint32_t i = 0; i < lane->count; i++) ring[i] = lane->ring[(lane->head + i) & (lane->cap - 1)];
        free(lane->ring);
        lane->ring = ring;
        lane->head = 0;
        lane->cap = cap;
    }
    lane->ring[(lane->head + lane->count) & (lane->cap - 1)] = *job;
    lane->count++;
    return 0;
}

static inline Job lane_pop(NativeLane *lane) {
    Job job = lane->ring[lane->head];
    lane->head = (lane->head + 1) & (lane->cap - 1);
    lane->count--;
    return job;
}

static NativeLane *pick_lane(Loop *L) {
    NativeLane *best = L->current;
    if (best && best->up && best->batch < MAX_BATCH) return best;
    best = NULL;
    for (int i = 0; i < L->nlanes; i++) {
        NativeLane *lane = &L->lanes[i];
        if (lane->up && (!best || lane->count < best->count)) best = lane;
    }
    return L->current = best;
}

static int lane_send(Loop *L, NativeLane *lane, const Job *job) {
    const char *params[1] = {job->id};
    int ok = (job->flags & J_FEED)
        ? PQsendQueryPrepared(lane->pg, "feed", 0, NULL, NULL, NULL, 1)
        : PQsendQueryPrepared(lane->pg, "post", 1, params, NULL, NULL, 1);
    if (!ok) return -1;
    if (!lane->in_pass) {
        lane->in_pass = 1;
        L->used[L->nused++] = (int)(lane - L->lanes);
    }
    lane->batch++;
    return lane_push(lane, job);
}

static void job_failed(Loop *L, const Job *job) {
    Slot *s = slot_of(L, job->fd);
    if (s->kind != K_CLIENT || s->gen != job->gen) return;
    respond_bytes(L, job, "500 Internal Server Error", L->internal);
    if (slot_of(L, job->fd)->gen == job->gen) client_finish(L, job->fd);
}

/* All of this pass's reads have been sent: a Sync and one write per lane used. */
static void flush_lanes(Loop *L) {
    int n = L->nused;
    L->nused = 0;
    L->current = NULL;
    for (int i = 0; i < n; i++) {
        NativeLane *lane = &L->lanes[L->used[i]];
        lane->in_pass = 0;
        if (!lane->up) continue;
        lane->batch = 0;
#ifdef LIBPQ_HAS_SEND_PIPELINE_SYNC
        int ok = PQsendPipelineSync(lane->pg);
#else
        int ok = PQpipelineSync(lane->pg);
#endif
        if (!ok) { lane_fail(L, lane); continue; }
        lane->queued++;
        int r = PQflush(lane->pg);
        if (r < 0) { lane_fail(L, lane); continue; }
        if (r == 1 && !lane->writing) {
            lane->writing = 1;
            ep_set(L, EPOLL_CTL_MOD, lane->fd, EPOLLIN | EPOLLOUT);
        }
    }
}

static void lane_answer(Loop *L, const Job *job, const PGresult *r) {
    Slot *s = slot_of(L, job->fd);
    if (s->kind != K_CLIENT || s->gen != job->gen) return;  /* the client went away */
    int fd = job->fd;
    uint32_t gen = job->gen;
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQnfields(r) != 5) {
        respond_bytes(L, job, "500 Internal Server Error", L->internal);
    } else if (PQntuples(r) == 0 && !(job->flags & J_FEED)) {
        respond_bytes(L, job, "404 Not Found", L->not_found);
    } else {
        Py_ssize_t n = render(L, r, job->flags & J_FEED);
        if (n < 0) respond_bytes(L, job, "500 Internal Server Error", L->internal);
        else respond_json(L, job, "200 OK", (size_t)n);
    }
    if (slot_of(L, fd)->gen == gen) client_finish(L, fd);
}

/* Every complete reply libpq holds. -> how many there were. */
static int lane_replies(Loop *L, NativeLane *lane) {
    int nulls = 0, handled = 0;
    /* PQisBusy first, always: in nonblocking mode PQgetResult on a busy connection would wait. */
    while (lane->up && lane->queued > 0 && !PQisBusy(lane->pg)) {
        PGresult *r = PQgetResult(lane->pg);
        if (!r) {
            if (++nulls > 1) break;
            continue;
        }
        nulls = 0;
        handled++;
        switch (PQresultStatus(r)) {
        case PGRES_PIPELINE_SYNC:
            lane->queued--;
            break;
        case PGRES_PIPELINE_ABORTED: {  /* skipped because an earlier read of its batch failed: again */
            Job job = lane_pop(lane);
            if (lane_send(L, lane, &job) < 0) {
                job_failed(L, &job);
                PQclear(r);
                lane_fail(L, lane);
                return handled;
            }
            break;
        }
        default: {
            Job job = lane_pop(lane);
            lane_answer(L, &job, r);
        }
        }
        PQclear(r);
    }
    return handled;
}

/* libpq reads about one input buffer per PQconsumeInput, far less than a batch of feeds. So keep
 * going while that makes progress; a lane that has not caught up by MAX_DRAIN also keeps the loop
 * from pausing, or its replies would wait in the socket while the loop sleeps with work to do. */
static void lane_readable(Loop *L, NativeLane *lane) {
    for (int i = 0; i < MAX_DRAIN; i++) {
        if (!PQconsumeInput(lane->pg)) { lane_fail(L, lane); return; }
        int handled = lane_replies(L, lane);
        if (!lane->up || !handled || !lane->queued) return;
    }
    L->more_to_read = 1;
}

static void lane_writable(Loop *L, NativeLane *lane) {
    int r = PQflush(lane->pg);
    if (r < 0) { lane_fail(L, lane); return; }
    if (r == 0) {
        lane->writing = 0;
        ep_set(L, EPOLL_CTL_MOD, lane->fd, EPOLLIN);
    }
}

/* Fails what the lane holds, and tells Python, which reconnects it (see fast.py). */
static void lane_fail(Loop *L, NativeLane *lane) {
    if (!lane->up) return;
    lane->up = 0;
    if (L->current == lane) L->current = NULL;
    Slot *s = slot_of(L, lane->fd);
    uint32_t gen = s->gen + 1;
    memset(s, 0, sizeof *s);
    s->gen = gen;
    while (lane->count) {
        Job job = lane_pop(lane);
        job_failed(L, &job);
    }
    lane->batch = 0;
    lane->queued = 0;
    PyObject *conn = lane->conn;
    lane->conn = NULL;
    lane->pg = NULL;
    PyObject *r = PyObject_CallMethod(conn, "close", NULL);
    if (r) Py_DECREF(r); else PyErr_WriteUnraisable(conn);
    Py_DECREF(conn);
    r = PyObject_CallFunction(L->on_lane_down, "i", (int)(lane - L->lanes));
    if (r) Py_DECREF(r); else PyErr_WriteUnraisable(L->on_lane_down);
}

/* ------------------------------------------------------------------ the parser */

static inline int is_digit(char c) { return c >= '0' && c <= '9'; }

/* One request. Returns its total length, 0 if it is not complete yet, -1 to drop the connection. */
typedef struct {
    const char *method, *path, *auth, *body;
    size_t method_len, path_len, auth_len, body_len;
    int gzip, close;
} Request;

static inline const char *trim(const char *p, const char *end, size_t *len) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *len = (size_t)(end - p);
    return p;
}

static Py_ssize_t parse_request(const char *data, size_t len, Request *q) {
    const char *head_end = memmem(data, len, "\r\n\r\n", 4);
    if (!head_end) return len > MAX_HEAD ? -1 : 0;
    size_t head_len = (size_t)(head_end - data);
    const char *line_end = memchr(data, '\r', head_len);
    if (!line_end) line_end = head_end;
    const char *sp1 = memchr(data, ' ', (size_t)(line_end - data));
    if (!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
    if (!sp2) return -1;
    q->method = data;
    q->method_len = (size_t)(sp1 - data);
    q->path = sp1 + 1;
    q->path_len = (size_t)(sp2 - sp1 - 1);
    const char *qm = memchr(q->path, '?', q->path_len);
    if (qm) q->path_len = (size_t)(qm - q->path);
    q->auth = NULL;
    q->auth_len = 0;
    q->gzip = q->close = 0;
    size_t body_len = 0;
    const char *p = line_end;
    while (p < head_end) {
        /* p is at the CRLF ending the previous line */
        p += 2;
        const char *e = memchr(p, '\r', (size_t)(head_end - p));
        if (!e || e > head_end) e = head_end;
        size_t n = (size_t)(e - p);
        size_t vlen;
        switch (*p | 0x20) {
        case 'a':
            if (n > 14 && !strncasecmp(p, "authorization:", 14)) {
                q->auth = trim(p + 14, e, &q->auth_len);
                if (!q->auth) q->auth_len = 0;
            } else if (n > 16 && !strncasecmp(p, "accept-encoding:", 16)) {
                q->gzip = memmem(p + 16, n - 16, "gzip", 4) != NULL;
            }
            break;
        case 'c':
            if (n > 15 && !strncasecmp(p, "content-length:", 15)) {
                const char *v = trim(p + 15, e, &vlen);
                if (!vlen || vlen > 7) return -1;
                body_len = 0;
                for (size_t i = 0; i < vlen; i++) {
                    if (!is_digit(v[i])) return -1;
                    body_len = body_len * 10 + (size_t)(v[i] - '0');
                }
                if (body_len > MAX_BODY) return -1;
            } else if (n > 11 && !strncasecmp(p, "connection:", 11)) {
                const char *v = trim(p + 11, e, &vlen);
                q->close = vlen == 5 && !strncasecmp(v, "close", 5);
            }
            break;
        }
        p = e;
    }
    size_t total = head_len + 4 + body_len;
    if (len < total) return 0;
    q->body = head_end + 4;
    q->body_len = body_len;
    return (Py_ssize_t)total;
}

static inline int native_route(const Request *q, Job *job) {
    if (q->method_len != 3 || memcmp(q->method, "GET", 3)) return 0;
    if (q->path_len == 5 && !memcmp(q->path, "/feed", 5)) {
        job->flags |= J_FEED;
        job->id[0] = 0;
        return 1;
    }
    if (q->path_len > 7 && q->path_len <= 7 + MAX_ID_DIGITS && !memcmp(q->path, "/posts/", 7)) {
        const char *id = q->path + 7;
        size_t n = q->path_len - 7;
        if (id[0] < '1' || id[0] > '9') return 0;
        for (size_t i = 1; i < n; i++) if (!is_digit(id[i])) return 0;
        memcpy(job->id, id, n);
        job->id[n] = 0;
        return 1;
    }
    return 0;
}

static void call_bridge(Loop *L, int fd, uint32_t gen, const Request *q) {
    PyObject *args[8] = {NULL};
    args[0] = PyLong_FromLong(fd);
    args[1] = PyLong_FromUnsignedLong(gen);
    args[2] = PyUnicode_DecodeLatin1(q->method, (Py_ssize_t)q->method_len, NULL);
    args[3] = PyUnicode_DecodeLatin1(q->path, (Py_ssize_t)q->path_len, NULL);
    args[4] = q->auth ? PyUnicode_DecodeLatin1(q->auth, (Py_ssize_t)q->auth_len, NULL) : Py_NewRef(Py_None);
    args[5] = PyBytes_FromStringAndSize(q->body, (Py_ssize_t)q->body_len);
    args[6] = PyBool_FromLong(q->gzip);
    args[7] = PyBool_FromLong(q->close);
    PyObject *r = NULL;
    int complete = 1;
    for (int i = 0; i < 8; i++) complete = complete && args[i];
    if (complete) r = PyObject_Vectorcall(L->bridge, args, 8, NULL);
    for (int i = 0; i < 8; i++) Py_XDECREF(args[i]);
    if (r) {
        Py_DECREF(r);
    } else {
        PyErr_WriteUnraisable(L->bridge);
        Slot *s = slot_of(L, fd);
        if (s->kind == K_CLIENT && s->gen == gen) client_close(L, fd);
    }
}

/* Serves what `data` holds until it runs out or a request has to wait. data is L->rbuf or s->in. */
static void client_parse(Loop *L, int fd, const char *data, size_t len) {
    Slot *s = slot_of(L, fd);
    uint32_t gen = s->gen;
    size_t off = 0;
    s->flags |= F_PARSING;
    while (off < len) {
        Request q;
        Py_ssize_t total = parse_request(data + off, len - off, &q);
        if (total < 0) { client_close(L, fd); return; }
        if (total == 0) break;
        Job job;
        job.fd = fd;
        job.gen = gen;
        job.flags = (q.gzip ? J_GZIP : 0) | (q.close ? J_CLOSE : 0);
        s->flags |= F_BUSY;
        if (L->nlanes && native_route(&q, &job)) {
            NativeLane *lane = pick_lane(L);
            if (!lane || lane_send(L, lane, &job) < 0) {
                if (lane) lane_fail(L, lane);
                if (s->gen != gen) return;
                job_failed(L, &job);
                if (s->gen != gen) return;
            }
        } else {
            call_bridge(L, fd, gen, &q);
            if (s->gen != gen) return;  /* closed: its buffers are gone, and data may have been one */
        }
        off += (size_t)total;
        if (s->flags & F_BUSY) break;
    }
    s->flags &= ~F_PARSING;
    size_t rest = len - off;
    if (data == s->in) {
        if (off && rest) memmove(s->in, s->in + off, rest);
        s->in_len = (uint32_t)rest;
    } else if (rest && client_append(s, data + off, rest) < 0) {
        client_close(L, fd);
    }
}

/* ------------------------------------------------------------------ the loop */

static void run_soon(Loop *L) {
    if (PyList_GET_SIZE(L->soon) == 0) return;
    PyObject *batch = L->soon;
    L->soon = PyList_New(0);
    if (!L->soon) { L->soon = batch; PyErr_Clear(); return; }
    Py_ssize_t n = PyList_GET_SIZE(batch);
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *cb = PyList_GET_ITEM(batch, i);
        PyObject *r = PyObject_CallNoArgs(cb);
        if (r) Py_DECREF(r); else PyErr_WriteUnraisable(cb);
    }
    Py_DECREF(batch);
}

static void py_event(Loop *L, int index, uint32_t events) {
    PyFd *h = &L->py[index];
    int gone = events & (EPOLLERR | EPOLLHUP);
    int fd = h->fd;
    uint32_t gen = slot_of(L, fd)->gen;
    if ((events & EPOLLOUT || gone) && h->writer) {
        PyObject *r = PyObject_CallNoArgs(h->writer);
        if (r) Py_DECREF(r); else PyErr_WriteUnraisable(h->writer);
    }
    if (slot_of(L, fd)->gen != gen || slot_of(L, fd)->kind != K_PY) return;
    if ((events & EPOLLIN || gone) && h->reader) {
        PyObject *r = PyObject_CallNoArgs(h->reader);
        if (r) Py_DECREF(r); else PyErr_WriteUnraisable(h->reader);
    }
}

static void dispatch(Loop *L, struct epoll_event *ev) {
    int fd = (int)(uint32_t)ev->data.u64;
    Slot *s = slot_of(L, fd);
    if (s->gen != (uint32_t)(ev->data.u64 >> 32)) return;  /* closed earlier in this pass */
    switch (s->kind) {
    case K_CLIENT:
        if (ev->events & EPOLLOUT) {
            client_writable(L, fd);
            if (s->gen != (uint32_t)(ev->data.u64 >> 32)) return;
        }
        if (ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP)) client_readable(L, fd);
        break;
    case K_LISTENER:
        accept_all(L, fd);
        break;
    case K_LANE: {
        NativeLane *lane = &L->lanes[s->aux];
        if (ev->events & EPOLLOUT) lane_writable(L, lane);
        if (lane->up && (ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP))) lane_readable(L, lane);
        break;
    }
    case K_PY:
        py_event(L, s->aux, ev->events);
        break;
    case K_WAKE: {
        uint64_t v;
        if (read(fd, &v, sizeof v) < 0) { /* nothing to do: it only had to wake us */ }
        break;
    }
    }
}

static PyObject *Loop_run(Loop *L, PyObject *unused) {
    if (!L->bridge) { PyErr_SetString(PyExc_RuntimeError, "serve() first"); return NULL; }
    for (;;) {
        int timeout = PyList_GET_SIZE(L->soon) ? 0 : -1;
        int n;
        Py_BEGIN_ALLOW_THREADS
        n = epoll_wait(L->ep, L->events, MAX_EVENTS, timeout);
        Py_END_ALLOW_THREADS
        if (n < 0) {
            if (errno != EINTR) return PyErr_SetFromErrno(PyExc_OSError);
            if (PyErr_CheckSignals() < 0) return NULL;
            continue;
        }
        L->more_to_read = 0;
        for (int i = 0; i < n; i++) dispatch(L, &L->events[i]);
        flush_lanes(L);
        run_soon(L);
        if (L->nused) flush_lanes(L);
        if (n > 0 && n < L->light_pass && L->window_ns && !L->more_to_read && !PyList_GET_SIZE(L->soon)) {
            struct timespec ts = {0, L->window_ns};
            Py_BEGIN_ALLOW_THREADS
            nanosleep(&ts, NULL);
            Py_END_ALLOW_THREADS
        }
        if (PyErr_CheckSignals() < 0) return NULL;
    }
}

/* ------------------------------------------------------------------ the Python surface */

static int check_fd(Loop *L, int fd) {
    if (fd < 0 || (size_t)fd >= L->nslots) {
        PyErr_Format(PyExc_ValueError, "fd %d is out of range", fd);
        return -1;
    }
    return 0;
}

static PyFd *py_fd(Loop *L, int fd) {
    Slot *s = slot_of(L, fd);
    if (s->kind == K_PY) return &L->py[s->aux];
    if (s->kind != K_FREE) { PyErr_Format(PyExc_ValueError, "fd %d is in use", fd); return NULL; }
    for (int i = 0; i < MAX_PY_FDS; i++) {
        if (L->py[i].fd < 0) {
            L->py[i].fd = fd;
            s->kind = K_PY;
            s->aux = (uint16_t)i;
            if (ep_set(L, EPOLL_CTL_ADD, fd, 0) < 0) {
                L->py[i].fd = -1;
                s->kind = K_FREE;
                return (PyFd *)PyErr_SetFromErrno(PyExc_OSError);
            }
            return &L->py[i];
        }
    }
    PyErr_SetString(PyExc_RuntimeError, "too many registered descriptors");
    return NULL;
}

static PyObject *py_fd_update(Loop *L, int fd, PyFd *h) {
    if (!h->reader && !h->writer) {
        epoll_ctl(L->ep, EPOLL_CTL_DEL, fd, NULL);
        h->fd = -1;
        Slot *s = slot_of(L, fd);
        uint32_t gen = s->gen + 1;
        memset(s, 0, sizeof *s);
        s->gen = gen;
    } else {
        uint32_t events = (h->reader ? EPOLLIN : 0) | (h->writer ? EPOLLOUT : 0);
        if (ep_set(L, EPOLL_CTL_MOD, fd, events) < 0) return PyErr_SetFromErrno(PyExc_OSError);
    }
    Py_RETURN_NONE;
}

static PyObject *set_handler(Loop *L, PyObject *args, int writer, int with_cb) {
    int fd;
    PyObject *cb = NULL;
    if (!PyArg_ParseTuple(args, with_cb ? "iO" : "i", &fd, &cb)) return NULL;
    if (check_fd(L, fd) < 0) return NULL;
    if (!with_cb && slot_of(L, fd)->kind != K_PY) Py_RETURN_NONE;
    PyFd *h = py_fd(L, fd);
    if (!h) return NULL;
    PyObject **place = writer ? &h->writer : &h->reader;
    Py_XSETREF(*place, Py_XNewRef(cb));
    return py_fd_update(L, fd, h);
}

static PyObject *Loop_add_reader(Loop *L, PyObject *args) { return set_handler(L, args, 0, 1); }
static PyObject *Loop_add_writer(Loop *L, PyObject *args) { return set_handler(L, args, 1, 1); }
static PyObject *Loop_remove_reader(Loop *L, PyObject *args) { return set_handler(L, args, 0, 0); }
static PyObject *Loop_remove_writer(Loop *L, PyObject *args) { return set_handler(L, args, 1, 0); }

static PyObject *Loop_call_soon(Loop *L, PyObject *cb) {
    if (PyList_Append(L->soon, cb) < 0) return NULL;
    Py_RETURN_NONE;
}

/* The GIL guards the list; the eventfd wakes a loop blocked in epoll_wait. */
static PyObject *Loop_call_soon_threadsafe(Loop *L, PyObject *cb) {
    if (PyList_Append(L->soon, cb) < 0) return NULL;
    uint64_t one = 1;
    if (write(L->wake_fd, &one, sizeof one) < 0) { /* already pending: it will wake anyway */ }
    Py_RETURN_NONE;
}

static PyObject *Loop_listen(Loop *L, PyObject *arg) {
    int fd = (int)PyLong_AsLong(arg);
    if (fd == -1 && PyErr_Occurred()) return NULL;
    if (check_fd(L, fd) < 0) return NULL;
    slot_of(L, fd)->kind = K_LISTENER;
    if (ep_set(L, EPOLL_CTL_ADD, fd, EPOLLIN) < 0) return PyErr_SetFromErrno(PyExc_OSError);
    Py_RETURN_NONE;
}

/* The Python path's answer to a request: head and body, already rendered. */
static PyObject *Loop_respond(Loop *L, PyObject *const *args, Py_ssize_t nargs) {
    if (nargs != 4) { PyErr_SetString(PyExc_TypeError, "respond(fd, gen, data, close)"); return NULL; }
    int fd = (int)PyLong_AsLong(args[0]);
    uint32_t gen = (uint32_t)PyLong_AsUnsignedLong(args[1]);
    if (PyErr_Occurred()) return NULL;
    char *data;
    Py_ssize_t n;
    if (PyBytes_AsStringAndSize(args[2], &data, &n) < 0) return NULL;
    int close_after = PyObject_IsTrue(args[3]);
    if (check_fd(L, fd) < 0) return NULL;
    Slot *s = slot_of(L, fd);
    if (s->kind != K_CLIENT || s->gen != gen) Py_RETURN_NONE;  /* the client went away */
    if (client_send(L, fd, data, (size_t)n, close_after) && !(s->flags & F_CLOSE_AFTER))
        client_finish(L, fd);
    Py_RETURN_NONE;
}

static int lane_attach(Loop *L, NativeLane *lane, PyObject *conn) {
    PGconn *pg = pq_pgconn(conn);
    if (!pg) return -1;
    int fd = PQsocket(pg);
    if (check_fd(L, fd) < 0) return -1;
    Slot *s = slot_of(L, fd);
    s->kind = K_LANE;
    s->aux = (uint16_t)(lane - L->lanes);
    if (ep_set(L, EPOLL_CTL_ADD, fd, EPOLLIN) < 0) {
        s->kind = K_FREE;
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    lane->conn = Py_NewRef(conn);
    lane->pg = pg;
    lane->fd = fd;
    lane->up = 1;
    lane->writing = 0;
    lane->batch = 0;
    lane->queued = 0;
    return 0;
}

static PyObject *Loop_add_lane(Loop *L, PyObject *conn) {
    /* Lanes and `used` are fixed once serving starts: only called while setting up. */
    NativeLane *lanes = realloc(L->lanes, (size_t)(L->nlanes + 1) * sizeof *lanes);
    if (!lanes) return PyErr_NoMemory();
    L->lanes = lanes;
    int *used = realloc(L->used, (size_t)(L->nlanes + 1) * sizeof *used);
    if (!used) return PyErr_NoMemory();
    L->used = used;
    NativeLane *lane = &L->lanes[L->nlanes];
    memset(lane, 0, sizeof *lane);
    L->nlanes++;
    if (lane_attach(L, lane, conn) < 0) { L->nlanes--; return NULL; }
    return PyLong_FromLong(L->nlanes - 1);
}

static PyObject *Loop_attach_lane(Loop *L, PyObject *args) {
    int index;
    PyObject *conn;
    if (!PyArg_ParseTuple(args, "iO", &index, &conn)) return NULL;
    if (index < 0 || index >= L->nlanes || L->lanes[index].up) {
        PyErr_SetString(PyExc_ValueError, "no such lane down");
        return NULL;
    }
    if (lane_attach(L, &L->lanes[index], conn) < 0) return NULL;
    Py_RETURN_NONE;
}

static PyObject *Loop_serve(Loop *L, PyObject *args, PyObject *kwds) {
    static char *names[] = {"bridge", "on_lane_down", "compressor", "gzip_min", "not_found", "internal_error",
                            "window_us", "light_pass", NULL};
    PyObject *bridge, *down, *capsule, *not_found, *internal;
    Py_ssize_t gzip_min;
    long window_us;
    int light_pass;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOOnSSli:serve", names, &bridge, &down, &capsule, &gzip_min,
                                     &not_found, &internal, &window_us, &light_pass))
        return NULL;
    if (capsule != Py_None) {
        L->compress = (compress_fn)PyCapsule_GetPointer(capsule, COMPRESS_CAPSULE);
        if (!L->compress) return NULL;
    }
    Py_XSETREF(L->bridge, Py_NewRef(bridge));
    Py_XSETREF(L->on_lane_down, Py_NewRef(down));
    Py_XSETREF(L->not_found, Py_NewRef(not_found));
    Py_XSETREF(L->internal, Py_NewRef(internal));
    L->gzip_min = (size_t)gzip_min;
    L->window_ns = window_us * 1000;
    L->light_pass = light_pass;
    size_t need = (size_t)(PyBytes_GET_SIZE(not_found) + PyBytes_GET_SIZE(internal)) + HEAD_ROOM + 32;
    if (need > L->json_cap) { PyErr_SetString(PyExc_ValueError, "error bodies too large"); return NULL; }
    Py_RETURN_NONE;
}

static PyObject *Loop_slot_size(PyObject *cls, PyObject *unused) { return PyLong_FromSize_t(sizeof(Slot)); }

static int Loop_init(Loop *L, PyObject *args, PyObject *kwds) {
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) < 0) { PyErr_SetFromErrno(PyExc_OSError); return -1; }
    if (lim.rlim_cur < lim.rlim_max) {  /* the soft limit up to the hard one, for this process only */
        lim.rlim_cur = lim.rlim_max;
        setrlimit(RLIMIT_NOFILE, &lim);
        getrlimit(RLIMIT_NOFILE, &lim);
    }
    L->nslots = lim.rlim_cur > MAX_TABLE ? MAX_TABLE : (size_t)lim.rlim_cur;
    L->slots = calloc(L->nslots, sizeof(Slot));  /* zero pages: touched only where descriptors exist */
    L->events = malloc(MAX_EVENTS * sizeof(struct epoll_event));
    L->rbuf = malloc(READ_BUF);
    L->json_cap = 128 * 1024;
    L->json = malloc(L->json_cap);
    L->gz_cap = 128 * 1024;
    L->gz = malloc(L->gz_cap);
    L->soon = PyList_New(0);
    if (!L->slots || !L->events || !L->rbuf || !L->json || !L->gz || !L->soon) {
        PyErr_NoMemory();
        return -1;
    }
    for (int i = 0; i < MAX_PY_FDS; i++) L->py[i].fd = -1;
    L->ep = epoll_create1(EPOLL_CLOEXEC);
    L->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (L->ep < 0 || L->wake_fd < 0 || check_fd(L, L->wake_fd) < 0) {
        if (!PyErr_Occurred()) PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    slot_of(L, L->wake_fd)->kind = K_WAKE;
    if (ep_set(L, EPOLL_CTL_ADD, L->wake_fd, EPOLLIN) < 0) { PyErr_SetFromErrno(PyExc_OSError); return -1; }
    return 0;
}

static PyMethodDef Loop_methods[] = {
    {"add_reader", (PyCFunction)Loop_add_reader, METH_VARARGS, NULL},
    {"add_writer", (PyCFunction)Loop_add_writer, METH_VARARGS, NULL},
    {"remove_reader", (PyCFunction)Loop_remove_reader, METH_VARARGS, NULL},
    {"remove_writer", (PyCFunction)Loop_remove_writer, METH_VARARGS, NULL},
    {"call_soon", (PyCFunction)Loop_call_soon, METH_O, "Run a callback at the end of this pass."},
    {"call_soon_threadsafe", (PyCFunction)Loop_call_soon_threadsafe, METH_O, NULL},
    {"listen", (PyCFunction)Loop_listen, METH_O, "Accept on this listening descriptor."},
    {"add_lane", (PyCFunction)Loop_add_lane, METH_O, "A ready pipeline-mode Conn for native reads -> its index."},
    {"attach_lane", (PyCFunction)Loop_attach_lane, METH_VARARGS, "A reconnected Conn for a lane that went down."},
    {"respond", (PyCFunction)(void (*)(void))Loop_respond, METH_FASTCALL, "Answer a bridged request."},
    {"serve", (PyCFunction)(void (*)(void))Loop_serve, METH_VARARGS | METH_KEYWORDS, "Configure the request path."},
    {"run", (PyCFunction)Loop_run, METH_NOARGS, "Run forever."},
    {"slot_size", (PyCFunction)Loop_slot_size, METH_NOARGS | METH_CLASS, "Bytes per connection in the table."},
    {NULL}
};

static PyTypeObject LoopType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_pq.Loop",
    .tp_basicsize = sizeof(Loop),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Loop_init,
    .tp_methods = Loop_methods,
};

/* The native timestamp formatter, exposed for tests. */
static PyObject *py_time_from_micros(PyObject *m, PyObject *arg) {
    long long us = PyLong_AsLongLong(arg);
    if (us == -1 && PyErr_Occurred()) return NULL;
    char out[32];
    char *end = fastloop_put_time(out, us);
    return PyUnicode_FromStringAndSize(out, end - out);
}

PyMethodDef fastloop_functions[] = {
    {"time_from_micros", py_time_from_micros, METH_O, "Binary timestamptz -> wire format; for tests."},
    {NULL}
};

int fastloop_register(PyObject *m) {
    if (PyType_Ready(&LoopType) < 0) return -1;
    return PyModule_AddObjectRef(m, "Loop", (PyObject *)&LoopType);
}
