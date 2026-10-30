#include "loop.h"
#include "http.h"
#include "json.h"
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define EV(tag, fd) ((uint64_t)(tag) << 32 | (uint32_t)(fd))

static void ep_ctl(loop_t *l, int op, int fd, uint32_t events, int tag) {
    struct epoll_event ev = {.events = events, .data.u64 = EV(tag, fd)};
    if (epoll_ctl(l->ep, op, fd, &ev) && op == EPOLL_CTL_ADD) { perror("epoll_ctl"); exit(1); }
}
void loop_epoll_mod_pg(struct loop *l, int fd, bool want_out) {
    if (l->uring) { if (want_out) { pgconn_t *p = NULL; for (int i = 0; i < l->nr; i++) if (l->pgr[i].fd == fd) p = &l->pgr[i]; for (int i = 0; i < l->nw; i++) if (l->pgw[i].fd == fd) p = &l->pgw[i]; if (p) uring_pg_poll(l, p, true); } return; }
    ep_ctl(l, EPOLL_CTL_MOD, fd, EPOLLIN | (want_out ? EPOLLOUT : 0), TAG_PG);
}

static int64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

void loop_init(loop_t *l, int id, int cpu, const int *lfds, int nl, const uint8_t *sample, size_t sample_len) {
    memset(l, 0, sizeof *l);
    l->id = id; l->cpu = cpu;
    l->ep = epoll_create1(EPOLL_CLOEXEC);
    if (l->ep < 0) { perror("epoll_create1"); exit(1); }
    for (int i = 0; i < nl; i++) { l->lfd[i] = lfds[i]; ep_ctl(l, EPOLL_CTL_ADD, lfds[i], EPOLLIN, TAG_LISTENER); }
    l->nl = nl;
    l->nconn_cap = 1024;
    l->conns = calloc(l->nconn_cap, sizeof *l->conns);
    l->scratch = malloc(SCRATCH_SIZE);
    buf_reserve(&l->out, 16384); buf_reserve(&l->body_tmp, 16384);
    verifier_init(&l->ver, cfg.secret, cfg.secret_len);
    deflater_init(&l->gz, sample, sample_len);
    l->nr = cfg.db_read / cfg.loops; if (l->nr < 1) l->nr = 1;
    l->nw = cfg.db_write / cfg.loops; if (l->nw < 1) l->nw = 1;
    l->pgr = calloc((size_t)l->nr, sizeof *l->pgr); l->pgw = calloc((size_t)l->nw, sizeof *l->pgw);
    for (int i = 0; i < l->nr; i++) pg_init(&l->pgr[i], l, false);
    for (int i = 0; i < l->nw; i++) pg_init(&l->pgw[i], l, true);
}

static void watch_pg(loop_t *l, pgconn_t *p) { if (l->uring) uring_pg_poll(l, p, false); else ep_ctl(l, EPOLL_CTL_ADD, p->fd, EPOLLIN, TAG_PG); }

void loop_pg_connect_all(loop_t *l) {
    for (int i = 0; i < l->nr + l->nw; i++) {
        pgconn_t *p = i < l->nr ? &l->pgr[i] : &l->pgw[i - l->nr];
        if (p->alive) continue;
        if (pg_open(p)) { fprintf(stderr, "postgres: connect failed (loop %d)\n", l->id); p->next_retry = now_sec() + 1; continue; }
        watch_pg(l, p);
    }
}

static pgconn_t *pg_by_fd(loop_t *l, int fd) {
    for (int i = 0; i < l->nr; i++) if (l->pgr[i].fd == fd) return &l->pgr[i];
    for (int i = 0; i < l->nw; i++) if (l->pgw[i].fd == fd) return &l->pgw[i];
    return NULL;
}

static pgconn_t *least_loaded(pgconn_t *ps, int n) {
    pgconn_t *best = NULL;
    for (int i = 0; i < n; i++) if (ps[i].alive && (!best || ps[i].inflight < best->inflight)) best = &ps[i];
    return best;
}

