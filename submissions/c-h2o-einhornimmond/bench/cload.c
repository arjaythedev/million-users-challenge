/*
 * cload: bench/mload's load generator in C. One epoll loop per thread, one keep-alive
 * connection per simulated user, and the user loop of mload (and of the $12 challenge's
 * load.js, which raoz's tests/loadgen.c followed and this started from):
 *
 *   sleep U(0,5)  ->  GET /feed  ->  think 3-7 s  ->  GET /posts/:id (one from the feed)
 *   ->  think 3-8 s  ->  like it (15%)  ->  write a post (2%)  ->  idle 5-15 s  ->  repeat
 *
 * Every think and idle time is multiplied by -think-scale. A user costs about 200 bytes while
 * it thinks and a response buffer while it waits, so one machine runs hundreds of thousands
 * of them; what runs out first is source ports -- about 28,000 per local address and target
 * port, so -src lists more local addresses and -ports more target ports.
 *
 * The result file has mload's schema, so `mload -merge` combines runs of either tool, and the
 * pass criteria are mload's: p95 < 500 ms, p99 < 1 s, failed requests < 1% (status >= 400,
 * no response in 60 s, or a connection that could not be made).
 *
 *   cload -target 127.0.0.1 -ports 80,81 -users 5000 -ramp 60s -hold 5m -gzip -think-scale 2.5 \
 *         -tokens seed/tokens.json -out run.json
 *
 * Needs zlib (to read a gzip'd feed for the post id). cc -O2 -o cload cload.c -lz -lm -lpthread
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

/* ---- configuration ----------------------------------------------------------------------- */

#define MAX_HOSTS 64
#define MAX_PORTS 1024
#define MAX_SRC 64
#define TOKENS_MAX 50000
#define REQ_MAX 2048
#define RB_INITIAL 8192
#define RB_MAX (1 << 20)
#define INFLATE_MAX (1 << 20)

static struct {
    char *host[MAX_HOSTS];
    struct sockaddr_in addr[MAX_HOSTS][MAX_PORTS];
    char *host_hdr[MAX_HOSTS];
    int nhosts;
    int port[MAX_PORTS];
    int nports;
    struct in_addr src[MAX_SRC];
    int nsrc;
    int users, offset, threads;
    double ramp, hold, timeout, think;
    int64_t start_ms;
    int gzip;
    const char *tokens_file, *out_file;
} cfg = {.nports = 0, .users = 1000, .ramp = 60, .hold = 300, .timeout = 60, .think = 1, .tokens_file = "tokens.json"};

static char *tokens[TOKENS_MAX], *usernames[TOKENS_MAX];
static int ntokens;

/* ---- statistics, mload's histogram -------------------------------------------------------- */

#define NB 2400
enum { EP_FEED, EP_POST, EP_LIKE, EP_CREATE, EP_N };
static const char *const ep_name[EP_N] = {"GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts"};
enum { ERR_TIMEOUT, ERR_EOF, ERR_REFUSED, ERR_RESET, ERR_STATUS, ERR_BAD, ERR_OTHER, ERR_N };
static const char *const err_name[ERR_N] = {"timeout", "EOF", "connection refused", "connection reset by peer", "status", "bad response", "other"};

typedef _Atomic int64_t ahist[NB];
static ahist all_hist, conn_hist, ep_hist[EP_N];
static _Atomic int64_t ep_ok[EP_N], ep_fail[EP_N], errors[ERR_N], http_status[600], dials, active, conns, fails_total;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int bucket(double seconds)
{
    const double us = seconds * 1e6;
    int b;

    if (us < 1)
        return 0;
    b = (int)(log(us) / log(1.01));
    return b >= NB ? NB - 1 : b;
}

static double bucket_ms(int b) { return pow(1.01, b + 0.5) / 1000; }

static double pct(const int64_t *h, double p)
{
    int64_t n = 0, want, acc = 0;

    for (int i = 0; i < NB; i++)
        n += h[i];
    if (n == 0)
        return 0;
    want = (int64_t)ceil(p * (double)n);
    for (int i = 0; i < NB; i++) {
        acc += h[i];
        if (acc >= want)
            return bucket_ms(i);
    }
    return 0;
}

static void snapshot(const ahist src, int64_t *dst)
{
    for (int i = 0; i < NB; i++)
        dst[i] = atomic_load_explicit(&src[i], memory_order_relaxed);
}

static void record(int ep, double latency, int ok, int err, int status)
{
    const int b = bucket(latency);

    atomic_fetch_add_explicit(&all_hist[b], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&ep_hist[ep][b], 1, memory_order_relaxed);
    if (ok) {
        atomic_fetch_add_explicit(&ep_ok[ep], 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&ep_fail[ep], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&fails_total, 1, memory_order_relaxed);
        if (err == ERR_STATUS && status >= 100 && status < 600)
            atomic_fetch_add_explicit(&http_status[status], 1, memory_order_relaxed);
        else
            atomic_fetch_add_explicit(&errors[err], 1, memory_order_relaxed);
    }
}

