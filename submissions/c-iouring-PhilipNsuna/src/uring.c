// io_uring backend (URING=1): one ring per loop; multishot accept and recv (provided buffers), SEND for
// responses, multishot POLL on the Postgres sockets, one io_uring_enter per iteration. See README.md.
#include "loop.h"
#include <errno.h>
#include <liburing.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { T_ACCEPT = 1, T_RECV, T_SEND, T_PGPOLL, T_PGOUT, T_CANCEL, T_PROVIDE, T_PGSEND };
#define BGID 1
#define NBUFS 1024
#define BUFSZ 8192
#define SBUF_SZ 8192
#define NSBUF 1024

typedef struct { uint8_t *data; size_t len, off; int fd; uint32_t gen; int next; } sbuf_t;
typedef struct {
    struct io_uring ring;
    uint8_t *bufs;          // NBUFS x BUFSZ provided to the kernel
    sbuf_t sb[NSBUF]; int sb_free;
} uring_t;

// user_data: 4 bits of type, 28 bits of generation or buffer index, 32 bits of fd
#define UD_MASK 0x0FFFFFFFu
static uint64_t ud(int type, int fd, uint32_t x) { return (uint64_t)type << 60 | (uint64_t)(x & UD_MASK) << 32 | (uint32_t)fd; }

static struct io_uring_sqe *sqe(loop_t *l) {
    uring_t *u = l->ring;
    struct io_uring_sqe *s = io_uring_get_sqe(&u->ring);
    while (!s) { io_uring_submit(&u->ring); s = io_uring_get_sqe(&u->ring); }
    return s;
}

static void arm_recv(loop_t *l, int fd, conn_t *c) {
    struct io_uring_sqe *s = sqe(l);
    io_uring_prep_recv_multishot(s, fd, NULL, 0, 0);
    s->flags |= IOSQE_BUFFER_SELECT;
    s->buf_group = BGID;
    io_uring_sqe_set_data64(s, ud(T_RECV, fd, c->gen));
    c->recv_armed = 1;
}

static void arm_accept(loop_t *l, int lfd) {
    struct io_uring_sqe *s = sqe(l);
    io_uring_prep_multishot_accept(s, lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(s, ud(T_ACCEPT, lfd, 0));
}

void uring_pg_poll(loop_t *l, pgconn_t *p, bool out) {
    struct io_uring_sqe *s = sqe(l);
    if (out) { io_uring_prep_poll_add(s, p->fd, POLLOUT); io_uring_sqe_set_data64(s, ud(T_PGOUT, p->fd, 0)); }
    else { io_uring_prep_poll_multishot(s, p->fd, POLLIN); io_uring_sqe_set_data64(s, ud(T_PGPOLL, p->fd, 0)); }
}

bool uring_init(loop_t *l) {
    uring_t *u = calloc(1, sizeof *u);
    struct io_uring_params p = {0};
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    int rc = io_uring_queue_init_params(4096, &u->ring, &p);
    if (rc < 0) { memset(&p, 0, sizeof p); rc = io_uring_queue_init_params(4096, &u->ring, &p); }
    if (rc < 0) { free(u); return false; }
    struct io_uring_probe *pr = io_uring_get_probe_ring(&u->ring);
    bool ok = pr && io_uring_opcode_supported(pr, IORING_OP_RECV) && io_uring_opcode_supported(pr, IORING_OP_SEND) &&
              io_uring_opcode_supported(pr, IORING_OP_ACCEPT) && io_uring_opcode_supported(pr, IORING_OP_PROVIDE_BUFFERS) &&
              io_uring_opcode_supported(pr, IORING_OP_POLL_ADD) && io_uring_opcode_supported(pr, IORING_OP_ASYNC_CANCEL);
    if (pr) io_uring_free_probe(pr);
    if (!ok) { io_uring_queue_exit(&u->ring); free(u); return false; }
    u->bufs = malloc((size_t)NBUFS * BUFSZ);
    struct io_uring_sqe *s = io_uring_get_sqe(&u->ring);
    io_uring_prep_provide_buffers(s, u->bufs, BUFSZ, NBUFS, BGID, 0);
    io_uring_sqe_set_data64(s, ud(T_PROVIDE, 0, 0));
    io_uring_submit(&u->ring);
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&u->ring, &cqe) < 0 || cqe->res < 0) { io_uring_queue_exit(&u->ring); free(u->bufs); free(u); return false; }
    io_uring_cqe_seen(&u->ring, cqe);
    for (int i = 0; i < NSBUF; i++) { u->sb[i].data = NULL; u->sb[i].next = i + 1 < NSBUF ? i + 1 : -1; }
    u->sb_free = 0;
    l->ring = u;
    for (int i = 0; i < l->nl; i++) arm_accept(l, l->lfd[i]);
    return true;
}