// --- connections ---
conn_t *conn_at(loop_t *l, int fd) {
    if ((size_t)fd >= l->nconn_cap) {
        size_t cap = l->nconn_cap * 2; while (cap <= (size_t)fd) cap *= 2;
        l->conns = realloc(l->conns, cap * sizeof *l->conns);
        memset(l->conns + l->nconn_cap, 0, (cap - l->nconn_cap) * sizeof *l->conns);
        l->nconn_cap = cap;
    }
    return &l->conns[fd];
}

void loop_conn_reset(loop_t *l, int fd) {
    conn_t *c = &l->conns[fd];
    c->gen++; c->open = c->busy = c->close_after = c->closing = c->pending = c->recv_armed = 0;
    free(c->inbuf); free(c->wbuf);
    c->inbuf = c->wbuf = NULL; c->inlen = c->incap = c->wlen = c->wcap = 0;
    l->nconn--;
    atomic_fetch_add_explicit(&l->st.closes, 1, memory_order_relaxed);
}

static void close_conn(loop_t *l, int fd) {
    conn_t *c = &l->conns[fd];
    if (!c->open) return;
    if (l->uring) { uring_close(l, fd); return; }
    close(fd);
    c->gen++; c->open = c->busy = c->close_after = 0;
    free(c->inbuf); free(c->wbuf);
    c->inbuf = c->wbuf = NULL; c->inlen = c->incap = c->wlen = c->wcap = 0;
    l->nconn--;
    atomic_fetch_add_explicit(&l->st.closes, 1, memory_order_relaxed);
}

static void keep(uint8_t **buf, uint32_t *len, uint32_t *cap, const uint8_t *data, size_t n, bool append) {
    size_t need = (append ? *len : 0) + n;
    if (need > *cap) { *cap = (uint32_t)(need < 4096 ? 4096 : need); *buf = realloc(*buf, *cap); }
    memcpy(*buf + (append ? *len : 0), data, n);
    *len = (uint32_t)need;
}

static void send_bytes(loop_t *l, int fd, conn_t *c, const uint8_t *b, size_t n) {
    if (l->uring) { uring_send(l, fd, c, b, n); return; }
    if (c->wlen) { keep(&c->wbuf, &c->wlen, &c->wcap, b, n, true); return; }
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { keep(&c->wbuf, &c->wlen, &c->wcap, b, n, false); ep_ctl(l, EPOLL_CTL_MOD, fd, EPOLLIN | EPOLLOUT, TAG_CLIENT); return; }
            close_conn(l, fd); return;
        }
        b += w; n -= (size_t)w;
    }
}

static void process(loop_t *l, int fd, conn_t *c, uint8_t *data, size_t n);

static void resume(loop_t *l, int fd, conn_t *c) {
    if (!c->inlen || c->busy || c->wlen || c->pending || c->closing) return;
    size_t n = c->inlen;
    memcpy(l->scratch, c->inbuf, n);
    c->inlen = 0;
    process(l, fd, c, l->scratch, n);
}

static void after_response(loop_t *l, int fd, conn_t *c) {
    if (!c->open) return;
    if (c->close_after) { if (!c->wlen && !c->pending) close_conn(l, fd); return; }
    resume(l, fd, c);
}

static void respond(loop_t *l, int fd, conn_t *c, int status, const char *body, bool close_it) {
    buf_reset(&l->out);
    append_response(&l->out, status, (const uint8_t *)body, strlen(body), false, close_it);
    c->close_after = close_it;
    send_bytes(l, fd, c, l->out.p, l->out.len);
    if (c->open && c->close_after && !c->wlen && !c->pending) close_conn(l, fd);
}

void job_free(job_t *j) { free(j->username); free(j->body); j->username = j->body = NULL; }

void loop_finish_job(struct loop *l, job_t *j, int status, const uint8_t *body, size_t n) {
    int fd = j->fd;
    conn_t *c = &l->conns[fd];
    if (!c->open || c->gen != j->gen) return;
    bool gz = false;
    if (j->gzip && status < 300 && n >= (size_t)cfg.gzip_min) { body = deflate_gzip(&l->gz, body, n, &n); gz = true; }
    buf_reset(&l->out);
    append_response(&l->out, status, body, n, gz, j->close);
    c->busy = 0;
    send_bytes(l, fd, c, l->out.p, l->out.len);
    after_response(l, fd, c);
}