/* ---- users ------------------------------------------------------------------------------- */

enum { S_IDLE, S_CONNECTING, S_WAIT };

typedef struct vu {
    int32_t n;          /* user number: token, host and port follow from it */
    int fd;
    uint8_t state, action, retried, gz, chunked, close_after, started;
    int32_t iter, heapi;
    double next;        /* when the heap pops this user: next action, or the deadline */
    double sent;        /* when the current request went out, minus nothing yet */
    double connecting;  /* connect time inside the current request, not latency */
    int64_t post_id;
    uint32_t seed;
    char *rb;
    int rlen, rcap;
    int will_like, will_create;
} vu_t;

typedef struct thr {
    vu_t **heap;
    int n, cap;
    int ep;
    z_stream inflater;
    unsigned char *inflated;
    char req[REQ_MAX];
    pthread_t thread;
} thr_t;

static double t0;

static uint32_t rnd32(vu_t *v)
{
    uint32_t x = v->seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return v->seed = x;
}
static double rnd(vu_t *v) { return rnd32(v) / 4294967296.0; }
static double between(vu_t *v, double lo, double hi) { return cfg.think * (lo + rnd(v) * (hi - lo)); }

static inline int vu_host(const vu_t *v) { return v->n % cfg.nhosts; }
static inline int vu_port(const vu_t *v) { return (v->n / cfg.nhosts) % cfg.nports; }
static inline int vu_token(const vu_t *v) { return v->n % ntokens; }

/* -- the heap: every user is in it exactly once, keyed by `next` -- */

static void hswap(thr_t *t, int a, int b)
{
    vu_t *x = t->heap[a];
    t->heap[a] = t->heap[b];
    t->heap[b] = x;
    t->heap[a]->heapi = a;
    t->heap[b]->heapi = b;
}
static void hup(thr_t *t, int i)
{
    while (i > 0 && t->heap[(i - 1) / 2]->next > t->heap[i]->next) {
        hswap(t, i, (i - 1) / 2);
        i = (i - 1) / 2;
    }
}
static void hdown(thr_t *t, int i)
{
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < t->n && t->heap[l]->next < t->heap[m]->next) m = l;
        if (r < t->n && t->heap[r]->next < t->heap[m]->next) m = r;
        if (m == i) return;
        hswap(t, i, m);
        i = m;
    }
}
static void hpush(thr_t *t, vu_t *v)
{
    v->heapi = t->n;
    t->heap[t->n++] = v;
    hup(t, v->heapi);
}
static void hremove(thr_t *t, vu_t *v)
{
    int i = v->heapi;
    t->n--;
    if (i != t->n) {
        t->heap[i] = t->heap[t->n];
        t->heap[i]->heapi = i;
        hup(t, i);
        hdown(t, i);
    }
    v->heapi = -1;
}
static vu_t *hpop(thr_t *t)
{
    vu_t *v = t->heap[0];
    hremove(t, v);
    return v;
}

static void schedule(thr_t *t, vu_t *v, double at, int action)
{
    v->next = at;
    v->action = (uint8_t)action;
    v->state = S_IDLE;
    hpush(t, v);
}

/* -- connections -- */

static void conn_drop(thr_t *t, vu_t *v)
{
    if (v->fd >= 0) {
        epoll_ctl(t->ep, EPOLL_CTL_DEL, v->fd, NULL);
        close(v->fd);
        v->fd = -1;
        atomic_fetch_add_explicit(&conns, -1, memory_order_relaxed);
    }
    free(v->rb);
    v->rb = NULL;
    v->rlen = v->rcap = 0;
}

static void conn_open(thr_t *t, vu_t *v)
{
    int one = 1;
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = v};

    v->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (v->fd < 0) {
        v->state = S_CONNECTING; /* answered like a refused connect, below */
        v->next = now();
        hpush(t, v);
        return;
    }
    setsockopt(v->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (cfg.nsrc) {
        struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr = cfg.src[(v->n / (cfg.nhosts * cfg.nports)) % cfg.nsrc]};
        setsockopt(v->fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
        bind(v->fd, (struct sockaddr *)&sa, sizeof sa);
    }
    v->sent = now();
    atomic_fetch_add_explicit(&dials, 1, memory_order_relaxed);
    connect(v->fd, (struct sockaddr *)&cfg.addr[vu_host(v)][vu_port(v)], sizeof(struct sockaddr_in));
    epoll_ctl(t->ep, EPOLL_CTL_ADD, v->fd, &ev);
    atomic_fetch_add_explicit(&conns, 1, memory_order_relaxed);
    v->state = S_CONNECTING;
    v->next = v->sent + cfg.timeout;
    hpush(t, v);
}