// --- sends: the bytes live in a pooled buffer until the completion ---
static int sb_get(uring_t *u) {
    int i = u->sb_free;
    if (i < 0) { i = -1; return i; }
    u->sb_free = u->sb[i].next;
    return i;
}
static void sb_put(uring_t *u, int i) { u->sb[i].next = u->sb_free; u->sb_free = i; }

static void submit_send(loop_t *l, int idx) {
    uring_t *u = l->ring;
    sbuf_t *b = &u->sb[idx];
    struct io_uring_sqe *s = sqe(l);
    io_uring_prep_send(s, b->fd, b->data + b->off, b->len - b->off, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(s, ud(T_SEND, b->fd, (uint32_t)idx));
}

void uring_send(loop_t *l, int fd, conn_t *c, const uint8_t *bytes, size_t n) {
    uring_t *u = l->ring;
    int idx = sb_get(u);
    if (idx < 0) { // pool exhausted: a huge burst; fall back to a blocking-ish direct write
        size_t off = 0;
        while (off < n) { ssize_t w = write(fd, bytes + off, n - off); if (w < 0) { if (errno == EINTR || errno == EAGAIN) continue; uring_close(l, fd); return; } off += (size_t)w; }
        return;
    }
    sbuf_t *b = &u->sb[idx];
    if (!b->data) b->data = malloc(SBUF_SZ);
    if (n > SBUF_SZ) { free(b->data); b->data = malloc(n); } // rare: bigger than a pool chunk; shrinks back below
    memcpy(b->data, bytes, n);
    b->len = n; b->off = 0; b->fd = fd; b->gen = c->gen;
    c->pending++;
    submit_send(l, idx);
}

bool uring_pg_send(struct loop *l, pgconn_t *p) {
    if (!l->uring || !p->alive) return false;
    struct io_uring_sqe *s = sqe(l);
    io_uring_prep_send(s, p->fd, p->sendbuf.p, p->sendbuf.len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(s, ud(T_PGSEND, p->fd, 0));
    return true;
}

// --- close: cancel everything on the fd, close when the ring has let go ---
static void finish_close(loop_t *l, int fd, conn_t *c) {
    if (c->recv_armed || c->pending) return;
    close(fd);
    loop_conn_reset(l, fd);
}

void uring_close(loop_t *l, int fd) {
    conn_t *c = &l->conns[fd];
    if (!c->open || c->closing) return;
    c->closing = 1;
    if (!c->recv_armed && !c->pending) { finish_close(l, fd, c); return; }
    shutdown(fd, SHUT_RDWR); // completes the multishot recv (0) and the sends (EPIPE) without a cancel request
}

static void reprovide(loop_t *l, int bid) {
    uring_t *u = l->ring;
    struct io_uring_sqe *s = sqe(l);
    io_uring_prep_provide_buffers(s, u->bufs + (size_t)bid * BUFSZ, BUFSZ, 1, BGID, bid);
    io_uring_sqe_set_data64(s, ud(T_PROVIDE, 0, 0));
    s->flags |= IOSQE_CQE_SKIP_SUCCESS; // no completion to process for a re-provide that worked
}

static pgconn_t *pg_by_fd(loop_t *l, int fd) {
    for (int i = 0; i < l->nr; i++) if (l->pgr[i].fd == fd) return &l->pgr[i];
    for (int i = 0; i < l->nw; i++) if (l->pgw[i].fd == fd) return &l->pgw[i];
    return NULL;
}

static int uring_debug = -1;
static void on_cqe(loop_t *l, struct io_uring_cqe *cqe) {
    uring_t *u = l->ring;
    uint64_t d = cqe->user_data;
    int type = (int)(d >> 60), fd = (int)(uint32_t)d; uint32_t x = (uint32_t)(d >> 32) & UD_MASK;
    int res = cqe->res;
    bool more = cqe->flags & IORING_CQE_F_MORE;
    if (uring_debug < 0) uring_debug = getenv("URING_DEBUG") != NULL;
    if (uring_debug) fprintf(stderr, "cqe type=%d fd=%d x=%u res=%d flags=%#x\n", type, fd, x, res, cqe->flags);
    switch (type) {
    case T_ACCEPT: {
        if (res >= 0) {
            conn_t *c = conn_at(l, res);
            c->open = 1; l->nconn++;
            atomic_fetch_add_explicit(&l->st.accepts, 1, memory_order_relaxed);
            arm_recv(l, res, c);
        } else if (res != -ECANCELED && res != -EINTR && res != -ECONNABORTED) {
            if (res == -EMFILE || res == -ENFILE || res == -ENOBUFS || res == -ENOMEM) { /* wait for closes */ }
            else fprintf(stderr, "accept: %s\n", strerror(-res));
        }
        if (!more) arm_accept(l, fd);
        break;
    }
    case T_RECV: {
        conn_t *c = &l->conns[fd];
        if (!c->open || (c->gen & UD_MASK) != x) { if (cqe->flags & IORING_CQE_F_BUFFER) reprovide(l, (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT)); break; }
        if (!more) c->recv_armed = 0;
        if (res > 0) {
            int bid = (int)(cqe->flags >> IORING_CQE_BUFFER_SHIFT);
            if (!c->closing) loop_ingest(l, fd, c, u->bufs + (size_t)bid * BUFSZ, (size_t)res);
            reprovide(l, bid);
            if (c->open && !more && !c->closing) arm_recv(l, fd, c); // the kernel stopped the multishot (buffers ran out): re-arm
        } else if (res == -ENOBUFS) {
            if (c->open && !c->closing) arm_recv(l, fd, c);
        } else { // 0 (EOF), -ECONNRESET, -ECANCELED, ...
            if (c->closing) finish_close(l, fd, c);
            else uring_close(l, fd);
        }
        if (c->open && c->closing) finish_close(l, fd, c);
        break;
    }
    case T_SEND: {
        sbuf_t *b = &u->sb[x];
        conn_t *c = &l->conns[fd];
        bool live = c->open && c->gen == b->gen;
        if (live && res >= 0 && (size_t)res < b->len - b->off) { b->off += (size_t)res; submit_send(l, (int)x); break; } // short send
        if (b->len > SBUF_SZ) { free(b->data); b->data = NULL; }
        sb_put(u, (int)x);
        if (!live) break;
        c->pending--;
        if (res < 0 && res != -EINTR) { uring_close(l, fd); if (c->open && c->closing) finish_close(l, fd, c); break; }
        if (c->closing) { finish_close(l, fd, c); break; }
        if (c->close_after) { if (!c->pending) uring_close(l, fd); break; }
        if (!c->pending && !c->busy && c->inlen) { // bytes the client pipelined while the send was in flight
            size_t n = c->inlen; c->inlen = 0;
            loop_ingest(l, fd, c, c->inbuf, n); // copies into the scratch first, then parses
        }
        break;
    }
    case T_PGPOLL: {
        pgconn_t *p = pg_by_fd(l, fd);
        if (!p || !p->alive) break;
        if (res < 0) { if (res != -ECANCELED) pg_die(p, "poll"); break; }
        if (res & (POLLERR | POLLHUP)) { pg_die(p, "hup"); break; }
        if (res & POLLIN) pg_on_readable(p);
        if (p->alive && !more) uring_pg_poll(l, p, false);
        break;
    }
    case T_PGOUT: {
        pgconn_t *p = pg_by_fd(l, fd);
        if (p && p->alive && res >= 0) pg_on_writable(p);
        break;
    }
    case T_PGSEND: {
        pgconn_t *p = pg_by_fd(l, fd);
        if (p) pg_send_done(p, res);
        break;
    }
    default: break; // T_PROVIDE, T_CANCEL
    }
}

void uring_run(loop_t *l) {
    uring_t *u = l->ring;
    struct __kernel_timespec ts = {0, (long long)cfg.batch_micros * 1000};
    struct __kernel_timespec idle = {1, 0};
    unsigned last_n = 0;
    for (;;) {
        // one enter: submit, then wait the whole window after a light iteration (the batching window),
        // or until anything completes after a busy one
        struct io_uring_cqe *cqe;
        bool window = cfg.batch_micros > 0 && last_n < (unsigned)cfg.batch_min;
        unsigned wait_nr = window ? 4096 : 1;
        int rc = io_uring_submit_and_wait_timeout(&u->ring, &cqe, wait_nr, window ? &ts : &idle, NULL);
        if (rc < 0 && rc != -ETIME && rc != -EINTR) { fprintf(stderr, "io_uring_enter: %s\n", strerror(-rc)); exit(1); }
        if (uring_debug > 0 && rc != -ETIME) fprintf(stderr, "enter rc=%d\n", rc);
        atomic_fetch_add_explicit(&l->st.wakeups, 1, memory_order_relaxed);
        unsigned head, n = 0;
        io_uring_for_each_cqe(&u->ring, head, cqe) { on_cqe(l, cqe); n++; }
        io_uring_cq_advance(&u->ring, n);
        atomic_fetch_add_explicit(&l->st.events, n, memory_order_relaxed);
        loop_flush_jobs(l);
        loop_reconnect_dead(l);
        last_n = n;
        if (window) atomic_fetch_add_explicit(&l->st.sleeps, 1, memory_order_relaxed);
    }
}