static void dispatch(loop_t *l, int fd, conn_t *c, const request_t *r, job_t *j, bool write) {
    j->fd = fd; j->gen = c->gen; j->gzip = r->gzip; j->close = r->close;
    c->busy = 1; c->close_after = r->close;
    atomic_fetch_add_explicit(&l->st.dbreqs, 1, memory_order_relaxed);
    pgconn_t *p;
    if (write) p = least_loaded(l->pgw, l->nw);
    else { if (!l->iter_read || !l->iter_read->alive) l->iter_read = least_loaded(l->pgr, l->nr); p = l->iter_read; }
    if (!p) {
        if (j->kind == K_HEALTH) { buf_reset(&l->body_tmp); append_health_degraded(&l->body_tmp, "no database connection"); loop_finish_job(l, j, 503, l->body_tmp.p, l->body_tmp.len); }
        else loop_finish_job(l, j, 500, (const uint8_t *)BODY_INTERNAL, sizeof BODY_INTERNAL - 1);
        job_free(j);
        return;
    }
    pg_enqueue(p, j);
}

static bool path_is(const request_t *r, const char *s) { size_t n = strlen(s); return r->path_len == n && !memcmp(r->path, s, n); }

static void respond_auth(loop_t *l, int fd, conn_t *c, int code, bool close_it) {
    respond(l, fd, c, 401, code == AUTH_MISSING ? BODY_MISSING_TOKEN : code == AUTH_INVALID ? BODY_INVALID_TOKEN : BODY_BAD_PAYLOAD, close_it);
}

static void handle(loop_t *l, int fd, conn_t *c, const request_t *r) {
    atomic_fetch_add_explicit(&l->st.reqs, 1, memory_order_relaxed);
    job_t j = {0};
    if (r->method == M_GET) {
        if (path_is(r, "/feed")) { j.kind = K_FEED; dispatch(l, fd, c, r, &j, false); return; }
        if (r->path_len > 7 && !memcmp(r->path, "/posts/", 7)) {
            if (!parse_id(r->path + 7, r->path_len - 7, &j.id)) { respond(l, fd, c, 400, BODY_INVALID_ID, r->close); return; }
            j.kind = K_POST; dispatch(l, fd, c, r, &j, false); return;
        }
        if (path_is(r, "/health")) { j.kind = K_HEALTH; dispatch(l, fd, c, r, &j, false); return; }
    } else if (r->method == M_POST) {
        if (path_is(r, "/posts")) {
            const uint8_t *name; size_t nl;
            int code = jwt_verify(&l->ver, r->auth, r->auth_len, now_sec(), &j.user, &name, &nl);
            if (code != AUTH_OK) { respond_auth(l, fd, c, code, r->close); return; }
            const uint8_t *trimmed; size_t tl; const char *msg;
            buf_reserve(&l->body_tmp, r->body_len + 16);
            int st = validate_body(r->body, r->body_len, l->body_tmp.p, l->body_tmp.cap, &trimmed, &tl, &msg);
            if (st) { respond(l, fd, c, st, msg, r->close); return; }
            j.kind = K_CREATE;
            j.username = malloc(nl); memcpy(j.username, name, nl); j.ulen = (uint32_t)nl;
            j.body = malloc(tl); memcpy(j.body, trimmed, tl); j.blen = (uint32_t)tl;
            dispatch(l, fd, c, r, &j, true); return;
        }
        if (r->path_len > 12 && !memcmp(r->path, "/posts/", 7) && !memcmp(r->path + r->path_len - 5, "/like", 5)) {
            const uint8_t *name; size_t nl;
            int code = jwt_verify(&l->ver, r->auth, r->auth_len, now_sec(), &j.user, &name, &nl);
            if (code != AUTH_OK) { respond_auth(l, fd, c, code, r->close); return; }
            if (!parse_id(r->path + 7, r->path_len - 12, &j.id)) { respond(l, fd, c, 400, BODY_INVALID_ID, r->close); return; }
            j.kind = K_LIKE; dispatch(l, fd, c, r, &j, true); return;
        }
    }
    respond(l, fd, c, 404, BODY_NOT_FOUND, r->close);
}