/* -- requests -- */

static int build_request(thr_t *t, vu_t *v)
{
    const int ti = vu_token(v);
    const char *host = cfg.host_hdr[vu_host(v)];
    const char *enc = cfg.gzip ? "Accept-Encoding: gzip\r\n" : "";
    char body[512];
    int bl;

    switch (v->action) {
    case EP_FEED:
        return snprintf(t->req, REQ_MAX, "GET /feed HTTP/1.1\r\nHost: %s\r\nUser-Agent: cload\r\n%s\r\n", host, enc);
    case EP_POST:
        return snprintf(t->req, REQ_MAX, "GET /posts/%lld HTTP/1.1\r\nHost: %s\r\nUser-Agent: cload\r\n%s\r\n",
                        (long long)v->post_id, host, enc);
    case EP_LIKE:
        return snprintf(t->req, REQ_MAX,
                        "POST /posts/%lld/like HTTP/1.1\r\nHost: %s\r\nUser-Agent: cload\r\n%sAuthorization: Bearer %s\r\n"
                        "Content-Length: 0\r\n\r\n",
                        (long long)v->post_id, host, enc, tokens[ti]);
    default: {
        struct timespec ts;
        struct tm tm;
        char stamp[32];

        clock_gettime(CLOCK_REALTIME, &ts);
        gmtime_r(&ts.tv_sec, &tm);
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);
        bl = snprintf(body, sizeof body, "{\"body\":\"%s says hi at %s.%03ldZ (VU %d, iter %d)\"}", usernames[ti], stamp,
                      ts.tv_nsec / 1000000, v->n + 1, v->iter);
        return snprintf(t->req, REQ_MAX,
                        "POST /posts HTTP/1.1\r\nHost: %s\r\nUser-Agent: cload\r\n%sAuthorization: Bearer %s\r\n"
                        "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                        host, enc, tokens[ti], bl, body);
    }
    }
}

static void after_request(thr_t *t, vu_t *v, int status);

/* The request failed without a response. A GET on a keep-alive connection the server closed
 * is tried once more on a fresh connection, as an HTTP client would; anything else is a
 * failed request. */
static void request_failed(thr_t *t, vu_t *v, int err)
{
    const int get = v->action == EP_FEED || v->action == EP_POST;

    if (get && !v->retried && err != ERR_TIMEOUT && v->rlen == 0 && v->fd >= 0) {
        v->retried = 1;
        conn_drop(t, v);
        conn_open(t, v); /* sent is set again; the retry is what is timed, as in mload */
        return;
    }
    record(v->action, now() - v->sent - v->connecting, 0, err, 0);
    conn_drop(t, v);
    after_request(t, v, 0);
}

static void do_send(thr_t *t, vu_t *v)
{
    const int len = build_request(t, v);
    ssize_t w;

    v->rlen = 0;
    v->gz = v->chunked = v->close_after = 0;
    if (v->rb == NULL) {
        v->rb = malloc(RB_INITIAL);
        v->rcap = RB_INITIAL;
    }
    w = send(v->fd, t->req, (size_t)len, MSG_NOSIGNAL);
    v->state = S_WAIT;
    v->next = v->sent + cfg.timeout;
    hpush(t, v);
    if (w != len) {
        hremove(t, v);
        request_failed(t, v, errno == ECONNRESET ? ERR_RESET : errno == EPIPE ? ERR_EOF : ERR_OTHER);
    }
}

static void start_request(thr_t *t, vu_t *v)
{
    v->retried = 0;
    v->connecting = 0;
    if (v->fd < 0) {
        conn_open(t, v); /* do_send follows when the connect completes */
        return;
    }
    v->sent = now();
    do_send(t, v);
}

/* The user loop, as a reaction to the request that just ended. */
static void after_request(thr_t *t, vu_t *v, int status)
{
    const double tn = now();

    switch (v->action) {
    case EP_FEED:
        if (status != 200) {
            schedule(t, v, tn + between(v, 3, 7), EP_FEED);
            return;
        }
        schedule(t, v, tn + between(v, 3, 7), EP_POST);
        return;
    case EP_POST:
        /* mload draws both after the think: think, then like and create back to back. */
        v->next = tn + between(v, 3, 8);
        v->will_like = rnd(v) < 0.15;
        v->will_create = rnd(v) < 0.02;
        if (v->will_like) {
            schedule(t, v, v->next, EP_LIKE);
            return;
        }
        if (v->will_create) {
            schedule(t, v, v->next, EP_CREATE);
            return;
        }
        v->iter++;
        schedule(t, v, v->next + between(v, 5, 15), EP_FEED);
        return;
    case EP_LIKE:
        if (v->will_create) {
            schedule(t, v, tn, EP_CREATE);
            return;
        }
        v->iter++;
        schedule(t, v, tn + between(v, 5, 15), EP_FEED);
        return;
    default:
        v->iter++;
        schedule(t, v, tn + between(v, 5, 15), EP_FEED);
        return;
    }
}

