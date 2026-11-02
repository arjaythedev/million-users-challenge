#include "net.h"
#include "http.h"
#include "pg.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HDR_CAP 16384
#define BODY_CAP 16384
#define SLAB_INIT 2048
#define SLAB_SHRINK_AT 8192
#define MAX_REQS 1000
#define IDLE_S 80
#define HDR_TIMEOUT_S 20
/* 128k conns/worker: 4-core (4 workers) needs >250k for the 225k run
 * (64k x 4 = 256k capped it); 16-core (16 workers) needs 1M/16 = 62500 for
 * 1M, and 16 x 128k = 2M total stays under LimitNOFILE 2M. Per-conn idle is
 * ~220B + lazy slabs, so the worst case fits well under 32GB. */
#define MAX_CONNS_PER_WORKER 128000
#define PG_NSLOTS 32
#define PG_R_NSLOTS 24 /* Pass 17: slots 0..23 = R, 24..31 = W */

/* Pass 16b: tuned light-batch coalesce. Sleep only after COAL_K
 * consecutive light batches (0<n<COAL_N), 500us (not 2ms), then the
 * next epoll_wait uses timeout 0 to redrain the slept window into a
 * bigger batch. Busy (n>=COAL_N) or idle (n==0) resets the streak and
 * never sleeps. Refs rival #10/#11/#30. */
#define COAL_N 32
#define COAL_K 3
#define COAL_NS 500000L

/* Pass 10: share-nothing per-thread state in one 64B-aligned block.
 * Groups the old scattered thread-locals plus the new slab freelists and
 * pool stats so workers never false-share, and the 1s sweep / hot path
 * touches one compact block. No atomics, no locks, no cross-thread writes. */
struct worker_state {
    alignas(64) int nconns;
    int epfd;
    struct { int slot; } pg_tags[PG_NSLOTS];
    int pg_armed[PG_NSLOTS];
    int pg_fd[PG_NSLOTS];
    /* Pass 9: 1s-tick-cached clocks. One clock_gettime + one time() per event
     * loop tick per worker replaces per-request time(NULL)x2 and the
     * per-request uptime clock_gettime. */
    time_t cached_wall;
    struct timespec cached_mono;
    /* Pass 10 size-class slab freelists for rbuf (2K/4K/8K/16K/32K, LIFO,
     * capped). Backing is 64B-aligned; steady state does zero malloc. */
    void *slab_head[5];
    unsigned slab_n[5];
    unsigned long slab_hit;
    unsigned long slab_miss;
    unsigned long slab_evict;
    char _pad[64]; /* keep workers' blocks on disjoint lines */
};
static _Thread_local struct worker_state t_w __attribute__((aligned(64))) = {
    .nconns = 0, .epfd = -1,
};

/* Pass 10: slab size classes mirror the old 2x-growth ladder exactly
 * (2K lazy start, doubling to the 32K HDR+BODY cap). Caps bound retained
 * memory per worker: 64x2K + 32x4K + 16x8K + 8x16K + 4x32K = 640K max. */
enum { SLAB_NCLASS = 5 };
static const unsigned SLAB_SZ[SLAB_NCLASS] = { 2048, 4096, 8192, 16384, 32768 };
static const unsigned SLAB_CAPN[SLAB_NCLASS] = { 64, 32, 16, 8, 4 };

static int slab_class_for(unsigned ncap) {
    for (int i = 0; i < SLAB_NCLASS; i++)
        if (SLAB_SZ[i] >= ncap) return i;
    return -1;
}

/* Take a 64B-aligned slab of at least ncap bytes (exact class). NULL on OOM. */
static char *slab_take(unsigned ncap) {
    int ci = slab_class_for(ncap);
    if (ci < 0) return NULL;
    if (t_w.slab_head[ci]) {
        void *p = t_w.slab_head[ci];
        t_w.slab_head[ci] = *(void **)p;
        t_w.slab_n[ci]--;
        t_w.slab_hit++;
        return (char *)p;
    }
    void *p = NULL;
    if (posix_memalign(&p, 64, SLAB_SZ[ci]) != 0) return NULL;
    t_w.slab_miss++;
    return (char *)p;
}

/* Return a slab of known class size cap to the freelist (LIFO); frees when
 * the class is full. Non-class sizes (defensive only) fall back to free. */
static void slab_release(char *p, unsigned cap) {
    if (!p) return;
    int ci = slab_class_for(cap);
    if (ci < 0 || SLAB_SZ[ci] != cap) { free(p); return; }
    if (t_w.slab_n[ci] < SLAB_CAPN[ci]) {
        *(void **)p = t_w.slab_head[ci];
        t_w.slab_head[ci] = p;
        t_w.slab_n[ci]++;
    } else {
        t_w.slab_evict++;
        free(p);
    }
}

/* One malloc per conn: hot (offset 0, 64B-aligned) + cold (offset 64) in a
 * single block. Zeroed like the old calloc. wprev==NULL until the timing
 * wheel links it on register. */
static struct conn_hot *conn_alloc(void) {
    void *mem = NULL;
    size_t total = sizeof(struct conn_hot) + sizeof(struct conn_cold);
    if (posix_memalign(&mem, 64, total) != 0) return NULL;
    memset(mem, 0, total);
    struct conn_hot *c = (struct conn_hot *)mem;
    c->cold = (struct conn_cold *)((char *)mem + sizeof(struct conn_hot));
    c->cold->mslot = MSHOT_NO_SLOT; /* no multishot recv armed (mgen/eof zeroed) */
    c->wprev = NULL;
    c->next = NULL;
    return c;
}

/* Pass 11: timing wheel for idle/header expiry. Single 128-slot circular
 * wheel, 1s per slot; slot(t) = t % 128. Max timeout is IDLE_S+1 = 81s
 * < 128, so every conn is visited exactly once, at its expiry second; the
 * 1s tick expires only slots in (last_sweep, now] -- O(expired), not
 * O(nconns), killing the per-second L1d flush at 1M conns.
 * Pass 19 (Greptile #6): per-slot doubly-linked lists with a sentinel head
 * (next/wprev intrusive in hot, which stays exactly 64B). Unlink and
 * insert are both O(1) -- the old singly-linked unlink scanned the whole
 * slot list (O(n^2) on mass-close bursts). conn_free() unlinks, so every
 * close path is covered. wheel_rearm() always unlinks+reinserts (O(1));
 * the old same-slot early-out is gone because no slot index is stored, but
 * both halves are pointer swaps either way. */
#define WHEEL_N 128
static _Thread_local struct conn_hot t_wheel_head[WHEEL_N];

/* Pass 12: O(1) per-batch liveness guard. The old guard re-scanned the whole
 * timing wheel (O(nconns)) on every client event to detect a conn freed by
 * an earlier PG event in the same epoll_wait batch. Instead each worker
 * records every conn it frees during one loop iteration (sweep + event
 * batch) in this small closed-set; the guard is a pointer scan of that set
 * (<= CLOSED_CAP entries, i.e. O(1) w.r.t. connection count). Overflow sets
 * a flag and the guard falls back to the exact wheel scan for the rest of
 * that tick, so the close verdict stays airtight. Pointer reuse within a
 * tick is harmless: a recycled address has no event queued in the current
 * batch, and skipping the stale event is the correct verdict. All frees go
 * through conn_free(), so every close path is covered. */
#define CLOSED_CAP 256
static _Thread_local struct conn_hot *t_closed[CLOSED_CAP];
static _Thread_local int t_closed_n;
static _Thread_local int t_closed_overflow;

/* Pass 13: per-PG-slot waiter counts + Pass 19 (Greptile #12) intrusive
 * per-slot DB waiter lists. Every PG_WAIT conn is counted under its claimed
 * PG slot: inc when http_respond defers a new waiter (the sole PG_WAIT
 * entry point, in handle_data), dec when a resume completes (the sole
 * PG_WAIT exit in wheel_poll_waiters / the handle_pg_ready kick) or when a
 * PG_WAIT conn is freed (sweep expiry / close paths via conn_free, which
 * sees state==CONN_PG_WAIT only before any resume dec, so no double
 * count). Counts let net_gc_pg disarm waiter-less slots and let completion
 * skip waiter-less PG events; the intrusive per-slot list (t_pg_qhead,
 * wq_* links in the cold stash) makes each completion O(waiters on that
 * slot) instead of scanning the 128-slot timing wheel (O(1M idles) per DB
 * event under load). Linked exactly while state==CONN_PG_WAIT; unlink
 * happens on completion or free, never twice (wq_owner NULLs on unlink). */
static _Thread_local int t_pg_waiters[PG_NSLOTS];
static _Thread_local struct pg_pending *t_pg_qhead[PG_NSLOTS];
/* Pass 21: total live PG_WAIT waiters on this worker (summed over slots).
 * Bounds the intrusive lists to PG_WAIT_MAX (4096): http_defer fails fast
 * past it, so every completion stays O(actual waiters) with a hard ceiling
 * instead of queueing without bound when the pool saturates. */
static _Thread_local int t_pg_waiters_total;

static inline void waiter_unlink(struct pg_pending *p, int slot) {
    if (!p->wq_owner) return; /* already unlinked */
    if (p->wq_prev) {
        p->wq_prev->wq_next = p->wq_next;
    } else if (slot >= 0 && slot < PG_NSLOTS && t_pg_qhead[slot] == p) {
        t_pg_qhead[slot] = p->wq_next;
    }
    if (p->wq_next) p->wq_next->wq_prev = p->wq_prev;
    p->wq_next = NULL;
    p->wq_prev = NULL;
    p->wq_owner = NULL;
}