static void process(loop_t *l, int fd, conn_t *c, uint8_t *data, size_t n) {
    request_t r;
    while (n && c->open && !c->busy && !c->wlen && !c->close_after && !c->pending && !c->closing) {
        size_t used;
        int st = parse_request(data, n, &r, &used);
        if (st == P_INCOMPLETE) break;
        if (st == P_BAD) { respond(l, fd, c, 400, BODY_BAD_REQUEST, true); close_conn(l, fd); return; }
        if (st == P_TOO_LARGE) { respond(l, fd, c, 413, BODY_TOO_LARGE, true); close_conn(l, fd); return; }
        data += used; n -= used;
        handle(l, fd, c, &r);
    }
    if (!c->open) return;
    if (n) {
        if (n > MAX_INBUF) { close_conn(l, fd); return; }
        keep(&c->inbuf, &c->inlen, &c->incap, data, n, false);
    } else if (c->inbuf) { free(c->inbuf); c->inbuf = NULL; c->inlen = c->incap = 0; }
}

// loop_ingest handles n bytes that arrived from a client (data may be a kernel-provided buffer).
void loop_ingest(loop_t *l, int fd, conn_t *c, const uint8_t *data, size_t n) {
    if (c->busy || c->wlen || c->close_after || c->pending || c->closing) {
        if (c->inlen + n > MAX_INBUF) { close_conn(l, fd); return; }
        keep(&c->inbuf, &c->inlen, &c->incap, data, n, true);
        return;
    }
    uint8_t *buf = l->scratch;
    size_t pre = c->inlen;
    if (pre + n > SCRATCH_SIZE) { close_conn(l, fd); return; }
    memcpy(buf, c->inbuf, pre);
    memcpy(buf + pre, data, n);
    c->inlen = 0;
    process(l, fd, c, buf, pre + n);
}

static void on_readable(loop_t *l, int fd) {
    conn_t *c = &l->conns[fd];
    uint8_t *buf = l->scratch;
    size_t pre = c->inlen;
    memcpy(buf, c->inbuf, pre);
    ssize_t n;
    for (;;) { n = read(fd, buf + pre, SCRATCH_SIZE - pre); if (n >= 0 || errno != EINTR) break; }
    if (n < 0) { if (errno == EAGAIN) return; close_conn(l, fd); return; }
    if (n == 0) { close_conn(l, fd); return; }
    if (c->busy || c->wlen || c->close_after) {
        if (c->inlen + (size_t)n > MAX_INBUF) { close_conn(l, fd); return; }
        keep(&c->inbuf, &c->inlen, &c->incap, buf + pre, (size_t)n, true);
        return;
    }
    c->inlen = 0;
    process(l, fd, c, buf, pre + (size_t)n);
}

static void on_writable(loop_t *l, int fd) {
    conn_t *c = &l->conns[fd];
    if (!c->open) return;
    uint8_t *b = c->wbuf; size_t n = c->wlen;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { memmove(c->wbuf, b, n); c->wlen = (uint32_t)n; return; }
            close_conn(l, fd); return;
        }
        b += w; n -= (size_t)w;
    }
    c->wlen = 0;
    ep_ctl(l, EPOLL_CTL_MOD, fd, EPOLLIN, TAG_CLIENT);
    if (c->close_after) { close_conn(l, fd); return; }
    resume(l, fd, c);
}

static void pause_listeners(loop_t *l) {
    if (l->paused) return;
    l->paused = true; l->pause_until_ms = now_ms() + 100;
    for (int i = 0; i < l->nl; i++) epoll_ctl(l->ep, EPOLL_CTL_DEL, l->lfd[i], NULL);
}
static void resume_listeners(loop_t *l) {
    l->paused = false;
    for (int i = 0; i < l->nl; i++) ep_ctl(l, EPOLL_CTL_ADD, l->lfd[i], EPOLLIN, TAG_LISTENER);
}