/* -- responses -- */

static int64_t pick_post(thr_t *t, vu_t *v, const unsigned char *body, size_t len)
{
    int64_t ids[32];
    int n = 0;
    const unsigned char *p = body, *end;

    if (v->gz) {
        z_stream *z = &t->inflater;

        inflateReset(z);
        z->next_in = (unsigned char *)body;
        z->avail_in = (uInt)len;
        z->next_out = t->inflated;
        z->avail_out = INFLATE_MAX;
        if (inflate(z, Z_FINISH) != Z_STREAM_END)
            return 0;
        p = t->inflated;
        len = INFLATE_MAX - z->avail_out;
    }
    end = p + len;
    while (n < 32 && (p = memmem(p, (size_t)(end - p), "{\"id\":", 6)) != NULL) {
        int64_t id = 0;
        p += 6;
        while (p < end && *p >= '0' && *p <= '9')
            id = id * 10 + (*p++ - '0');
        ids[n++] = id;
    }
    return n == 0 ? 0 : ids[rnd32(v) % (uint32_t)n];
}

/* A complete chunked body in @p body of @p len: decoded in place, the decoded length is
 * returned, or -1 when the chunking is not whole yet or is broken. */
static ssize_t dechunk(char *body, size_t len)
{
    char *in = body, *out = body, *end = body + len;

    for (;;) {
        char *eol = memmem(in, (size_t)(end - in), "\r\n", 2);
        long n;

        if (eol == NULL)
            return -1;
        n = strtol(in, NULL, 16);
        if (n < 0)
            return -1;
        in = eol + 2;
        if (n == 0) {
            /* the trailer: an empty line, possibly after trailer headers */
            return memmem(in, (size_t)(end - in), "\r\n", 2) != NULL ? (ssize_t)(out - body) : -1;
        }
        if (end - in < n + 2)
            return -1;
        memmove(out, in, (size_t)n);
        out += n;
        in += n + 2;
    }
}

/* Reads what the socket has. Returns 1 when a complete response is in rb, 0 to wait, -1 when
 * the connection ended (errno says how). */
static int read_response(vu_t *v, size_t *body_off, size_t *body_len, int *status)
{
    for (;;) {
        ssize_t r;

        if (v->rb == NULL) {
            char scratch[256];
            r = recv(v->fd, scratch, sizeof scratch, 0);
        } else {
            if (v->rlen + 1 >= v->rcap) {
                if (v->rcap >= RB_MAX) {
                    errno = EMSGSIZE;
                    return -1;
                }
                v->rcap *= 4;
                v->rb = realloc(v->rb, (size_t)v->rcap);
            }
            r = recv(v->fd, v->rb + v->rlen, (size_t)(v->rcap - v->rlen - 1), 0);
        }
        if (r > 0) {
            if (v->rb != NULL)
                v->rlen += (int)r;
            continue;
        }
        if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            if (r == 0)
                errno = 0;
            /* A body without a length ends with the connection. */
            if (v->state == S_WAIT && v->close_after && *body_len == (size_t)-1 && v->rlen > 0) {
                *body_len = (size_t)v->rlen - *body_off;
                return 1;
            }
            return -1;
        }
        break;
    }
    if (v->state != S_WAIT)
        return 0;

    {
        char *hdr_end;
        size_t clen = (size_t)-1;

        v->rb[v->rlen] = '\0';
        if ((hdr_end = strstr(v->rb, "\r\n\r\n")) == NULL)
            return 0;
        *status = v->rlen >= 12 && memcmp(v->rb, "HTTP/1.", 7) == 0 ? atoi(v->rb + 9) : 0;
        if (*status == 0) {
            errno = EBADMSG;
            return -1;
        }
        for (char *line = strchr(v->rb, '\n'); line != NULL && line < hdr_end; line = strchr(line + 1, '\n')) {
            const char *h = line + 1;
            if (strncasecmp(h, "content-length:", 15) == 0) clen = (size_t)atol(h + 15);
            else if (strncasecmp(h, "transfer-encoding:", 18) == 0 && strcasestr(h, "chunked") != NULL && strcasestr(h, "chunked") < hdr_end) v->chunked = 1;
            else if (strncasecmp(h, "content-encoding:", 17) == 0 && strcasestr(h, "gzip") != NULL && strcasestr(h, "gzip") < hdr_end) v->gz = 1;
            else if (strncasecmp(h, "connection:", 11) == 0 && strcasestr(h, "close") != NULL && strcasestr(h, "close") < hdr_end) v->close_after = 1;
        }
        *body_off = (size_t)(hdr_end + 4 - v->rb);
        if (v->chunked) {
            ssize_t n = dechunk(v->rb + *body_off, (size_t)v->rlen - *body_off);
            if (n < 0)
                return 0;
            *body_len = (size_t)n;
            return 1;
        }
        if (clen != (size_t)-1) {
            if ((size_t)v->rlen < *body_off + clen)
                return 0;
            *body_len = clen;
            return 1;
        }
        /* No length: the body ends with the connection. */
        v->close_after = 1;
        *body_len = (size_t)-1;
        return 0;
    }
}