static inline void waiter_link(struct conn_hot *c) {
    struct pg_pending *p = &c->cold->pend;
    int slot = p->slot;
    if (p->wq_owner) {
        /* Defensive: a live link must never be linked again (double-link
         * self-loops the intrusive list and hangs the completion poll in
         * an infinite loop). Unlink first; the accounting count is
         * unchanged (still exactly one waiter). */
        waiter_unlink(p, slot);
    } else if (p->wq_next || p->wq_prev) {
        /* Stale pointers without an owner (should be unreachable under the
         * del-before-free discipline): drop them, do not trust them. */
        p->wq_next = NULL;
        p->wq_prev = NULL;
    }
    p->wq_owner = c;
    p->wq_prev = NULL;
    p->wq_next = (slot >= 0 && slot < PG_NSLOTS) ? t_pg_qhead[slot] : NULL;
    if (p->wq_next) p->wq_next->wq_prev = p;
    if (slot >= 0 && slot < PG_NSLOTS) t_pg_qhead[slot] = p;
}

static inline void pg_waiter_add(struct conn_hot *c) {
    int slot = c->cold->pend.slot;
    if (slot >= 0 && slot < PG_NSLOTS) t_pg_waiters[slot]++;
    /* Pass 21: total bound accounting (mirrors the per-slot count). */
    t_pg_waiters_total++;
    waiter_link(c);
}

/* Completion/free path. Takes the saved (node, slot): http_resume's
 * completion frees the stash, so callers must pass the pre-resume values.
 * Unlink is idempotent (wq_owner NULLs on unlink). Node links are owned by
 * the list and cleared only here -- never inside http_pending_free. */
static inline void pg_waiter_del_node(struct pg_pending *pn, int slot) {
    waiter_unlink(pn, slot);
    if (slot >= 0 && slot < PG_NSLOTS && t_pg_waiters[slot] > 0) t_pg_waiters[slot]--;
    /* Pass 21: total bound accounting (guarded; del paths are exclusive). */
    if (t_pg_waiters_total > 0) t_pg_waiters_total--;
}

static inline unsigned wheel_idx(time_t t) {
    return (unsigned)(t % WHEEL_N);
}

static inline struct conn_hot *wheel_head(unsigned s) {
    return &t_wheel_head[s];
}

/* Linked iff wprev != NULL. Sentinels are never unlinked (discipline: only
 * real conns pass through here). */
static inline void wheel_insert_at(struct conn_hot *c, unsigned s) {
    struct conn_hot *h = wheel_head(s);
    c->next = h->next;
    c->wprev = h;
    h->next->wprev = c;
    h->next = c;
}

/* Unlink c from its wheel slot, if linked. O(1). No-op when wprev == NULL. */
static void wheel_unlink(struct conn_hot *c) {
    if (!c->wprev) return;
    c->wprev->next = c->next;
    c->next->wprev = c->wprev;
    c->wprev = NULL;
    c->next = NULL;
}

/* (Re)arm c's expiry for activity at time now. Steady-state conns expire at
 * now+81 (first tick with now-last_active > IDLE_S); header-stalled conns
 * (no complete request yet, partial bytes buffered) expire at
 * created_at+21 (first tick with now-created_at > HDR_TIMEOUT_S).
 * Already-past hints land on the next tick, preserving the old sweep's 1s
 * close granularity. */
static void wheel_rearm(struct conn_hot *c, time_t now) {
    time_t e;
    if (c->req_count == 0 && c->rlen > 0)
        e = c->cold->created_at + HDR_TIMEOUT_S + 1;
    else
        e = now + IDLE_S + 1;
    if (e <= now) e = now + 1;
    wheel_unlink(c);
    wheel_insert_at(c, wheel_idx(e));
}

static inline void tick_now(void) {
    clock_gettime(CLOCK_MONOTONIC, &t_w.cached_mono);
    time(&t_w.cached_wall);
}

int net_waiter_count(int slot) {
    if (slot < 0 || slot >= PG_NSLOTS) return 0;
    return t_pg_waiters[slot];
}

/* Pass 21: total live PG_WAIT waiters on this worker (O(1)). */
int net_waiter_total(void) {
    return t_pg_waiters_total;
}

time_t net_now_wall(void) {
    return t_w.cached_wall;
}

long current_uptime_s(void) {
    long d = (long)(t_w.cached_mono.tv_sec - g_start_mono.tv_sec);
    return d < 0 ? 0 : d;
}

static int set_nb(int fd) {
    int f = fcntl(fd, F_GETFL, 0);
    if (f < 0) return -1;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK);
}

static int make_listener(const char *host, int port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* Dual-stack: try IPv6 any then bind host. Simplest: resolve host. */
    struct addrinfo hints, *res = NULL, *rp = NULL;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0) { close(fd); return -1; }
    int bound = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (rp->ai_family == AF_INET6) {
            int off = 0;
            setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
        }
        if (bind(s, rp->ai_addr, rp->ai_addrlen) == 0) { bound = s; break; }
        close(s);
    }
    freeaddrinfo(res);
    close(fd); /* template closed; use bound */
    if (bound < 0) return -1;
#ifdef TCP_DEFER_ACCEPT
    /* Wake the listener only once peer data arrives (1s cap): fewer accept
     * wakeups from half-open / speculative connections. */
    { int dval = 1; setsockopt(bound, IPPROTO_TCP, TCP_DEFER_ACCEPT, &dval, sizeof dval); }
#endif
    if (listen(bound, 65535) != 0) { close(bound); return -1; }
    set_nb(bound);
    return bound;
}

/* Pass 20: forward decls (defined after the pbuf ring section). */
static void mshot_slot_release(struct conn_hot *c);
static int mshot_on_data(struct conn_hot *c, char *b, unsigned int n);
static int handle_data_boxed(struct conn_hot *c);

static void conn_free(struct conn_hot *c, int epfd) {
    if (!c) return;
    /* Pass 12: record in the per-tick closed-set for the O(1) guard. */
    if (t_closed_n < CLOSED_CAP) t_closed[t_closed_n++] = c;
    else t_closed_overflow = 1;
    wheel_unlink(c); /* every close path covered; no-op when already unlinked */
    t_w.nconns--;
    /* Pass 20: generation-drop close. Release the recv slot (bumps the
     * generation so in-flight CQEs go stale) and shutdown() so the armed
     * multishot ends promptly -- no extra CANCEL SQE. */
    mshot_slot_release(c);
    if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    if (c->rbuf) slab_release(c->rbuf, c->rcap);
    struct conn_cold *cl = c->cold;
    if (cl) {
        free(cl->wbuf);
        if (c->state == CONN_PG_WAIT) {
            pg_waiter_del_node(&cl->pend, cl->pend.slot); /* sweep/close of a live waiter */
            http_pending_free(&cl->pend);
        }
    }
    free(c);
}

/* ---- Pass 3: io_uring multishot accept with epoll fallback ---- */
#define URING_ENTRIES 2048
#define ACCEPTS_PER_LISTENER 2
#define TAG_L1 ((void *)(intptr_t)-1)
#define TAG_L2 ((void *)(intptr_t)-2)
#define TAG_URING ((void *)(intptr_t)-3)

/* ---- Pass 18: provided-buffer recv (pbuf ring) ----
 * Per-thread ring of 1024 x 4KB buffers (fixed 4MB, 64B-aligned, allocated
 * once at worker startup so it sits in baseline RSS, outside per-conn idle
 * math). The epoll data path recvs into the next ring slot and parses in
 * place (pico, zero-copy); the slot is "returned" by cursor advance at
 * handler return -- nothing is retained (http_defer dups every stashed
 * field). Only partial (need-more) or pipelined requests are boxed into the
 * size-class slab (existing slow path, unchanged).
 * The ring is also provided to the io_uring instance (PROVIDE_BUFFERS,
 * bgid 1) so a future pass can submit buffer-select multishot recvs without
 * re-wiring; recv-multishot support is probed at startup and logged, while
 * Pass 20 arms one buffer-select RECV_MULTISHOT SQE per conn on this ring
 * (generational user_data, cancel-on-close via generation-drop, refill at
 * <=25% avail); the epoll recv path stays as the runtime fallback
 * (USE_EPOLL_RECV forces it at build time). */
#define PBUF_N 1024
#define PBUF_SZ 4096
#define PBUF_BGID 1
#define PBUF_PROVIDE_TAG (-1)

static _Thread_local char *t_pbuf_base = NULL;
static _Thread_local unsigned t_pbuf_cur = 0;
static _Thread_local char t_recv_probe_buf[2048];

/* ---- Pass 20: multishot recv lifecycle (transport only) ----
 * One RECV_MULTISHOT SQE per conn, armed at accept with IOSQE_BUFFER_SELECT
 * on this worker's provided-buffer ring (bgid = worker index, PBUF_N x
 * PBUF_SZ). user_data is generational (gen<<32 | slot): slot indexes this
 * worker's recv table, gen pairs with the table generation. Close bumps the
 * generation and shutdown()s the fd (no extra CANCEL SQE); any in-flight
 * CQE then mismatches and is dropped as stale. Buffers consumed by the
 * kernel (F_BUFFER bid) are recycled in bulk once avail falls to 1/4
 * (low-water refill, one PROVIDE_BUFFERS SQE per consumed bid). Re-arm
 * happens only when a multishot ends without EOF (nomore counter); steady
 * state is ~1 SQE/conn. No cache, no batching change: bytes are parsed
 * exactly like the epoll path (zero-copy fast path, slab box otherwise).
 * Rule 7: ring buffers are per-recv transport, never retained data. */
#define MSHOT_REFILL_DIV 4   /* refill when avail <= PBUF_N/4 */
struct mshot_slot { struct conn_hot *c; unsigned gen; };
static _Thread_local struct mshot_slot *t_mslots = NULL;
static _Thread_local int t_mfree_head = -1;
static _Thread_local int t_mshot_on = 0;
static _Thread_local unsigned t_pbuf_bgid = PBUF_BGID;
static _Thread_local unsigned t_pb_avail = 0;
static _Thread_local unsigned t_pb_dirty_n = 0;
static _Thread_local unsigned t_pb_dirty[PBUF_N];
static _Thread_local unsigned long t_ms_conns, t_ms_cqes, t_ms_rearms;
static _Thread_local unsigned long t_ms_stale, t_ms_refills, t_ms_nomore, t_ms_ubid;