static void on_accept(loop_t *l, int lfd) {
    for (int i = 0; i < 256; i++) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN) return;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) { pause_listeners(l); return; }
            perror("accept4"); return;
        }
        conn_t *c = conn_at(l, fd);
        c->open = 1;
        l->nconn++;
        atomic_fetch_add_explicit(&l->st.accepts, 1, memory_order_relaxed);
        ep_ctl(l, EPOLL_CTL_ADD, fd, EPOLLIN, TAG_CLIENT);
        on_readable(l, fd); // TCP_DEFER_ACCEPT: the first request is usually already here
    }
}

void loop_flush_jobs(loop_t *l) {
    for (int i = 0; i < l->nr; i++) if (l->pgr[i].dirty) { atomic_fetch_add_explicit(&l->st.batches, 1, memory_order_relaxed); pg_flush(&l->pgr[i]); }
    for (int i = 0; i < l->nw; i++) if (l->pgw[i].dirty) { atomic_fetch_add_explicit(&l->st.batches, 1, memory_order_relaxed); pg_flush(&l->pgw[i]); }
    l->iter_read = NULL;
}

void loop_reconnect_dead(loop_t *l) {
    int64_t now = now_sec();
    for (int i = 0; i < l->nr + l->nw; i++) {
        pgconn_t *p = i < l->nr ? &l->pgr[i] : &l->pgw[i - l->nr];
        if (p->alive || p->next_retry > now) continue;
        if (pg_open(p)) { p->next_retry = now + 1; continue; }
        watch_pg(l, p);
        fprintf(stderr, "postgres: reconnected (loop %d)\n", l->id);
    }
}

void loop_pin(loop_t *l) {
    if (cfg.pin && l->cpu >= 0) {
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(l->cpu, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
}

void *loop_run(void *arg) {
    loop_t *l = arg;
    loop_pin(l);
    if (cfg.uring && uring_init(l)) { l->uring = true; loop_pg_connect_all(l); uring_run(l); return NULL; }
    if (cfg.uring) fprintf(stderr, "loop %d: io_uring unavailable, using epoll\n", l->id);
    loop_pg_connect_all(l);
    struct epoll_event evs[512];
    struct timespec pause = {0, (long)cfg.batch_micros * 1000};
    for (;;) {
        int timeout = 1000;
        if (l->paused) { int64_t d = l->pause_until_ms - now_ms(); if (d <= 0) resume_listeners(l); else timeout = (int)d + 1; }
        int n = epoll_wait(l->ep, evs, 512, timeout);
        if (n < 0) { if (errno == EINTR) continue; perror("epoll_wait"); exit(1); }
        atomic_fetch_add_explicit(&l->st.wakeups, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&l->st.events, (uint64_t)n, memory_order_relaxed);
        for (int i = 0; i < n; i++) {
            int tag = (int)(evs[i].data.u64 >> 32), fd = (int)(uint32_t)evs[i].data.u64;
            uint32_t e = evs[i].events;
            if (tag == TAG_LISTENER) { on_accept(l, fd); continue; }
            if (tag == TAG_PG) {
                pgconn_t *p = pg_by_fd(l, fd);
                if (!p || !p->alive) continue;
                if (e & (EPOLLERR | EPOLLHUP)) { pg_die(p, "hup"); continue; }
                if (e & EPOLLOUT) pg_on_writable(p);
                if ((e & EPOLLIN) && p->alive) pg_on_readable(p);
                continue;
            }
            if (!l->conns[fd].open) continue;
            if (e & (EPOLLERR | EPOLLHUP)) { close_conn(l, fd); continue; }
            if (e & EPOLLOUT) on_writable(l, fd);
            if ((e & EPOLLIN) && l->conns[fd].open) on_readable(l, fd);
        }
        loop_flush_jobs(l);
        loop_reconnect_dead(l);
        if (cfg.batch_micros > 0 && n < cfg.batch_min) { atomic_fetch_add_explicit(&l->st.sleeps, 1, memory_order_relaxed); nanosleep(&pause, NULL); }
    }
    return NULL;
}