static void on_readable(thr_t *t, vu_t *v)
{
    size_t body_off = 0, body_len = (size_t)-1;
    int status = 0;
    const int r = read_response(v, &body_off, &body_len, &status);

    if (r == 0)
        return;
    if (v->state != S_WAIT) {
        /* An idle keep-alive connection the server closed: the next request dials again. */
        conn_drop(t, v);
        return;
    }
    hremove(t, v);
    if (r < 0) {
        request_failed(t, v, errno == 0 ? ERR_EOF : errno == ECONNRESET ? ERR_RESET : errno == EBADMSG ? ERR_BAD : ERR_OTHER);
        return;
    }
    {
        const double latency = now() - v->sent - v->connecting;
        int ok;

        switch (v->action) {
        case EP_FEED: ok = status == 200; break;
        case EP_POST: ok = status == 200; break;
        case EP_LIKE: ok = status == 200 || status == 201; break;
        default: ok = status == 201; break;
        }
        if (ok && v->action == EP_FEED)
            v->post_id = pick_post(t, v, (unsigned char *)v->rb + body_off, body_len);
        record(v->action, latency, ok, ERR_STATUS, status);
        if (v->close_after)
            conn_drop(t, v);
        else {
            free(v->rb);
            v->rb = NULL;
            v->rlen = v->rcap = 0;
        }
        after_request(t, v, status);
    }
}

static void on_connected(thr_t *t, vu_t *v, int err)
{
    hremove(t, v);
    {
        const double ct = now() - v->sent;
        atomic_fetch_add_explicit(&conn_hist[bucket(ct)], 1, memory_order_relaxed);
        v->connecting += ct;
    }
    if (err != 0) {
        conn_drop(t, v);
        /* A connect that failed is a failed request, which mload times from its start. */
        record(v->action, 0, 0, err == ECONNREFUSED ? ERR_REFUSED : err == ECONNRESET ? ERR_RESET : ERR_OTHER, 0);
        after_request(t, v, 0);
        return;
    }
    do_send(t, v);
}

static void *thread_main(void *arg)
{
    thr_t *t = arg;
    struct epoll_event events[1024];

    for (;;) {
        double tn = now();

        while (t->n > 0 && t->heap[0]->next <= tn) {
            vu_t *v = hpop(t);

            switch (v->state) {
            case S_IDLE:
                if (!v->started) {
                    v->started = 1;
                    atomic_fetch_add_explicit(&active, 1, memory_order_relaxed);
                }
                start_request(t, v);
                break;
            case S_CONNECTING: /* a connect that never completed */
            default:           /* a request that was never answered */
                record(v->action, cfg.timeout, 0, ERR_TIMEOUT, 0);
                conn_drop(t, v);
                after_request(t, v, 0);
                break;
            }
        }
        {
            int timeout_ms = t->n > 0 ? (int)ceil((t->heap[0]->next - now()) * 1000) : 100;
            int n;

            if (timeout_ms < 0) timeout_ms = 0;
            if (timeout_ms > 100) timeout_ms = 100;
            n = epoll_wait(t->ep, events, 1024, timeout_ms);
            for (int i = 0; i < n; i++) {
                vu_t *v = events[i].data.ptr;

                if (v->fd < 0)
                    continue;
                if (v->state == S_CONNECTING) {
                    int err = 0;
                    socklen_t len = sizeof err;

                    if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                        getsockopt(v->fd, SOL_SOCKET, SO_ERROR, &err, &len);
                        on_connected(t, v, err != 0 ? err : ECONNREFUSED);
                        continue;
                    }
                    if (events[i].events & EPOLLOUT) {
                        on_connected(t, v, 0);
                        if (v->state != S_WAIT || !(events[i].events & EPOLLIN))
                            continue;
                    }
                }
                if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP))
                    on_readable(t, v);
            }
        }
    }
    return NULL;
}

/* ---- setup and reporting ----------------------------------------------------------------- */

static int rss_mb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, resident = 0;

    if (f == NULL)
        return 0;
    if (fscanf(f, "%ld %ld", &pages, &resident) != 2)
        resident = 0;
    fclose(f);
    return (int)(resident * sysconf(_SC_PAGESIZE) >> 20);
}