/* Idle client epoll interest. Multishot owns IN; epoll keeps OUT (stash
 * drain) + ERR/HUP only. Fallback path keeps the old EPOLLIN. */
static inline uint32_t cli_in(void) { return t_mshot_on ? 0u : (uint32_t)EPOLLIN; }

/* Allocate the recv table + intrusive free list (head index in
 * t_mfree_head, next index smuggled in c while free). 0 ok, -1 OOM. */
static int mshot_table_init(void) {
    if (t_mslots) return 0;
    t_mslots = (struct mshot_slot *)calloc((size_t)MAX_CONNS_PER_WORKER, sizeof *t_mslots);
    if (!t_mslots) return -1;
    for (int i = 0; i < MAX_CONNS_PER_WORKER; i++)
        t_mslots[i].c = (struct conn_hot *)(intptr_t)(i + 1);
    t_mslots[MAX_CONNS_PER_WORKER - 1].c = (struct conn_hot *)(intptr_t)-1;
    t_mfree_head = 0;
    return 0;
}

/* Claim a table slot. Returns the index, or -1 when exhausted. */
static int mshot_slot_alloc(void) {
    if (!t_mslots || t_mfree_head < 0) return -1;
    int s = t_mfree_head;
    t_mfree_head = (int)(intptr_t)t_mslots[s].c;
    if (t_mslots[s].gen == 0) t_mslots[s].gen = 1; /* gen 0 collides with accept CQEs */
    t_mslots[s].c = NULL;
    return s;
}

/* Generation-drop release (every close path funnels through conn_free).
 * Clears ownership, pushes the slot, bumps the generation past any
 * in-flight CQE, and shutdown()s so the armed multishot ends promptly. */
static void mshot_slot_release(struct conn_hot *c) {
    if (!c || !c->cold) return;
    unsigned s = c->cold->mslot;
    c->cold->mslot = MSHOT_NO_SLOT;
    if (s == MSHOT_NO_SLOT) return;
    (void)shutdown(c->fd, SHUT_RDWR); /* no extra CANCEL SQE; the EOF CQE goes stale */
    if (!t_mslots || s >= (unsigned)MAX_CONNS_PER_WORKER) return;
    if (t_mslots[s].c == c) {
        t_mslots[s].c = (struct conn_hot *)(intptr_t)t_mfree_head;
        t_mfree_head = (int)s;
        if (++t_mslots[s].gen == 0) t_mslots[s].gen = 1;
    }
}

/* Arm one buffer-select multishot recv on c's fd. 0 ok, -1 ring full.
 * Submits immediately: accepts are rare (vs requests), and the first
 * request on a conn must not wait a tick for a piggybacked submit. */
static int mshot_recv_submit(struct io_uring *ring, struct conn_hot *c) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        if (io_uring_submit(ring) < 0) return -1;
        sqe = io_uring_get_sqe(ring);
        if (!sqe) return -1;
    }
    io_uring_prep_recv_multishot(sqe, c->fd, NULL, PBUF_SZ, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = (unsigned short)t_pbuf_bgid;
    io_uring_sqe_set_data64(sqe, ((unsigned long long)c->cold->mgen << 32) | c->cold->mslot);
    return io_uring_submit(ring) < 0 ? -1 : 0;
}

/* Record a kernel-consumed buffer (recycled at low water, even for stale
 * CQEs -- the kernel spent it either way). */
static void mshot_note_buffer(unsigned bid) {
    if (bid >= (unsigned)PBUF_N || t_pb_dirty_n >= (unsigned)PBUF_N) { t_ms_ubid++; }
    else t_pb_dirty[t_pb_dirty_n++] = bid;
    if (t_pb_avail > 0) t_pb_avail--;
}

/* Bulk re-provide consumed bids at/below low water. */
static void mshot_refill(struct io_uring *ring) {
    if (!t_mshot_on || t_pb_dirty_n == 0) return;
    if (t_pb_avail > (unsigned)PBUF_N / MSHOT_REFILL_DIV) return;
    unsigned done = 0;
    while (done < t_pb_dirty_n) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
        if (!sqe) {
            if (io_uring_submit(ring) <= 0) break;
            continue;
        }
        unsigned bid = t_pb_dirty[done];
        io_uring_prep_provide_buffers(sqe, t_pbuf_base + (size_t)bid * PBUF_SZ,
                                      PBUF_SZ, 1, (int)t_pbuf_bgid, bid);
        io_uring_sqe_set_data64(sqe, (unsigned long long)(long)PBUF_PROVIDE_TAG);
        done++;
    }
    if (done > 0) {
        io_uring_submit(ring);
        t_ms_refills++;
        t_pb_avail += done;
        if (done < t_pb_dirty_n)
            memmove(t_pb_dirty, t_pb_dirty + done, (t_pb_dirty_n - done) * sizeof t_pb_dirty[0]);
        t_pb_dirty_n -= done;
    }
}

static int pbuf_init(void) {
    if (t_pbuf_base) return 0;
    void *p = NULL;
    if (posix_memalign(&p, 64, (size_t)PBUF_N * PBUF_SZ) != 0) return -1;
    t_pbuf_base = (char *)p;
    t_pbuf_cur = 0;
    return 0;
}

static inline char *pbuf_next(void) {
    if (!t_pbuf_base && pbuf_init() != 0) return NULL;
    char *b = t_pbuf_base + (size_t)t_pbuf_cur * PBUF_SZ;
    t_pbuf_cur++;
    if (t_pbuf_cur >= PBUF_N) t_pbuf_cur = 0;
    return b;
}

/* Register the whole ring with the uring instance (one PROVIDE_BUFFERS SQE,
 * PBUF_N buffers of PBUF_SZ from bid 0). The provide CQE (data64 = -1) is
 * soaked by the caller via uring_drain, which skips that tag. */
static int pbuf_provide(struct io_uring *ring) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        if (io_uring_submit(ring) < 0) return -1;
        sqe = io_uring_get_sqe(ring);
        if (!sqe) return -1;
    }
    io_uring_prep_provide_buffers(sqe, t_pbuf_base, PBUF_SZ, PBUF_N, (int)t_pbuf_bgid, 0);
    io_uring_sqe_set_data64(sqe, (unsigned long long)(long)PBUF_PROVIDE_TAG);
    return io_uring_submit(ring) >= 0 ? 0 : -1;
}

/* Probe IORING_OP_RECV_MULTISHOT + IOSQE_BUFFER_SELECT on a throwaway
 * socketpair (same pattern as the multishot-accept probe). 1 when a
 * buffer-select multishot recv arms on this kernel, 0 otherwise. */
static int uring_recv_mshot_probe(void) {
    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) != 0)
        return 0;
    int supported = 0;
    struct io_uring pring;
    struct io_uring_params pp;
    memset(&pp, 0, sizeof pp);
    pp.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
    if (io_uring_queue_init_params(8, &pring, &pp) == 0) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&pring);
        if (sqe) {
            io_uring_prep_provide_buffers(sqe, t_recv_probe_buf, sizeof t_recv_probe_buf, 1, 77, 0);
            io_uring_sqe_set_data64(sqe, 0x50524244ull); /* 'PRBD' */
            if (io_uring_submit(&pring) == 1) {
                struct io_uring_cqe *cqe = NULL;
                struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
                if (io_uring_wait_cqe_timeout(&pring, &cqe, &ts) == 0 && cqe) {
                    int ok = (cqe->res == 0);
                    io_uring_cqe_seen(&pring, cqe);
                    if (ok) {
                        sqe = io_uring_get_sqe(&pring);
                        if (sqe) {
                            io_uring_prep_recv_multishot(sqe, sv[0], NULL, 2048, 0);
                            sqe->flags |= IOSQE_BUFFER_SELECT;
                            sqe->buf_group = 77;
                            io_uring_sqe_set_data64(sqe, 0x52564356ull); /* 'RVCV' */
                            if (io_uring_submit(&pring) == 1) {
                                (void)!write(sv[1], "x", 1);
                                struct __kernel_timespec ts2 = { .tv_sec = 0, .tv_nsec = 300 * 1000 * 1000 };
                                if (io_uring_wait_cqe_timeout(&pring, &cqe, &ts2) == 0 && cqe) {
                                    if (cqe->res > 0) supported = 1;
                                    else if (cqe->res < 0 && cqe->res != -EINVAL) supported = 1;
                                    io_uring_cqe_seen(&pring, cqe);
                                } else {
                                    supported = 1; /* still pending: multishot armed */
                                }
                            }
                        }
                    }
                }
            }
        }
        io_uring_queue_exit(&pring);
    }
    if (sv[0] >= 0) close(sv[0]);
    if (sv[1] >= 0) close(sv[1]);
    return supported;
}

static struct conn_hot *conn_register(int epfd, struct io_uring *ring, int afd, time_t now) {
    int one = 1;
    setsockopt(afd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (t_w.nconns >= MAX_CONNS_PER_WORKER) { close(afd); return NULL; }
    struct conn_hot *c = conn_alloc();
    if (!c) { close(afd); return NULL; }
    c->fd = afd; c->last_active = now; c->cold->created_at = now; c->keep_alive = 1;
    c->state = CONN_ACTIVE;
    t_w.nconns++;
    /* Pass 9 lazy slab: no read buffer until the first bytes arrive
     * (1M conns x 0B instead of 1M x 4K). First recv allocates SLAB_INIT. */
    c->rbuf = NULL;
    c->rcap = 0;
    wheel_rearm(c, now); /* idle expiry now+81 (rlen==0 at accept) */
    struct epoll_event cev;
    /* Pass 20: multishot owns IN; epoll watches OUT/HUP only. */
    cev.events = cli_in();
    cev.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, afd, &cev) != 0) {
        conn_free(c, -1);
        return NULL;
    }
    if (t_mshot_on && ring) {
        int s = mshot_slot_alloc();
        if (s < 0) { conn_free(c, epfd); return NULL; }
        c->cold->mslot = (unsigned)s;
        c->cold->mgen = t_mslots[s].gen;
        t_mslots[s].c = c;
        t_ms_conns++;
        if (mshot_recv_submit(ring, c) != 0) { conn_free(c, epfd); return NULL; }
    }
    return c;
}