static void load_tokens(void)
{
    FILE *f = fopen(cfg.tokens_file, "r");
    long n;
    char *js, *p;

    if (f == NULL) {
        perror(cfg.tokens_file);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    js = malloc((size_t)n + 1);
    if (fread(js, 1, (size_t)n, f) != (size_t)n) {
        perror(cfg.tokens_file);
        exit(2);
    }
    js[n] = '\0';
    fclose(f);
    for (p = js; ntokens < TOKENS_MAX && (p = strstr(p, "\"username\":\"")) != NULL;) {
        char *e;
        p += 12;
        e = strchr(p, '"');
        *e = '\0';
        usernames[ntokens] = p;
        p = strstr(e + 1, "\"token\":\"");
        if (p == NULL)
            break;
        p += 9;
        e = strchr(p, '"');
        *e = '\0';
        tokens[ntokens++] = p;
        p = e + 1;
    }
    if (ntokens == 0) {
        fprintf(stderr, "%s: no tokens\n", cfg.tokens_file);
        exit(2);
    }
}

static void parse_list(char *s, void (*add)(const char *))
{
    for (char *tok = strtok(s, ","); tok != NULL; tok = strtok(NULL, ","))
        add(tok);
}
static void add_host(const char *h) { if (cfg.nhosts < MAX_HOSTS) cfg.host[cfg.nhosts++] = strdup(h); }
static void add_port(const char *p)
{
    const char *dash = strchr(p, '-');
    int lo = atoi(p), hi = dash != NULL ? atoi(dash + 1) : lo;

    for (int port = lo; port <= hi && cfg.nports < MAX_PORTS; port++)
        cfg.port[cfg.nports++] = port;
}
static void add_src(const char *ip) { if (cfg.nsrc < MAX_SRC && inet_pton(AF_INET, ip, &cfg.src[cfg.nsrc]) == 1) cfg.nsrc++; }

static double parse_duration(const char *s)
{
    char *end;
    double v = strtod(s, &end);

    if (strcmp(end, "ms") == 0) return v / 1000;
    if (strcmp(end, "m") == 0) return v * 60;
    if (strcmp(end, "h") == 0) return v * 3600;
    return v; /* s, or bare seconds */
}

static void usage(void)
{
    fprintf(stderr,
            "cload -target ip[,ip..] [-ports 80[,81|lo-hi]] [-users N] [-offset N] [-ramp 60s] [-hold 5m]\n"
            "      [-think-scale X] [-gzip] [-timeout 60s] [-tokens tokens.json] [-out run.json]\n"
            "      [-threads N] [-src ip[,ip..]] [-start unix_ms]\n");
    exit(2);
}

static void write_hist_json(FILE *f, const int64_t *h)
{
    fputc('[', f);
    for (int i = 0; i < NB; i++)
        fprintf(f, "%s%lld", i ? "," : "", (long long)h[i]);
    fputc(']', f);
}

typedef struct {
    int t;
    int64_t users, conns, reqs, fails;
    double p95, p99;
    int rss;
} tick_t;

int main(int argc, char **argv)
{
    static int64_t hist[NB], chist[NB], ehist[EP_N][NB], prev[NB];
    tick_t *ticks = NULL;
    int nticks = 0;
    thr_t *threads;
    vu_t *users;
    struct rlimit rl;
    double end, last_tick;
    int64_t prev_fails = 0;

    cfg.threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *val = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (strcmp(a, name) == 0 && val != NULL && ++i)
        if (ARG("-target")) parse_list(strdup(val), add_host);
        else if (ARG("-ports")) parse_list(strdup(val), add_port);
        else if (ARG("-users")) cfg.users = atoi(val);
        else if (ARG("-offset")) cfg.offset = atoi(val);
        else if (ARG("-ramp")) cfg.ramp = parse_duration(val);
        else if (ARG("-hold")) cfg.hold = parse_duration(val);
        else if (ARG("-timeout")) cfg.timeout = parse_duration(val);
        else if (ARG("-think-scale")) cfg.think = atof(val);
        else if (ARG("-tokens")) cfg.tokens_file = val;
        else if (ARG("-out")) cfg.out_file = val;
        else if (ARG("-threads")) cfg.threads = atoi(val);
        else if (ARG("-src")) parse_list(strdup(val), add_src);
        else if (ARG("-start")) cfg.start_ms = atoll(val);
        else if (strcmp(a, "-gzip") == 0) cfg.gzip = 1;
        else usage();
#undef ARG
    }
    if (cfg.nhosts == 0 || cfg.users <= 0)
        usage();
    if (cfg.nports == 0)
        cfg.port[cfg.nports++] = 80;
    if (cfg.threads < 1)
        cfg.threads = 1;
    for (int h = 0; h < cfg.nhosts; h++) {
        struct addrinfo *ai, hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};

        if (getaddrinfo(cfg.host[h], NULL, &hints, &ai) != 0) {
            fprintf(stderr, "cannot resolve %s\n", cfg.host[h]);
            return 2;
        }
        for (int p = 0; p < cfg.nports; p++) {
            cfg.addr[h][p] = *(struct sockaddr_in *)ai->ai_addr;
            cfg.addr[h][p].sin_port = htons((uint16_t)cfg.port[p]);
        }
        freeaddrinfo(ai);
        cfg.host_hdr[h] = cfg.host[h];
    }
    load_tokens();
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    threads = calloc((size_t)cfg.threads, sizeof *threads);
    users = calloc((size_t)cfg.users, sizeof *users);
    for (int i = 0; i < cfg.threads; i++) {
        thr_t *t = &threads[i];
        t->cap = cfg.users / cfg.threads + 2;
        t->heap = calloc((size_t)t->cap, sizeof(vu_t *));
        t->ep = epoll_create1(EPOLL_CLOEXEC);
        t->inflated = malloc(INFLATE_MAX);
        inflateInit2(&t->inflater, 31);
    }

    if (cfg.start_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        double wait = (double)cfg.start_ms / 1000 - ((double)ts.tv_sec + (double)ts.tv_nsec * 1e-9);
        if (wait > 0)
            usleep((useconds_t)(wait * 1e6));
    }
    t0 = now();
    for (int i = 0; i < cfg.users; i++) {
        vu_t *v = &users[i];
        thr_t *t = &threads[i % cfg.threads];

        v->n = cfg.offset + i;
        v->fd = -1;
        v->heapi = -1;
        v->seed = (uint32_t)v->n * 2654435761u + 0x9e3779b9u;
        v->action = EP_FEED;
        /* mload starts user i at i/users of the ramp, and a user sleeps U(0,5) first. */
        v->next = t0 + cfg.ramp * i / cfg.users + between(v, 0, 5);
        v->state = S_IDLE;
        hpush(t, v);
    }
    for (int i = 0; i < cfg.threads; i++)
        pthread_create(&threads[i].thread, NULL, thread_main, &threads[i]);
    fprintf(stderr, "cload: %d users over %d host%s x %d port%s, %d threads, %d tokens, ramp %.0fs, hold %.0fs, think x%.2f%s\n",
            cfg.users, cfg.nhosts, cfg.nhosts == 1 ? "" : "s", cfg.nports, cfg.nports == 1 ? "" : "s", cfg.threads,
            ntokens, cfg.ramp, cfg.hold, cfg.think, cfg.gzip ? ", gzip" : "");

    end = t0 + cfg.ramp + cfg.hold;
    last_tick = t0;
    while (now() < end) {
        double tn;
        int64_t reqs = 0, fails;
        static int64_t win[NB];
        tick_t tick;

        usleep(100000);
        tn = now();
        if (tn - last_tick < 5)
            continue;
        last_tick += 5;
        snapshot(all_hist, hist);
        for (int i = 0; i < NB; i++) {
            win[i] = hist[i] - prev[i];
            prev[i] = hist[i];
            reqs += win[i];
        }
        fails = atomic_load(&fails_total);
        tick = (tick_t){.t = (int)(tn - t0), .users = atomic_load(&active), .conns = atomic_load(&conns), .reqs = reqs,
                        .fails = fails - prev_fails, .p95 = pct(win, .95), .p99 = pct(win, .99), .rss = rss_mb()};
        prev_fails = fails;
        ticks = realloc(ticks, (size_t)(nticks + 1) * sizeof *ticks);
        ticks[nticks++] = tick;
        printf("t=%4ds users=%lld conns=%lld req/s=%.0f fails=%lld p95=%.1fms p99=%.1fms rss=%dMB\n", tick.t,
               (long long)tick.users, (long long)tick.conns, (double)reqs / 5, (long long)tick.fails, tick.p95, tick.p99, tick.rss);
        fflush(stdout);
    }

    /* The result, from a snapshot: the users keep going until the process ends. */
    {
        const double duration = now() - t0;
        int64_t reqs = 0, fails = 0, ok_ep[EP_N], fail_ep[EP_N], err[ERR_N], ndials;
        static int64_t http[600];
        double p50, p95, p99, max_ms = 0, err_pct, cp95, cp99;
        int pass;

        snapshot(all_hist, hist);
        snapshot(conn_hist, chist);
        for (int e = 0; e < EP_N; e++) {
            snapshot(ep_hist[e], ehist[e]);
            ok_ep[e] = atomic_load(&ep_ok[e]);
            fail_ep[e] = atomic_load(&ep_fail[e]);
            reqs += ok_ep[e] + fail_ep[e];
            fails += fail_ep[e];
        }
        for (int e = 0; e < ERR_N; e++)
            err[e] = atomic_load(&errors[e]);
        for (int c = 0; c < 600; c++)
            http[c] = atomic_load(&http_status[c]);
        ndials = atomic_load(&dials);
        p50 = pct(hist, .5);
        p95 = pct(hist, .95);
        p99 = pct(hist, .99);
        cp95 = pct(chist, .95);
        cp99 = pct(chist, .99);
        for (int i = NB - 1; i >= 0; i--)
            if (hist[i] > 0) {
                max_ms = bucket_ms(i);
                break;
            }
        err_pct = reqs > 0 ? 100.0 * (double)fails / (double)reqs : 0;
        pass = reqs > 0 && p95 < 500 && p99 < 1000 && err_pct < 1;

        if (cfg.out_file != NULL) {
            FILE *f = fopen(cfg.out_file, "w");

            if (f == NULL) {
                perror(cfg.out_file);
            } else {
                fprintf(f, "{\"users\":%d,\"duration_s\":%.3f,\"reqs\":%lld,\"fails\":%lld,\"req_per_s\":%.3f,\"err_pct\":%.6f,"
                           "\"p50_ms\":%.4f,\"p95_ms\":%.4f,\"p99_ms\":%.4f,\"max_ms\":%.4f,\"pass\":%s,\"endpoints\":[",
                        cfg.users, duration, (long long)reqs, (long long)fails, (double)reqs / duration, err_pct, p50, p95, p99,
                        max_ms, pass ? "true" : "false");
                for (int e = 0; e < EP_N; e++) {
                    fprintf(f, "%s{\"name\":\"%s\",\"ok\":%lld,\"fail\":%lld,\"p95_ms\":%.4f,\"p99_ms\":%.4f,\"hist\":", e ? "," : "",
                            ep_name[e], (long long)ok_ep[e], (long long)fail_ep[e], pct(ehist[e], .95), pct(ehist[e], .99));
                    write_hist_json(f, ehist[e]);
                    fputc('}', f);
                }
                fprintf(f, "],\"errors\":{");
                {
                    int first = 1;
                    for (int e = 0; e < ERR_N; e++)
                        if (err[e] > 0) {
                            fprintf(f, "%s\"%s\":%lld", first ? "" : ",", err_name[e], (long long)err[e]);
                            first = 0;
                        }
                    for (int c = 0; c < 600; c++)
                        if (http[c] > 0) {
                            fprintf(f, "%s\"http %d\":%lld", first ? "" : ",", c, (long long)http[c]);
                            first = 0;
                        }
                }
                fprintf(f, "},\"hist\":");
                write_hist_json(f, hist);
                fprintf(f, ",\"dials\":%lld,\"connect_p95_ms\":%.4f,\"connect_p99_ms\":%.4f,\"connect_hist\":", (long long)ndials, cp95, cp99);
                write_hist_json(f, chist);
                fprintf(f, ",\"timeline\":[");
                for (int i = 0; i < nticks; i++)
                    fprintf(f, "%s{\"t\":%d,\"users\":%lld,\"conns\":%lld,\"reqs\":%lld,\"fails\":%lld,\"p95_ms\":%.4f,\"p99_ms\":%.4f,\"rss_mb\":%d}",
                            i ? "," : "", ticks[i].t, (long long)ticks[i].users, (long long)ticks[i].conns, (long long)ticks[i].reqs,
                            (long long)ticks[i].fails, ticks[i].p95, ticks[i].p99, ticks[i].rss);
                fprintf(f, "]}\n");
                fclose(f);
            }
        }
        printf("RESULT users=%d req/s=%.0f p50=%.1fms p95=%.1fms p99=%.1fms max=%.0fms err=%.3f%% pass=%s errors=map[", cfg.users,
               (double)reqs / duration, p50, p95, p99, max_ms, err_pct, pass ? "true" : "false");
        {
            int first = 1;
            for (int e = 0; e < ERR_N; e++)
                if (err[e] > 0) {
                    printf("%s%s:%lld", first ? "" : " ", err_name[e], (long long)err[e]);
                    first = 0;
                }
            for (int c = 0; c < 600; c++)
                if (http[c] > 0) {
                    printf("%shttp %d:%lld", first ? "" : " ", c, (long long)http[c]);
                    first = 0;
                }
        }
        printf("]\n  %-22s dials=%lld p95=%.1fms p99=%.1fms (not in request latency, as in k6)\n", "TCP connect", (long long)ndials, cp95, cp99);
        for (int e = 0; e < EP_N; e++)
            printf("  %-22s ok=%lld fail=%lld p95=%.1fms p99=%.1fms\n", ep_name[e], (long long)ok_ep[e], (long long)fail_ep[e],
                   pct(ehist[e], .95), pct(ehist[e], .99));
    }
    fflush(stdout);
    _exit(0);
}