/* Classic accept4 drain (fallback path). */
static void epoll_accept_all(int epfd, int lfd, time_t now) {
    while (1) {
        int afd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (afd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EMFILE || errno == ENFILE) { usleep(10*1000); break; }
            break;
        }
        conn_register(epfd, NULL, afd, now);
    }
}

/* Submit one multishot accept for lfd. 0 ok, -1 ring full. */
static int uring_accept_submit(struct io_uring *ring, int lfd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        if (io_uring_submit(ring) < 0) return -1;
        sqe = io_uring_get_sqe(ring);
        if (!sqe) return -1;
    }
    io_uring_prep_multishot_accept(sqe, lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, (unsigned long long)(unsigned int)lfd);
    return 0;
}

/* Probe multishot-accept support on a throwaway loopback listener so real
 * traffic is never disturbed. Returns 1 if a multishot accept arms. */
static int uring_mshot_probe(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int supported = 0;
    struct io_uring pring;
    struct io_uring_params pp;
    memset(&pp, 0, sizeof pp);
    pp.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 8) == 0 &&
        io_uring_queue_init_params(8, &pring, &pp) == 0) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&pring);
        if (sqe) {
            io_uring_prep_multishot_accept(sqe, s, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            io_uring_sqe_set_data64(sqe, 0);
            if (io_uring_submit(&pring) == 1) {
                struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
                struct io_uring_cqe *cqe = NULL;
                int rc = io_uring_wait_cqe_timeout(&pring, &cqe, &ts);
                if (rc != 0) {
                    supported = 1; /* still pending: multishot armed */
                } else if (cqe) {
                    if (cqe->res >= 0) { close(cqe->res); supported = 1; }
                    else if (cqe->res != -EINVAL) supported = 1;
                    io_uring_cqe_seen(&pring, cqe);
                }
            }
        }
        io_uring_queue_exit(&pring);
    }
    close(s);
    return supported;
}

/* Drain ready completions: multishot accepts (gen 0) register new conns,
 * multishot recvs (gen != 0) deliver transport bytes. *einval counts
 * consecutive -EINVAL verdicts (kernel without multishot support). */
static void uring_drain(struct io_uring *ring, int epfd,
                        time_t now, int *einval) {
    struct io_uring_cqe *cqe;
    int resub = 0;
    while (io_uring_peek_cqe(ring, &cqe) == 0) {
        unsigned long long ud = io_uring_cqe_get_data64(cqe);
        int res = cqe->res;
        unsigned flags = cqe->flags;
        io_uring_cqe_seen(ring, cqe);
        if ((long long)ud == (long long)(long)PBUF_PROVIDE_TAG) continue; /* provide-buffers CQE, no conn */
        unsigned gen = (unsigned)(ud >> 32);
        if (gen == 0) {
            /* Accept completion (gen 0 is reserved: live recvs never use it). */
            int lfd = (int)ud;
            if (res >= 0) {
                conn_register(epfd, ring, res, now);
                *einval = 0;
            } else if (res == -EMFILE || res == -ENFILE) {
                usleep(10 * 1000);
            } else if (res == -EINVAL) {
                (*einval)++;
            }
            if (!(flags & IORING_CQE_F_MORE)) {
                if (uring_accept_submit(ring, lfd) == 0) resub = 1;
            }
            continue;
        }
        /* Pass 20 recv completion. */
        unsigned slot = (unsigned)ud;
        int more = (flags & IORING_CQE_F_MORE) != 0;
        unsigned bid = 0;
        int has_buf = (flags & IORING_CQE_F_BUFFER) != 0;
        if (has_buf) {
            bid = flags >> IORING_CQE_BUFFER_SHIFT;
            /* Recycle even when stale: the kernel spent the buffer. */
            mshot_note_buffer(bid);
        }
        struct conn_hot *rc = (t_mslots && slot < (unsigned)MAX_CONNS_PER_WORKER)
            ? t_mslots[slot].c : NULL;
        if (!rc || t_mslots[slot].gen != gen ||
            rc->cold->mslot != slot || rc->cold->mgen != gen) {
            t_ms_stale++; /* generation-drop: already closed/recycled */
            continue;
        }
        t_ms_cqes++;
        if (res == 0 && !more) {
            /* FIN: drain boxed backlog like the epoll EOF path, then close
             * unless a drain-deferred waiter now owns the conn. A pending
             * stash (or one raised by the drain) closes via the EPOLLOUT
             * eof verdict instead of being dropped. */
            rc->cold->mshot_eof = 1;
            if (rc->cold->wbuf) continue;
            if (rc->state != CONN_PG_WAIT && rc->rlen > 0)
                (void)handle_data_boxed(rc);
            if (rc->state == CONN_PG_WAIT || rc->cold->wbuf) continue;
            conn_free(rc, epfd);
            continue;
        }
        if (res < 0) { conn_free(rc, epfd); continue; } /* RST/err */
        if (!has_buf || bid >= (unsigned)PBUF_N || res <= 0) {
            /* res==0 with MORE is a spurious empty: no re-arm needed. */
            if (res != 0) { t_ms_ubid++; conn_free(rc, epfd); }
            continue;
        }
        if (!mshot_on_data(rc, t_pbuf_base + (size_t)bid * PBUF_SZ, (unsigned)res)) {
            conn_free(rc, epfd);
            continue;
        }
        if (!more) {
            /* Multishot ended without EOF (not observed in practice):
             * re-arm the same slot/gen so the conn stays live. */
            t_ms_nomore++;
            t_ms_rearms++;
            if (mshot_recv_submit(ring, rc) != 0) conn_free(rc, epfd);
        }
    }
    mshot_refill(ring);
    if (resub) io_uring_submit(ring);
}

/* ---- Pass 6: PG socket arming in the same epoll ---- */

static int pg_tag_slot(void *ptr) {
    if (ptr >= (void *)&t_w.pg_tags[0] && ptr <= (void *)&t_w.pg_tags[PG_NSLOTS - 1])
        return (int)((void *)ptr - (void *)&t_w.pg_tags[0]) / (int)sizeof t_w.pg_tags[0];
    return -1;
}

static void net_arm_pg(int slot) {
    if (slot < 0 || slot >= PG_NSLOTS) return;
    if (t_w.epfd < 0) return;
    int fd = pg_socket(slot);
    if (fd < 0) return;
    struct epoll_event ev;
    /* EPOLLIN normally suffices for result readiness. An idle PG socket is
     * always writable, so a standing EPOLLOUT arm would wake the loop
     * continuously at 100% CPU; arm OUT only while a flush is pending. */
    ev.events = EPOLLIN | (pg_need_write(slot) ? EPOLLOUT : 0);
    ev.data.ptr = &t_w.pg_tags[slot];
    if (t_w.pg_armed[slot]) {
        if (t_w.pg_fd[slot] == fd) {
            /* Greptile #7: MOD can hit ENOENT when the PG conn reconnected
             * and the new socket reused the old fd number while the old
             * registration lapsed (DEL of a replaced fd). Retry as ADD so
             * the slot does not go silently unarmed. */
            if (epoll_ctl(t_w.epfd, EPOLL_CTL_MOD, fd, &ev) != 0 &&
                errno == ENOENT) {
                if (epoll_ctl(t_w.epfd, EPOLL_CTL_ADD, fd, &ev) == 0)
                    t_w.pg_fd[slot] = fd;
                else { t_w.pg_armed[slot] = 0; t_w.pg_fd[slot] = -1; }
            }
        } else {
            epoll_ctl(t_w.epfd, EPOLL_CTL_DEL, t_w.pg_fd[slot], NULL);
            if (epoll_ctl(t_w.epfd, EPOLL_CTL_ADD, fd, &ev) == 0) t_w.pg_fd[slot] = fd;
            else { t_w.pg_armed[slot] = 0; t_w.pg_fd[slot] = -1; }
        }
    } else {
        if (epoll_ctl(t_w.epfd, EPOLL_CTL_ADD, fd, &ev) == 0) {
            t_w.pg_armed[slot] = 1;
            t_w.pg_fd[slot] = fd;
        }
    }
}

/* Drop PG arming for slots with no waiters (level-triggered POLLOUT would
 * otherwise spin while idle). Also heal ownerless busy slots: a waiter
 * freed mid-flight (or a transport-error failover) can leave a slot busy
 * with no one left to poll it; the heal frees it once its orphan query
 * completes. Only heals when the waiter count is zero, so it can never
 * steal a live waiter's result (see pg_send_async ownership gate). */
/* Pass 21: single-slot GC. The completion path knows exactly which slot
 * fired, so it heals/disarms only that slot -- O(1) + O(its waiters) --
 * instead of re-scanning all 32 slots per PG event. The full sweep below
 * stays for the 1s tick, where O(32) once a second is noise. */
static void net_gc_pg_slot(int slot) {
    if (t_w.epfd < 0) return;
    if (slot < 0 || slot >= PG_NSLOTS) return;
    if (t_pg_waiters[slot] == 0 && pg_slot_busy(slot)) pg_slot_heal(slot);
    if (t_w.pg_armed[slot] && t_pg_waiters[slot] == 0) {
        epoll_ctl(t_w.epfd, EPOLL_CTL_DEL, t_w.pg_fd[slot], NULL);
        t_w.pg_armed[slot] = 0;
        t_w.pg_fd[slot] = -1;
    }
    /* count > 0 implies needed: keep armed, no scan. */
}

static void net_gc_pg(void) {
    for (int s = 0; s < PG_NSLOTS; s++) net_gc_pg_slot(s);
}

/* Pass 10: shrink an oversized slab back to SLAB_INIT once fully drained.
 * Same policy as Pass 9 (2K floor, >8K trigger), but via the size-class
 * pool: the big slab returns to the freelist (or free when full) and the
 * 2K replacement comes from the pool. No per-request free/malloc churn. */
static void slab_maybe_shrink(struct conn_hot *c) {
    if (c->rcap > SLAB_SHRINK_AT && c->rlen == 0 && c->rbuf) {
        char *nb = slab_take(SLAB_INIT);
        if (!nb) return; /* OOM: keep the big slab, try next sweep */
        slab_release(c->rbuf, c->rcap);
        c->rbuf = nb; c->rcap = SLAB_INIT;
    }
}

/* Re-arm a conn for EPOLLOUT (unsent response pending). 0 ok, -1 on error. */
int net_arm_out(struct conn_hot *c) {
    if (!c || t_w.epfd < 0) return -1;
    struct epoll_event ev;
    ev.events = cli_in() | EPOLLOUT;
    ev.data.ptr = c;
    return epoll_ctl(t_w.epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* Best-effort drain of stashed response bytes. Returns 0 when drained (or
 * would-block: stays armed for the next EPOLLOUT), -1 on transport error
 * OR when a Connection:close stash just finished draining (Greptile #11:
 * the close must happen only after the last byte is sent; callers treat -1
 * as close). */
static int drain_pending(struct conn_hot *c) {
    struct conn_cold *cl = c->cold;
    if (!cl->wbuf) {
        if (cl->close_after_drain) {
            /* Drained on an earlier event; the close verdict was deferred
             * until the bytes were out. */
            cl->close_after_drain = 0;
            return -1;
        }
        if (t_w.epfd >= 0) {
            struct epoll_event ev;
            ev.events = cli_in();
            ev.data.ptr = c;
            epoll_ctl(t_w.epfd, EPOLL_CTL_MOD, c->fd, &ev);
        }
        return 0;
    }
    while (cl->woff < cl->wlen) {
        ssize_t r = send(c->fd, cl->wbuf + cl->woff, cl->wlen - cl->woff, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                net_arm_out(c);
                return 0;
            }
            return -1;
        }
        if (r == 0) return -1;
        cl->woff += (size_t)r;
    }
    free(cl->wbuf);
    cl->wbuf = NULL; cl->wlen = 0; cl->woff = 0;
    if (cl->close_after_drain) {
        cl->close_after_drain = 0;
        return -1; /* stash fully sent: now close */
    }
    if (t_w.epfd >= 0) {
        struct epoll_event ev;
        ev.events = cli_in();
        ev.data.ptr = c;
        epoll_ctl(t_w.epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }
    /* Pass 20: a drained stash on a FIN'd conn closes now (the EOF CQE
     * already fired; no further wakeup is coming). */
    if (cl->mshot_eof) return -1;
    return 0;
}

/* Greptile #8: pause client EPOLLIN while PG_WAIT. Level-triggered epoll
 * re-reports an idle-but-armed client fd on every wait; the handler could
 * only ignore it (state==CONN_PG_WAIT early-return), spinning the loop at
 * 100% CPU while the DB works. MOD to no events on defer; restore EPOLLIN
 * when the waiter resumes. Safe if the fd was already replaced/closed
 * (MOD-ENOENT falls back to ADD; DEL is idempotent). */
static void client_pause(struct conn_hot *c) {
    if (t_w.epfd < 0) return;
    struct epoll_event ev;
    ev.events = 0;
    ev.data.ptr = c;
    epoll_ctl(t_w.epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

static void client_resume(struct conn_hot *c) {
    if (t_w.epfd < 0) return;
    struct epoll_event ev;
    /* Restore EPOLLIN; keep EPOLLOUT if a response stash is still draining
     * (Greptile #11: dropping OUT here would strand the stash with no
     * wakeup left to finish it). */
    ev.events = cli_in() | (c->cold->wbuf ? (uint32_t)EPOLLOUT : 0u);
    ev.data.ptr = c;
    if (epoll_ctl(t_w.epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0 && errno == ENOENT)
        epoll_ctl(t_w.epfd, EPOLL_CTL_ADD, c->fd, &ev);
}

/* Box pbuf bytes [b, b+n) into the conn slab (slow-path entry). Grows
 * the slab exactly like the old recv loop (lazy 2K start, doubling to the
 * 32K HDR+BODY cap). 0 ok, -1 on over-cap/OOM (caller closes). */
static int box_pbuf(struct conn_hot *c, const char *b, unsigned int n) {
    while (c->rlen + n > c->rcap) {
        unsigned int ncap = c->rcap ? c->rcap * 2 : SLAB_INIT;
        if (ncap > HDR_CAP + BODY_CAP) ncap = HDR_CAP + BODY_CAP;
        if (ncap <= c->rcap || c->rlen + n > ncap) return -1; /* cap hit */
        char *nb = slab_take(ncap);
        if (!nb) return -1;
        int ci = slab_class_for(ncap);
        unsigned int take_cap = (ci >= 0) ? SLAB_SZ[ci] : ncap;
        if (c->rbuf) {
            if (c->rlen) memcpy(nb, c->rbuf, c->rlen);
            slab_release(c->rbuf, c->rcap);
        }
        c->rbuf = nb; c->rcap = take_cap;
    }
    memcpy(c->rbuf + c->rlen, b, n);
    c->rlen += n;
    return 0;
}

static int handle_data_boxed(struct conn_hot *c);

/* Pass 18 fast path: rlen==0 (nothing boxed yet). One recv into the next
 * pbuf ring slot, parse in place, and -- when the datagram holds exactly one
 * complete request -- dispatch zero-copy and return the slot by cursor
 * advance. Partial (need-more) or pipelined bytes are boxed into the slab
 * and handled by the unchanged slow path below (one extra EAGAIN recv at
 * most). Same contract: 1 keep-alive, 0 close, HTTP_DEFERRED handling. */
static int handle_data(struct conn_hot *c) {
    if (c->state == CONN_PG_WAIT) return 1; /* waiting: ignore client events */
    if (c->cold->wbuf) { /* stalled sender: drain first, don't grow backlog */
        if (drain_pending(c) != 0) return 0;
        if (c->cold->wbuf) return 1;
    }
    if (t_mshot_on) {
        /* Multishot owns the socket: new bytes arrive via CQEs (boxed while
         * PG_WAIT). Never recv() here -- it would split a request with the
         * armed multishot. Parse the boxed backlog only. */
        if (c->cold->mshot_eof && c->rlen == 0) return 0; /* FIN already seen */
        if (c->rlen == 0) return 1;
        return handle_data_boxed(c);
    }
    if (c->rlen > 0) return handle_data_boxed(c); /* partial already boxed */
    char *pb = pbuf_next();
    if (!pb) return handle_data_boxed(c); /* ring OOM: fall back to slab */
    ssize_t r = recv(c->fd, pb, PBUF_SZ, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return 0;
    }
    if (r == 0) return 0; /* peer closed */
    http_req_t rq;
    int n = http_parse(pb, (size_t)r, &rq);
    if (n == -2) return 0; /* body too large: close (out of scope) */
    if (n < 0) return 0;   /* malformed: close */
    if ((size_t)n < (size_t)r) {
        /* Partial or pipelined: box everything, take the slow path. */
        if (box_pbuf(c, pb, (unsigned int)r) != 0) return 0;
        return handle_data_boxed(c);
    }
    /* Exactly one complete request: zero-copy dispatch (rq points into pb,
     * valid for this call; http_defer dups everything it keeps). */
    c->req_count++;
    long up = current_uptime_s();
    int ka = http_respond(c, &rq, up);
    c->last_active = t_w.cached_wall;
    wheel_rearm(c, t_w.cached_wall);
    if (ka == HTTP_DEFERRED) {
        pg_waiter_add(c); /* new waiter enters PG_WAIT (count + list) */
        net_arm_pg(c->cold->pend.slot);
        client_pause(c); /* Greptile #8: no EPOLLIN spin while PG_WAIT */
        return 1; /* keep waiting; resume on PG readiness */
    }
    if (c->cold->wbuf) return 1; /* EAGAIN stash: wait for EPOLLOUT */
    if (!ka) return 0;
    if (c->req_count >= MAX_REQS) return 0; /* close after 1000 reqs */
    slab_maybe_shrink(c);
    return 1;
}

/* Slow path (was handle_data): slab recv + pipelining loop, unchanged.
 * Returns 1 keep-alive, 0 close. Handles pipelined requests in a loop.
 * HTTP_DEFERRED from http_respond means the request is stashed in PG_WAIT:
 * shift consumed bytes, arm the PG socket, and stop pipelining until resume. */
static int handle_data_boxed(struct conn_hot *c) {
    if (c->state == CONN_PG_WAIT) return 1; /* waiting: ignore client events */
    if (c->cold->wbuf) { /* stalled sender: drain first, don't grow backlog */
        if (drain_pending(c) != 0) return 0;
        if (c->cold->wbuf) return 1;
    }
    /* Grow slab if needed (lazy: first bytes allocate SLAB_INIT).
     * Pass 10: pooled 64B-aligned size classes; old slab recycles. */
    int eof = 0; /* peer FIN seen: parse buffered completes before closing */
    /* Pass 20: multishot owns the socket (EOF arrives as a CQE with
     * mshot_eof latched); skip the recv, parse the boxed backlog only. */
    while (!t_mshot_on) {
        if (c->rlen == c->rcap) {
            unsigned int ncap = c->rcap ? c->rcap * 2 : SLAB_INIT;
            if (ncap > HDR_CAP + BODY_CAP) ncap = HDR_CAP + BODY_CAP;
            if (ncap <= c->rcap) return 0; /* cap hit -> close */
            char *nb = slab_take(ncap);
            if (!nb) return 0;
            int ci = slab_class_for(ncap);
            unsigned int take_cap = (ci >= 0) ? SLAB_SZ[ci] : ncap;
            if (c->rbuf) {
                if (c->rlen) memcpy(nb, c->rbuf, c->rlen);
                slab_release(c->rbuf, c->rcap);
            }
            c->rbuf = nb; c->rcap = take_cap;
        }
        ssize_t r = recv(c->fd, c->rbuf + c->rlen, c->rcap - c->rlen, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return 0;
        }
        /* Greptile #9: a FIN (shutdown WR) with pipelined bytes buffered
         * must not drop the buffered completes. Mark EOF, fall into the
         * parse loop below, and close only when no complete request
         * remains (need-more or drained with EOF set). */
        if (r == 0) { eof = 1; break; }
        c->rlen += (unsigned int)r;
        if (c->rlen > HDR_CAP + BODY_CAP) return 0;
        break;
    }
    /* Pipelining loop: parse + respond while a full request is buffered. */
    while (c->rlen > 0) {
        http_req_t rq;
        int n = http_parse(c->rbuf, c->rlen, &rq);
        if (n == 0) {
            /* Greptile #10: the 16K cap applies to the header alone, not
             * header+buffered-body. When the header terminator is present
             * we are only waiting for body bytes (total cap enforced by
             * the slab); only a terminator-less buffer past HDR_CAP is a
             * header-cap violation. */
            if (!memmem(c->rbuf, c->rlen, "\r\n\r\n", 4) && c->rlen > HDR_CAP)
                return 0; /* header cap 16K */
            if (eof) return 0; /* FIN: nothing complete left */
            if (c->req_count == 0 && c->rlen > 0)
                wheel_rearm(c, t_w.cached_wall); /* header-stall expiry */
            break; /* need more */
        }
        if (n == -2) return 0; /* body too large: close (out of scope) */
        if (n < 0) return 0;   /* malformed: close */
        c->req_count++;
        long up = current_uptime_s();
        int ka = http_respond(c, &rq, up);
        /* Shift consumed bytes. */
        size_t consumed = (size_t)n;
        if (consumed < c->rlen) memmove(c->rbuf, c->rbuf + consumed, c->rlen - consumed);
        c->rlen -= (unsigned int)consumed;
        c->last_active = t_w.cached_wall;
        wheel_rearm(c, t_w.cached_wall);
        if (ka == HTTP_DEFERRED) {
            pg_waiter_add(c); /* new waiter enters PG_WAIT (count + list) */
            net_arm_pg(c->cold->pend.slot);
            client_pause(c); /* Greptile #8: no EPOLLIN spin while PG_WAIT */
            return 1; /* keep waiting; resume on PG readiness */
        }
        if (c->cold->wbuf) return 1; /* EAGAIN stash: wait for EPOLLOUT */
        if (!ka) return 0;
        if (c->req_count >= MAX_REQS) return 0; /* close after 1000 reqs */
        /* loop for pipelined next request */
    }
    if (eof) return 0; /* peer went away and the buffer is drained */
    slab_maybe_shrink(c);
    return 1;
}

/* Pass 20: handle one buffer-select recv CQE's bytes [b, b+n), which live in
 * the provided ring (zero-copy: owned by the kernel until the next refill,
 * never retained past this call -- http_defer dups everything it stashes).
 * Mirrors handle_data's contract (1 keep-alive, 0 close) without any recv()
 * syscall: exact single requests dispatch in place; partial/pipelined bytes
 * are boxed into the slab and parsed by the unchanged pipelining loop. */
static int mshot_on_data(struct conn_hot *c, char *b, unsigned int n) {
    if (c->cold->mshot_eof) return 0; /* defensive: FIN already latched */
    if (c->state == CONN_PG_WAIT) { /* waiting: box pipelined bytes, resume parses */
        if (box_pbuf(c, b, n) != 0) return 0;
        return 1;
    }
    if (c->cold->wbuf) { /* stalled sender: drain first, box what arrived */
        if (drain_pending(c) != 0) return 0;
        if (c->cold->wbuf) {
            if (box_pbuf(c, b, n) != 0) return 0;
            return 1;
        }
    }
    if (c->rlen > 0) {
        if (box_pbuf(c, b, n) != 0) return 0;
        return handle_data_boxed(c);
    }
    http_req_t rq;
    int pn = http_parse(b, (size_t)n, &rq);
    if (pn == -2) return 0; /* body too large: close (out of scope) */
    if (pn < 0) return 0;   /* malformed: close */
    if ((size_t)pn < (size_t)n) {
        /* Partial or pipelined: box everything, take the slow path. */
        if (box_pbuf(c, b, n) != 0) return 0;
        return handle_data_boxed(c);
    }
    /* Exactly one complete request: zero-copy dispatch (rq points into the
     * ring slot, valid for this call; http_defer dups everything it keeps). */
    c->req_count++;
    long up = current_uptime_s();
    int ka = http_respond(c, &rq, up);
    c->last_active = t_w.cached_wall;
    wheel_rearm(c, t_w.cached_wall);
    if (ka == HTTP_DEFERRED) {
        pg_waiter_add(c); /* new waiter enters PG_WAIT (count + list) */
        net_arm_pg(c->cold->pend.slot);
        client_pause(c); /* no EPOLLIN spin while PG_WAIT */
        return 1; /* keep waiting; resume on PG readiness */
    }
    if (c->cold->wbuf) return 1; /* EAGAIN stash: wait for EPOLLOUT */
    if (!ka) return 0;
    if (c->req_count >= MAX_REQS) return 0; /* close after 1000 reqs */
    slab_maybe_shrink(c);
    return 1;
}

/* c just finished its PG wait (state ACTIVE, stash already freed).
 * r is http_resume's verdict (1 keep / 0 close). Frees c when closing
 * (wheel_unlink happens inside conn_free), else restores client EPOLLIN
 * (paused during PG_WAIT, Greptile #8), rearms the idle expiry and drains
 * pipelined data buffered while waiting.
 * Returns 1 when c was freed, 0 when c stays. Callers iterate waiter lists
 * with a saved-next pattern, so c may die underneath. */
static int finish_completed(struct conn_hot *c, int r) {
    c->last_active = t_w.cached_wall;
    wheel_rearm(c, t_w.cached_wall);
    if (!r || c->req_count >= (unsigned)MAX_REQS) {
        conn_free(c, t_w.epfd);
        return 1;
    }
    client_resume(c); /* PG_WAIT pause ends before any client recv */
    int alive = handle_data(c);
    if (c->state == CONN_PG_WAIT) {
        net_arm_pg(c->cold->pend.slot);
        return 0;
    }
    if (!alive) {
        conn_free(c, t_w.epfd);
        return 1;
    }
    return 0;
}

/* Progress every waiter bound to PG slot; free waiters whose resumed
 * response closes the conn (conn_free unlinks from the wheel). Buffered
 * pipelined data is drained inline.
 * Multi-pass: a completion frees the slot, so queued (sent==0) waiters are
 * retried in the same event instead of stalling for a future wakeup.
 * Pass 19 (Greptile #12): iterates the slot's intrusive waiter list with a
 * saved-next pattern -- resume may free c underneath us, and a resumed
 * keep-alive may defer a NEW waiter (head insert: safe under saved-next),
 * but no wheel slot pointer is held across the call. Completion is
 * O(waiters on this slot), not O(all conns). */
/* Poll all waiters bound to PG slot with sent==want_sent. Progress shares
 * the old loop semantics exactly: a completion (freed or resumed) or a
 * queued->in-flight advance counts. */
static int wheel_poll_waiters(int slot, int want_sent) {
    if (slot < 0 || slot >= PG_NSLOTS || t_pg_waiters[slot] == 0) return 0; /* O(1) skip */
    int progress = 0;
    struct pg_pending *pn = t_pg_qhead[slot];
    while (pn) {
        struct pg_pending *wn = pn->wq_next; /* resume may free c */
        struct conn_hot *c = pn->wq_owner;
        if (c && c->state == CONN_PG_WAIT && c->cold->pend.slot == slot &&
            pn == &c->cold->pend && pn->sent == want_sent) {
            int was_sent = pn->sent;
            int wslot = pn->slot; /* resume frees the stash: save first */
            int r = http_resume(c);
            if (r == HTTP_DEFERRED) {
                if (pn->sent != was_sent) progress = 1; /* queued -> in flight */
                net_arm_pg(slot);
            } else {
                pg_waiter_del_node(pn, wslot); /* waiter completed */
                progress = 1;
                finish_completed(c, r);
            }
        }
        pn = wn;
    }
    return progress;
}

static void handle_pg_ready_one(int slot) {
    for (int pass = 0; pass < 8; pass++) {
        int progress = 0;
        /* Phase 1: poll waiters with a query in flight. */
        progress |= wheel_poll_waiters(slot, 1);
        /* Phase 2: retry queued sends now that slots may be free. */
        progress |= wheel_poll_waiters(slot, 0);
        if (!progress) break;
    }
    /* Airtight kick: a queued waiter facing a free slot must not wait for an
     * event that will never come (socket idle, no POLLIN ahead). One
     * optimistic retry guarantees a future wakeup either way: a successful
     * send yields result data (POLLIN), a BUSY verdict means someone is in
     * flight whose completion will fire. */
    if (slot >= 0 && slot < PG_NSLOTS && t_pg_waiters[slot] > 0) {
        struct pg_pending *pn = t_pg_qhead[slot];
        while (pn) {
            struct pg_pending *wn = pn->wq_next;
            struct conn_hot *c = pn->wq_owner;
            if (c && c->state == CONN_PG_WAIT && pn == &c->cold->pend &&
                c->cold->pend.slot == slot && !pn->sent) {
                int wslot = pn->slot;
                int r = http_resume(c);
                if (r == HTTP_DEFERRED) {
                    net_arm_pg(slot);
                } else {
                    pg_waiter_del_node(pn, wslot); /* waiter completed */
                    finish_completed(c, r);
                }
                break;
            }
            pn = wn;
        }
    }
    net_gc_pg_slot(slot); /* Pass 21: only the fired slot, not all 32 */
}

/* Pass 17: packed read completions. When any R slot fires, drain ALL R
 * waiters in one wheel sweep per phase instead of one slot per epoll event.
 * Each read keeps its own Sync/autocommit timing (per-slot PQflush/poll
 * inside http_resume); packing only batches completions per pass so a
 * 20-row feed burst completes in fewer wakeups. Single in-flight per conn
 * is preserved (wheel_poll_waiters single-slot path is untouched for W).
 * No shared-txn group commit: writes keep their own Sync each. */
static int wheel_poll_reads_packed(int want_sent) {
    int any = 0;
    for (int q = 0; q < PG_R_NSLOTS; q++) {
        if (t_pg_waiters[q] > 0) { any = 1; break; }
    }
    if (!any) return 0;
    int progress = 0;
    for (int q = 0; q < PG_R_NSLOTS; q++) {
        struct pg_pending *pn = t_pg_qhead[q];
        while (pn) {
            struct pg_pending *wn = pn->wq_next; /* resume may free c */
            struct conn_hot *c = pn->wq_owner;
            if (c && c->state == CONN_PG_WAIT && pn == &c->cold->pend &&
                c->cold->pend.slot == q && pn->sent == want_sent) {
                int was_sent = pn->sent;
                int r = http_resume(c);
                if (r == HTTP_DEFERRED) {
                    if (pn->sent != was_sent) progress = 1;
                    net_arm_pg(q);
                } else {
                    pg_waiter_del_node(pn, q);
                    progress = 1;
                    finish_completed(c, r);
                }
            }
            pn = wn;
        }
    }
    return progress;
}


static void handle_pg_ready(int slot) {
    if (slot >= 0 && slot < PG_R_NSLOTS) {
        /* Packed R path: shared pass over all R waiters (fewer wakeups). */
        for (int pass = 0; pass < 8; pass++) {
            int progress = 0;
            progress |= wheel_poll_reads_packed(1);
            progress |= wheel_poll_reads_packed(0);
            if (!progress) break;
        }
        /* Airtight kick: a queued R waiter facing a free R slot must not
         * stall (socket idle, no POLLIN ahead). Retry one optimistically. */
        {
            int done = 0;
            for (int q = 0; q < PG_R_NSLOTS && !done; q++) {
                struct pg_pending *pn = t_pg_qhead[q];
                while (pn && !done) {
                    struct pg_pending *wn = pn->wq_next;
                    struct conn_hot *c = pn->wq_owner;
                    if (c && c->state == CONN_PG_WAIT && pn == &c->cold->pend &&
                        c->cold->pend.slot == q && !pn->sent) {
                        int r = http_resume(c);
                        if (r == HTTP_DEFERRED) {
                            net_arm_pg(q);
                        } else {
                            pg_waiter_del_node(pn, q);
                            finish_completed(c, r);
                        }
                        done = 1;
                    }
                    pn = wn;
                }
            }
        }
        net_gc_pg_slot(slot); /* Pass 21: firing slot only; other R slots
                                 * GC on their own events + the 1s tick */
        return;
    }
    handle_pg_ready_one(slot);
}

/* Pass 20: drop back to epoll recv. Disarms multishot, re-arms EPOLLIN on
 * every live ACTIVE conn (PG_WAIT stays paused; the resume path re-arms),
 * releases the table. Runs at most once per process lifetime. */
static void mshot_fallback_all(int epfd) {
    t_mshot_on = 0;
    for (int w = 0; w < WHEEL_N; w++) {
        struct conn_hot *h = wheel_head((unsigned)w);
        for (struct conn_hot *c = h->next; c != h; c = c->next) {
            c->cold->mslot = MSHOT_NO_SLOT;
            if (c->state != CONN_ACTIVE) continue; /* paused waiter: resume re-arms */
            struct epoll_event ev;
            ev.events = EPOLLIN | (c->cold->wbuf ? (uint32_t)EPOLLOUT : 0u);
            ev.data.ptr = c;
            if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0 && errno == ENOENT)
                epoll_ctl(epfd, EPOLL_CTL_ADD, c->fd, &ev);
        }
    }
    free(t_mslots); t_mslots = NULL; t_mfree_head = -1;
    t_pb_avail = 0; t_pb_dirty_n = 0;
    fprintf(stderr, "multishot recv disabled at runtime, epoll recv fallback\n");
}

/* Kernel rejected multishot at runtime: drop the ring, fall back. */
static void uring_fallback(struct io_uring *ring, int epfd, int l1, int l2,
                           long idx, int *have_uring, int *einval) {
    if (t_mshot_on) mshot_fallback_all(epfd);
    epoll_ctl(epfd, EPOLL_CTL_DEL, ring->ring_fd, NULL);
    io_uring_queue_exit(ring);
    *have_uring = 0;
    *einval = 0;
    struct epoll_event lev;
    lev.events = EPOLLIN;
    lev.data.ptr = TAG_L1; epoll_ctl(epfd, EPOLL_CTL_ADD, l1, &lev);
    lev.data.ptr = TAG_L2; epoll_ctl(epfd, EPOLL_CTL_ADD, l2, &lev);
    fprintf(stderr, "worker %ld: io_uring disabled, epoll accept fallback\n", idx);
}

void *worker_main(void *arg) {
    long idx = (long)arg;
    (void)idx;

    int l1 = make_listener(g_host, g_port);
    int l2 = make_listener(g_host, g_extra_port);
    if (l1 < 0 || l2 < 0) {
        fprintf(stderr, "worker %ld: bind %s:%d/%d failed: %s\n", idx, g_host, g_port, g_extra_port, strerror(errno));
        if (l1 >= 0) close(l1);
        if (l2 >= 0) close(l2);
        /* Overload hardening: never exit() the process from a worker
         * thread (that kills healthy workers mid-bench). Report via the
         * thread return; main converts any failure to exit code 1. */
        return (void *)1;
    }

    pg_init_thread(g_db_url[0] ? g_db_url : NULL);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) { perror("epoll_create1"); return (void *)1; }
    t_w.epfd = epfd;
    for (int i = 0; i < PG_NSLOTS; i++) {
        t_w.pg_tags[i].slot = i;
        t_w.pg_armed[i] = 0;
        t_w.pg_fd[i] = -1;
        t_pg_waiters[i] = 0;
        t_pg_qhead[i] = NULL;
    }
    t_pg_waiters_total = 0; /* Pass 21: total waiter bound accounting */
    /* Pass 19: init timing-wheel sentinels (each head self-linked). */
    for (int i = 0; i < WHEEL_N; i++) {
        t_wheel_head[i].next = &t_wheel_head[i];
        t_wheel_head[i].wprev = &t_wheel_head[i];
    }
    if (idx == 0)
        fprintf(stderr, "conn_hot size=%zu align=%zu cold=%zu\n",
                sizeof(struct conn_hot), alignof(struct conn_hot),
                sizeof(struct conn_cold));
    /* Pass 3: io_uring multishot accept (2 per listener, depth 2048,
     * SINGLE_ISSUER|COOP_TASKRUN). The ring fd is pollable, so one epoll_wait
     * still drives everything; classic accept4 is the fallback. */
    struct io_uring ring;
    int have_uring = 0;
    memset(&ring, 0, sizeof ring);
    if (uring_mshot_probe()) {
        struct io_uring_params p;
        memset(&p, 0, sizeof p);
        p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN;
        p.sq_entries = URING_ENTRIES; p.cq_entries = URING_ENTRIES * 2;
        if (io_uring_queue_init_params(URING_ENTRIES, &ring, &p) == 0) {
            int ok = 1;
            for (int k = 0; k < ACCEPTS_PER_LISTENER; k++) {
                if (uring_accept_submit(&ring, l1) != 0) ok = 0;
                if (uring_accept_submit(&ring, l2) != 0) ok = 0;
            }
            if (ok && io_uring_submit(&ring) == 2 * ACCEPTS_PER_LISTENER) {
                struct epoll_event uev;
                uev.events = EPOLLIN;
                uev.data.ptr = TAG_URING;
                if (epoll_ctl(epfd, EPOLL_CTL_ADD, ring.ring_fd, &uev) == 0)
                    have_uring = 1;
            }
            if (!have_uring) io_uring_queue_exit(&ring);
        }
    }
    int uring_einval = 0;
    if (have_uring) {
        fprintf(stderr, "worker %ld: io_uring multishot accept active\n", idx);
    } else {
        /* listeners use distinct ptr markers (-1/-2). */
        struct epoll_event lev;
        lev.events = EPOLLIN;
        lev.data.ptr = TAG_L1; epoll_ctl(epfd, EPOLL_CTL_ADD, l1, &lev);
        lev.data.ptr = TAG_L2; epoll_ctl(epfd, EPOLL_CTL_ADD, l2, &lev);
    }

    struct epoll_event events[256];
    int coal_streak = 0;  /* consecutive light batches */
    int coal_drain = 0;   /* next epoll_wait uses timeout 0 (redrain) */
    tick_now(); /* seed cached clocks before any pg/time use */
    /* Pass 18/20: per-thread pbuf ring (1024x4KB = fixed 4MB, allocated
     * once at worker startup so it lands in baseline RSS, outside per-conn
     * idle math) + PROVIDE_BUFFERS to the ring (bgid = worker index).
     * Pass 20 arms multishot recvs on it when the probe passes; the epoll
     * recv path stays as the runtime/build-time fallback. */
    if (pbuf_init() != 0) {
        fprintf(stderr, "worker %ld: pbuf ring alloc failed\n", idx);
        return (void *)1;
    }
    /* Pass 20: per-worker buffer group id (bgids are per-ring). */
    t_pbuf_bgid = (unsigned)idx;
    int recv_mshot = 0;
    if (have_uring && pbuf_provide(&ring) == 0) {
        uring_drain(&ring, epfd, t_w.cached_wall, &uring_einval); /* soak provide CQE */
#ifdef USE_EPOLL_RECV
        (void)uring_recv_mshot_probe; /* present but never armed */
        recv_mshot = 0; /* build-time force of the epoll recv path */
#else
        recv_mshot = uring_recv_mshot_probe();
#endif
    }
    /* Pass 20: activate multishot recv only with ring + buffers + probe +
     * table all good; otherwise the epoll recv path below stays live. */
    if (recv_mshot && mshot_table_init() == 0) {
        t_mshot_on = 1;
        t_pb_avail = (unsigned)PBUF_N;
    }
    fprintf(stderr, "worker %ld: pbuf ring 1024x4KB bgid=%u %s, %s\n",
            idx, t_pbuf_bgid, have_uring ? "provided" : "ready",
            t_mshot_on ? "multishot recv ACTIVE" : "epoll recv active (multishot unsupported-or-disabled)");
    time_t last_sweep = t_w.cached_wall;
#ifdef DEBUG_SLAB
    unsigned long sweep_n = 0;
#endif

    while (1) {
        int coal_timeout = coal_drain ? 0 : 1000; /* 1s idle sweep tick */
        coal_drain = 0;
        int n = epoll_wait(epfd, events, 256, coal_timeout);
#ifdef DEBUG_TICK
        { /* Pass 10 temp: tick-latency probe (compile out by default). */
            struct timespec _t0, _t1;
            clock_gettime(CLOCK_MONOTONIC, &_t0);
            tick_now();
            clock_gettime(CLOCK_MONOTONIC, &_t1);
            long _dt = (long)(_t1.tv_sec - _t0.tv_sec) * 1000000000L
                     + (long)(_t1.tv_nsec - _t0.tv_nsec);
            fprintf(stderr, "worker %ld: tick %ld ns wall=%ld\n",
                    idx, _dt, (long)t_w.cached_wall);
        }
#else
        tick_now();
#endif
        time_t now = t_w.cached_wall;
        /* Pass 12: open a fresh per-tick closed-set; every conn_free below
         * (sweep + event batch) records here for the O(1) guard. */
        t_closed_n = 0;
        t_closed_overflow = 0;
        if (now - last_sweep >= 1) {
            /* Pass 11: expire wheel slots in (last_sweep, now] -- O(expired
             * conns), not O(nconns). Close conditions are byte-identical to
             * the old linear sweep; every conn is visited exactly at its
             * expiry second (128 slots > 81s max timeout). */
            time_t t0 = last_sweep;
            last_sweep = now;
            for (time_t t = t0 + 1; t <= now; t++) {
                struct conn_hot *h = wheel_head(wheel_idx(t));
                struct conn_hot *c = h->next;
                while (c != h) {
                    struct conn_hot *nx = c->next; /* conn_free unlinks c */
                    /* Prefetch the expiry burst ahead; closes dominate and
                     * survivors are near-zero under exact scheduling. */
                    if (nx != h) __builtin_prefetch(nx, 0, 3);
                    __builtin_prefetch(c->cold, 0, 3);
                    if (t - c->last_active > IDLE_S ||
                        (c->req_count == 0 && c->rlen > 0 && t - c->cold->created_at > HDR_TIMEOUT_S)) {
                        conn_free(c, epfd); /* unlinks from the wheel */
                    } else {
                        /* Not due (unreachable under exact scheduling:
                         * placement is always exact, so the conn is already
                         * home); just shrink oversized idle slabs. */
                        slab_maybe_shrink(c);
                    }
                    c = nx;
                }
            }
            net_gc_pg();
            /* Pass 20: 60s multishot counters. Steady state expects
             * rearms/conns ~= 0 (one SQE/conn + re-arm only on !MORE). */
            if (t_mshot_on) {
                static _Thread_local unsigned long stat_tick = 0;
                if (++stat_tick % 60 == 0)
                    fprintf(stderr, "worker %ld: mshot conns=%lu cqes=%lu rearms=%lu stale=%lu refills=%lu nomore=%lu ubid=%lu avail=%u dirty=%u\n",
                            idx, t_ms_conns, t_ms_cqes, t_ms_rearms, t_ms_stale,
                            t_ms_refills, t_ms_nomore, t_ms_ubid, t_pb_avail, t_pb_dirty_n);
            }
#ifdef DEBUG_SLAB
            if (++sweep_n % 60 == 0) {
                unsigned long tot = t_w.slab_hit + t_w.slab_miss;
                fprintf(stderr, "worker %ld: slab hit=%lu miss=%lu evict=%lu hitrate=%.1f%% pooled=%u/%u/%u/%u/%u\n",
                        idx, t_w.slab_hit, t_w.slab_miss, t_w.slab_evict,
                        tot ? 100.0 * (double)t_w.slab_hit / (double)tot : 0.0,
                        t_w.slab_n[0], t_w.slab_n[1], t_w.slab_n[2],
                        t_w.slab_n[3], t_w.slab_n[4]);
            }
#endif
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Pass 20: drain recv/accept CQEs every tick (the ring-fd edge may
         * coalesce under burst; peek is cheap when empty). The EINVAL
         * fallback is checked at the TAG_URING event below. */
        if (t_mshot_on && have_uring) {
            uring_drain(&ring, epfd, now, &uring_einval);
            if (uring_einval >= 4)
                uring_fallback(&ring, epfd, l1, l2, idx, &have_uring, &uring_einval);
        }
        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;
            int pgslot = pg_tag_slot(ptr);
            if (pgslot >= 0) {
                handle_pg_ready(pgslot);
            } else if (ptr == TAG_URING) {
                if (have_uring) {
                    uring_drain(&ring, epfd, now, &uring_einval);
                    if (uring_einval >= 4)
                        uring_fallback(&ring, epfd, l1, l2, idx, &have_uring, &uring_einval);
                }
            } else if (ptr == TAG_L1 || ptr == TAG_L2) {
                if (!have_uring) {
                    int lfd = (ptr == TAG_L1) ? l1 : l2;
                    epoll_accept_all(epfd, lfd, now);
                }
            } else {
                struct conn_hot *c = (struct conn_hot *)ptr;
                /* Pass 20: the pre-batch uring_drain above (or the sweep, or
                 * a PG event earlier in this batch) may already have freed c
                 * while this tick's epoll_wait still queued its HUP/IN/OUT
                 * event. The O(1) guard must precede EVERY use -- including
                 * the HUP fast path below, which would otherwise double-free
                 * (heap corruption) or drain a dead stash. */
                int dead = 0;
                if (t_closed_overflow) {
                    dead = 1;
                    for (int w = 0; w < WHEEL_N && dead; w++) {
                        struct conn_hot *h = wheel_head((unsigned)w);
                        for (struct conn_hot *x = h->next; x != h; x = x->next) {
                            if (x == c) { dead = 0; break; }
                        }
                    }
                } else {
                    for (int k = 0; k < t_closed_n; k++) {
                        if (t_closed[k] == c) { dead = 1; break; }
                    }
                }
                if (dead) continue;
                /* Pass 19 (Greptile #8): a hung-up/errored fd with no
                 * readable data is dead -- close it immediately instead of
                 * spinning the level-triggered HUP/ERR (this fires even on
                 * EPOLLIN-paused PG_WAIT fds). With EPOLLIN also set, fall
                 * through to handle_data so buffered pipelined bytes (FIN
                 * after data, Greptile #9) are parsed before any EOF close.
                 * conn_free unlinks a live PG_WAIT waiter, so the slot is
                 * reclaimable by the ownership-gate heal. */
                if ((events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) &&
                    !(events[i].events & EPOLLIN)) {
                    /* Pass 21: only a waiter close can orphan a slot; a
                     * non-waiter close needs no GC at all (was: 32-slot scan). */
                    int hup_gc = (c->state == CONN_PG_WAIT) ? c->cold->pend.slot : -1;
                    conn_free(c, epfd); /* unlinks from the wheel internally */
                    net_gc_pg_slot(hup_gc);
                    continue;
                }
                int alive;
                if (t_mshot_on) {
                    /* Multishot owns IN: epoll here is EPOLLOUT (stash
                     * drain, eof verdict closes) + HUP only. */
                    if ((events[i].events & EPOLLOUT) && drain_pending(c) != 0)
                        alive = 0;
                    else
                        alive = 1;
                } else alive = handle_data(c);
                if (!alive) {
                    /* Pass 21: scoped GC (see HUP path above). */
                    int cls_gc = (c->state == CONN_PG_WAIT) ? c->cold->pend.slot : -1;
                    conn_free(c, epfd); /* unlinks from the wheel internally */
                    net_gc_pg_slot(cls_gc);
                }
            }
        }
        /* Pass 16b: gated coalesce; busy (n>=COAL_N) or idle (n==0)
         * resets the streak and never sleeps. Only K consecutive light
         * batches sleep 500us; the next epoll_wait(0) redrains the slept
         * window into a bigger batch. n<0 is EINTR/break above. */
        if (n >= COAL_N || n == 0) {
            coal_streak = 0;
        } else if (n > 0) {
            if (++coal_streak >= COAL_K) {
                struct timespec coal_ts = { 0, COAL_NS };
                nanosleep(&coal_ts, NULL);
                coal_streak = 0;
                coal_drain = 1;
            }
        }
    }
    return NULL;
}
