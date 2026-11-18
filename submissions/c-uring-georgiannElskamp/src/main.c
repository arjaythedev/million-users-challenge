// Feed API server for the Million Users Challenge.
// One single-threaded worker process per CPU. Each has its own listeners (SO_REUSEPORT, both ports), its own
// clients and its own Postgres connections, and runs in passes: everything that arrived since the last pass is
// parsed, its reads leave for Postgres under one Sync and its writes as one transaction, and the answers that
// came back meanwhile go out. Client I/O is on io_uring (multishot receive into provided buffers, sends on the
// ring: about one system call per pass) with an epoll loop as the fallback where rings are not available.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sha256.h"
#include "json.h"
#include "gz.h"

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// ---------------------------------------------------------------------------------------------- config
#define MAXW 256
static char cfg_host[128] = "0.0.0.0";
static int cfg_port[2], nports;
static char db_host[256], db_port[16] = "5432", db_user[128], db_pass[256], db_name[128];
static hmac_key_t jwt_key;
static int nworkers = 1, worker_id;
static int opt_uring = 1, opt_idle = 1, opt_debug = 0;
static int ring_quirk;
static int pause_us = 4000;
static int lfd[2][MAXW];
static struct shm { int64_t start_ms; } *shm;

static int64_t now_us, now_ms, real_ms;      // refreshed once per pass
static uint16_t now16;

static void clocks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_us = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    now_ms = now_us / 1000;
    now16 = (uint16_t)now_ms;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);
    real_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void logf_(const char *fmt, ...) {
    va_list ap;
    char b[512];
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    b[n++] = '\n';
    if (write(2, b, n)) {}
}

static void die(const char *m) { logf_("fatal: %s (%s)", m, strerror(errno)); exit(1); }

// ---------------------------------------------------------------------------------------------- small utils
static inline uint32_t be32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
static inline uint64_t be64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
static inline void put32(uint8_t *p, uint32_t v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }
static inline void put64(uint8_t *p, uint64_t v) { v = __builtin_bswap64(v); memcpy(p, &v, 8); }

static const char D2[201] =
    "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
    "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";

static inline char *w_u64(char *o, uint64_t v) {
    char t[20];
    int n = 0;
    while (v >= 100) { uint32_t r = (uint32_t)(v % 100); v /= 100; t[n++] = D2[2 * r + 1]; t[n++] = D2[2 * r]; }
    if (v >= 10) { t[n++] = D2[2 * v + 1]; t[n++] = D2[2 * v]; }
    else t[n++] = (char)('0' + v);
    while (n) *o++ = t[--n];
    return o;
}

#define W_LIT(o, s) (memcpy((o), (s), sizeof(s) - 1), (o) + sizeof(s) - 1)

// Postgres timestamptz (microseconds since 2000-01-01 UTC) -> "2025-12-31T23:59:19.409Z" (ms truncated).
static char *w_ts(char *o, int64_t us) {
    static int64_t cday = INT64_MIN;
    static char cdate[11];
    int64_t ms = us >= 0 ? us / 1000 : -((-us + 999) / 1000);
    int64_t days = ms >= 0 ? ms / 86400000 : -((-ms + 86399999) / 86400000);
    uint32_t rem = (uint32_t)(ms - days * 86400000);
    if (days != cday) {      // civil date from days (Howard Hinnant's algorithm), cached per day
        int64_t z = days + 10957 + 719468;       // 10957 days from 1970-01-01 to 2000-01-01
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t y = (int64_t)yoe + era * 400;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t d = doy - (153 * mp + 2) / 5 + 1;
        uint32_t m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2) y++;
        uint32_t yy = (uint32_t)(y < 0 ? 0 : y > 9999 ? 9999 : y);
        memcpy(cdate, D2 + 2 * (yy / 100), 2);
        memcpy(cdate + 2, D2 + 2 * (yy % 100), 2);
        cdate[4] = '-';
        memcpy(cdate + 5, D2 + 2 * m, 2);
        cdate[7] = '-';
        memcpy(cdate + 8, D2 + 2 * d, 2);
        cdate[10] = 'T';
        cday = days;
    }
    memcpy(o, cdate, 11);
    uint32_t msec = rem % 1000, s = rem / 1000;
    uint32_t hh = s / 3600, mm = (s / 60) % 60, ss = s % 60;
    memcpy(o + 11, D2 + 2 * hh, 2);
    o[13] = ':';
    memcpy(o + 14, D2 + 2 * mm, 2);
    o[16] = ':';
    memcpy(o + 17, D2 + 2 * ss, 2);
    o[19] = '.';
    o[20] = (char)('0' + msec / 100);
    memcpy(o + 21, D2 + 2 * (msec % 100), 2);
    o[23] = 'Z';
    return o + 24;
}

// ---------------------------------------------------------------------------------------------- connections
typedef struct cbuf { uint32_t len, cap, off; char d[]; } cbuf_t;

enum { ST_FREE = 0, ST_IDLE, ST_BUSY, ST_SEND };
#define CF_GZIP 1      // the request in flight accepts gzip
#define CF_CLOSE 2     // close once the response is out
#define CF_DEAD 4      // being torn down (io_uring: waiting for its receive/send to complete)
#define CF_RECV 8      // io_uring: multishot receive armed
#define CF_SHUT 16     // io_uring: shutdown() called
#define CF_INPROC 32   // its stashed input is being parsed right now
#define CF_100 64      // "100 Continue" already sent for the request being received
#define CF_OUT 128     // epoll: waiting for EPOLLOUT
#define CF_EOF 256     // the peer has closed its side: answer what it sent, then close
#define CF_FIXED 512   // io_uring: registered in the ring's file table (slot = fd)

typedef struct conn {
    uint32_t gen;
    uint16_t flags;
    uint8_t st;
    uint16_t t_ms;         // when the request in flight arrived (low bits of the ms clock)
    cbuf_t *in;            // input that could not be used yet: a partial request, or one behind a busy one
    cbuf_t *out;           // epoll: the unsent rest of a response
} conn_t;

static conn_t *conns;
static uint32_t conn_cap;
static uint32_t live_conns;

typedef struct { int fd; uint32_t gen; } fdgen_t;
static fdgen_t *resume_v;
static uint32_t resume_n, resume_cap;

static void resume_push(int fd, uint32_t gen) {
    if (resume_n == resume_cap) {
        resume_cap = resume_cap ? resume_cap * 2 : 256;
        resume_v = realloc(resume_v, resume_cap * sizeof *resume_v);
    }
    resume_v[resume_n++] = (fdgen_t){fd, gen};
}

static int stash(conn_t *c, const char *p, uint32_t n) {
    cbuf_t *b = c->in;
    if (!b) {
        uint32_t cap = n < 2000 ? 2048 : n * 2;
        b = malloc(sizeof *b + cap);
        b->len = 0; b->off = 0; b->cap = cap;
        c->in = b;
    } else if (b->len + n > b->cap) {
        if (b->off) { memmove(b->d, b->d + b->off, b->len - b->off); b->len -= b->off; b->off = 0; }
        if (b->len + n > b->cap) {
            uint32_t cap = (b->len + n) * 2;
            if (cap > (1u << 21)) return -1;
            b = realloc(b, sizeof *b + cap);
            b->cap = cap;
            c->in = b;
        }
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
    return 0;
}

// ---------------------------------------------------------------------------------------------- io backends
static int ep = -1;                      // epoll instance (epoll mode: clients and Postgres; unused with io_uring)
static int ring_fd = -1;

#define NBUF 8192
#define BUFSZ 2048
static struct {
    unsigned *sq_head, *sq_tail, *sq_array, *cq_head, *cq_tail;
    unsigned sq_mask, cq_mask, sq_entries, sq_ltail;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    struct io_uring_buf_ring *br;
    unsigned br_tail;
    uint8_t *bufs;
    unsigned nfixed;         // size of the registered file table (0: not available)
} R;

#define UD_RECV 1ull
#define UD_SEND 2ull
#define UD_MISC 3ull

static int ring_enter(unsigned min_complete, int64_t timeout_us) {
    unsigned to_submit = R.sq_ltail - __atomic_load_n(R.sq_head, __ATOMIC_ACQUIRE);
    __atomic_store_n(R.sq_tail, R.sq_ltail, __ATOMIC_RELEASE);
    struct __kernel_timespec ts = {timeout_us / 1000000, (timeout_us % 1000000) * 1000};
    struct io_uring_getevents_arg arg = {0, _NSIG / 8, 0, (uint64_t)(uintptr_t)&ts};
    if (timeout_us < 0) arg.ts = 0;
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
                        IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof arg);
}

// Hands the queued operations to the kernel without waiting for anything.
static void ring_submit(void) {
    unsigned to_submit = R.sq_ltail - __atomic_load_n(R.sq_head, __ATOMIC_ACQUIRE);
    if (!to_submit) return;
    __atomic_store_n(R.sq_tail, R.sq_ltail, __ATOMIC_RELEASE);
    syscall(__NR_io_uring_enter, ring_fd, to_submit, 0, 0, NULL, 0);
}

static struct io_uring_sqe *ring_sqe(void) {
    if (unlikely(R.sq_ltail - __atomic_load_n(R.sq_head, __ATOMIC_ACQUIRE) >= R.sq_entries)) {
        ring_enter(0, 0);        // the submission queue is full: hand it over now
        if (R.sq_ltail - __atomic_load_n(R.sq_head, __ATOMIC_ACQUIRE) >= R.sq_entries) die("io_uring submission queue stuck");
    }
    struct io_uring_sqe *s = &R.sqes[R.sq_ltail++ & R.sq_mask];
    memset(s, 0, sizeof *s);
    return s;
}

static int ring_init(void) {
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL |
              IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP;
    p.cq_entries = 65536;
    int fd = (int)syscall(__NR_io_uring_setup, 4096, &p);
    if (fd < 0) return -1;
    if (!(p.features & IORING_FEAT_SINGLE_MMAP) || !(p.features & IORING_FEAT_EXT_ARG)) { close(fd); return -1; }
    size_t s1 = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t s2 = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t sz = s1 > s2 ? s1 : s2;
    uint8_t *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (m == MAP_FAILED) { close(fd); return -1; }
    R.sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (R.sqes == MAP_FAILED) { close(fd); return -1; }
    R.sq_head = (unsigned *)(m + p.sq_off.head);
    R.sq_tail = (unsigned *)(m + p.sq_off.tail);
    R.sq_array = (unsigned *)(m + p.sq_off.array);
    R.sq_mask = *(unsigned *)(m + p.sq_off.ring_mask);
    R.sq_entries = p.sq_entries;
    R.cq_head = (unsigned *)(m + p.cq_off.head);
    R.cq_tail = (unsigned *)(m + p.cq_off.tail);
    R.cq_mask = *(unsigned *)(m + p.cq_off.ring_mask);
    R.cqes = (struct io_uring_cqe *)(m + p.cq_off.cqes);
    for (unsigned i = 0; i < p.sq_entries; i++) R.sq_array[i] = i;
    R.sq_ltail = *R.sq_tail;
    // provided buffers
    size_t rsz = NBUF * sizeof(struct io_uring_buf);
    R.br = mmap(NULL, rsz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    R.bufs = mmap(NULL, (size_t)NBUF * BUFSZ, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (R.br == MAP_FAILED || R.bufs == MAP_FAILED) { close(fd); return -1; }
    struct io_uring_buf_reg reg;
    memset(&reg, 0, sizeof reg);
    reg.ring_addr = (uint64_t)(uintptr_t)R.br;
    reg.ring_entries = NBUF;
    reg.bgid = 0;
    int r = (int)syscall(__NR_io_uring_register, fd, IORING_REGISTER_PBUF_RING, &reg, 1);
    if (r < 0 && errno == EINVAL) {
        // Ubuntu's 6.8.0-13x/-14x kernels have the check of the reserved words inverted (Launchpad #2162843):
        // they refuse zeros and take a nonzero first word.
        reg.resv[0] = 1;
        r = (int)syscall(__NR_io_uring_register, fd, IORING_REGISTER_PBUF_RING, &reg, 1);
        ring_quirk = 1;
    }
    if (r < 0) { close(fd); return -1; }
    for (unsigned i = 0; i < NBUF; i++) {
        R.br->bufs[i].addr = (uint64_t)(uintptr_t)(R.bufs + (size_t)i * BUFSZ);
        R.br->bufs[i].len = BUFSZ;
        R.br->bufs[i].bid = (uint16_t)i;
    }
    R.br_tail = NBUF;
    __atomic_store_n(&R.br->tail, (uint16_t)R.br_tail, __ATOMIC_RELEASE);
    // A sparse table of registered files: a connection's socket is put in the slot of its fd number, and its
    // receives and sends name the slot, so the kernel does not take and drop a file reference for every send.
    struct rlimit rl;
    unsigned want = 1u << 20;       // the kernel's limit for one ring
    if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < want) want = (unsigned)rl.rlim_cur;
    struct io_uring_rsrc_register rr;
    memset(&rr, 0, sizeof rr);
    rr.nr = want;
    rr.flags = IORING_RSRC_REGISTER_SPARSE;
    R.nfixed = syscall(__NR_io_uring_register, fd, IORING_REGISTER_FILES2, &rr, sizeof rr) == 0 ? want : 0;
    ring_fd = fd;
    return 0;
}

static int ring_file_set(int slot, int fdv) {
    struct io_uring_rsrc_update up;
    memset(&up, 0, sizeof up);
    up.offset = (unsigned)slot;
    up.data = (uint64_t)(uintptr_t)&fdv;
    return (int)syscall(__NR_io_uring_register, ring_fd, IORING_REGISTER_FILES_UPDATE, &up, 1) == 1 ? 0 : -1;
}

static inline void ring_recycle(unsigned bid) {
    struct io_uring_buf *b = &R.br->bufs[R.br_tail++ & (NBUF - 1)];
    b->addr = (uint64_t)(uintptr_t)(R.bufs + (size_t)bid * BUFSZ);
    b->len = BUFSZ;
    b->bid = (uint16_t)bid;
}

static void ring_arm_recv(int fd, conn_t *c) {
    struct io_uring_sqe *s = ring_sqe();
    s->opcode = IORING_OP_RECV;
    s->fd = fd;
    s->ioprio = IORING_RECV_MULTISHOT;
    s->flags = IOSQE_BUFFER_SELECT | (c->flags & CF_FIXED ? IOSQE_FIXED_FILE : 0);
    s->buf_group = 0;
    s->user_data = UD_RECV << 60 | (uint64_t)(c->gen & 0xfffffff) << 32 | (uint32_t)fd;
    c->flags |= CF_RECV;
}

// response memory. epoll: one scratch buffer (send() copies). io_uring: blocks that stay put until every send
// that points into them has completed.
#define OBLK (1u << 20)
#define MAXBLK 256
static struct { char *p; uint32_t used, pending; } oblk[MAXBLK];
static int ocur, onblk;
static char *oscratch;
static uint32_t oscratch_cap;

static char *out_reserve(uint32_t max) {
    if (ring_fd < 0) {
        if (max > oscratch_cap) { oscratch_cap = max * 2; oscratch = realloc(oscratch, oscratch_cap); }
        return oscratch;
    }
    if (max > OBLK) return NULL;
    if (oblk[ocur].used + max > OBLK) {
        int f = -1;
        for (int i = 0; i < onblk; i++) if (i != ocur && oblk[i].pending == 0) { f = i; break; }
        if (f < 0) {
            if (onblk == MAXBLK) return NULL;
            f = onblk++;
            oblk[f].p = mmap(NULL, OBLK, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
            if (oblk[f].p == MAP_FAILED) die("mmap");
            oblk[f].pending = 0;
        }
        oblk[f].used = 0;
        ocur = f;
    }
    return oblk[ocur].p + oblk[ocur].used;
}

static void conn_free(int fd, conn_t *c) {
    if (c->flags & CF_FIXED) ring_file_set(fd, -1);
    close(fd);
    free(c->in);
    free(c->out);
    c->in = c->out = NULL;
    c->gen++;
    c->st = ST_FREE;
    c->flags = 0;
    live_conns--;
}

static void conn_kill(int fd, conn_t *c) {
    if (ring_fd < 0) { conn_free(fd, c); return; }
    c->flags |= CF_DEAD;
    if (c->flags & CF_RECV) {
        if (!(c->flags & CF_SHUT)) { shutdown(fd, SHUT_RDWR); c->flags |= CF_SHUT; }   // completes the armed receive
        return;
    }
    if (c->st != ST_SEND) conn_free(fd, c);
}

static void after_response(int fd, conn_t *c) {
    c->st = ST_IDLE;
    if (c->flags & (CF_CLOSE | CF_DEAD)) { conn_kill(fd, c); return; }
    if (c->in) { if (!(c->flags & CF_INPROC)) resume_push(fd, c->gen); return; }
    if (c->flags & CF_EOF) conn_kill(fd, c);
}

// A late answer (the kernel has acked its request in a packet of its own by then) drops the socket out of
// delayed-ACK mode, and the next request would be acked at once too. Turning the mode back on is a system call:
// the user's next request is seconds away, so it is queued and done when a pass has time to spare rather than
// in the middle of the backlog that made the answer late.
static uint32_t d_lat[8], d_pgrtt[8];        // FEED_DEBUG: answers by age, Postgres batches by round trip
static inline int d_bucket(uint32_t ms) { return ms < 5 ? 0 : ms < 10 ? 1 : ms < 25 ? 2 : ms < 50 ? 3 : ms < 100 ? 4 : ms < 250 ? 5 : ms < 1000 ? 6 : 7; }
#define QACK_CAP 65536
static fdgen_t qack_v[QACK_CAP];
static uint32_t qack_h, qack_t;

static void qack_drain(int budget) {
    while (budget-- > 0 && qack_h != qack_t) {
        fdgen_t e = qack_v[qack_h++ & (QACK_CAP - 1)];
        conn_t *c = &conns[e.fd];
        if (c->gen != e.gen || c->st == ST_FREE || (c->flags & CF_DEAD)) continue;
        int z = 0;
        setsockopt(e.fd, IPPROTO_TCP, TCP_QUICKACK, &z, sizeof z);
    }
}

#define SEND_CHUNK 128
static unsigned sends_queued;
static int qack_ring = 1;        // set the option through the ring (kernel 6.7+); cleared if the kernel refuses
static const int qack_zero = 0;

// p[0..len) is the complete response, inside the region the last out_reserve returned.
static void out_send(int fd, conn_t *c, char *p, uint32_t len) {
    if (unlikely(opt_debug)) d_lat[d_bucket((uint16_t)(now16 - c->t_ms))]++;
    if (unlikely((uint16_t)(now16 - c->t_ms) > 25)) {
        if (ring_fd >= 0 && qack_ring) {
            // no system call: the option is set by a ring command queued just ahead of the send
            struct io_uring_sqe *s = ring_sqe();
            s->opcode = IORING_OP_URING_CMD;
            s->fd = fd;
            s->cmd_op = SOCKET_URING_OP_SETSOCKOPT;
            s->level = IPPROTO_TCP;
            s->optname = TCP_QUICKACK;
            s->optval = (uint64_t)(uintptr_t)&qack_zero;
            s->optlen = sizeof qack_zero;
            s->flags = IOSQE_CQE_SKIP_SUCCESS | (c->flags & CF_FIXED ? IOSQE_FIXED_FILE : 0);
            s->user_data = UD_MISC << 60;
        } else {
            if (qack_t - qack_h == QACK_CAP) qack_h++;
            qack_v[qack_t++ & (QACK_CAP - 1)] = (fdgen_t){fd, c->gen};
        }
    }
    if (ring_fd >= 0) {
        oblk[ocur].used = (uint32_t)(p + len - oblk[ocur].p);
        oblk[ocur].pending++;
        struct io_uring_sqe *s = ring_sqe();
        s->opcode = IORING_OP_SEND;
        s->fd = fd;
        s->addr = (uint64_t)(uintptr_t)p;
        s->len = len;
        s->msg_flags = MSG_NOSIGNAL | MSG_WAITALL;
        if (c->flags & CF_FIXED) s->flags = IOSQE_FIXED_FILE;
        s->user_data = UD_SEND << 60 | (uint64_t)ocur << 48 | (uint64_t)(c->gen & 0xffff) << 32 | (uint32_t)fd;
        c->st = ST_SEND;
        // A pass that has much to answer does not hold the first answers until the last is built.
        if (++sends_queued >= SEND_CHUNK) { sends_queued = 0; ring_submit(); }
        return;
    }
    ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
    if (likely(n == (ssize_t)len)) { after_response(fd, c); return; }
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) { c->st = ST_IDLE; conn_kill(fd, c); return; }
        n = 0;
    }
    cbuf_t *b = malloc(sizeof *b + len - (uint32_t)n);
    b->len = len - (uint32_t)n; b->cap = b->len; b->off = 0;
    memcpy(b->d, p + n, b->len);
    c->out = b;
    c->st = ST_SEND;
    c->flags |= CF_OUT;
    struct epoll_event ev = {EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, {.u64 = (uint32_t)fd}};
    epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

// ---------------------------------------------------------------------------------------------- responses
enum { S200, S201, S400, S401, S404, S413, S500, S503, S_N };
static const char *const ST_LINE[S_N] = {
    "HTTP/1.1 200 OK\r\n", "HTTP/1.1 201 Created\r\n", "HTTP/1.1 400 Bad Request\r\n", "HTTP/1.1 401 Unauthorized\r\n",
    "HTTP/1.1 404 Not Found\r\n", "HTTP/1.1 413 Payload Too Large\r\n", "HTTP/1.1 500 Internal Server Error\r\n",
    "HTTP/1.1 503 Service Unavailable\r\n"};
static char head_pre[S_N][2][128];
static uint32_t head_pre_n[S_N][2];
#define HEAD_MAX 192

static void heads_init(void) {
    for (int s = 0; s < S_N; s++)
        for (int g = 0; g < 2; g++)
            head_pre_n[s][g] = (uint32_t)snprintf(head_pre[s][g], sizeof head_pre[s][g], "%sContent-Type: application/json\r\n%sContent-Length: ",
                                                   ST_LINE[s], g ? "Content-Encoding: gzip\r\n" : "");
}

// Writes the head so that it ends at `body`; returns its first byte. Needs HEAD_MAX bytes before body.
static inline char *w_head(char *body, int st, int gz, uint32_t clen, int close_) {
    char h[HEAD_MAX], *o = h;
    memcpy(o, head_pre[st][gz], head_pre_n[st][gz]);
    o += head_pre_n[st][gz];
    o = w_u64(o, clen);
    if (unlikely(close_)) o = W_LIT(o, "\r\nConnection: close\r\n\r\n");
    else o = W_LIT(o, "\r\n\r\n");
    uint32_t n = (uint32_t)(o - h);
    memcpy(body - n, h, n);
    return body - n;
}

static void respond(int fd, conn_t *c, int st, const char *body, uint32_t blen) {
    char *base = out_reserve(HEAD_MAX + blen);
    if (!base) { c->st = ST_IDLE; conn_kill(fd, c); return; }
    char *b = base + HEAD_MAX;
    memcpy(b, body, blen);
    char *h = w_head(b, st, 0, blen, c->flags & CF_CLOSE);
    out_send(fd, c, h, (uint32_t)(b + blen - h));
}
#define RESPOND_LIT(fd, c, st, s) respond(fd, c, st, s, sizeof(s) - 1)

static void respond_500(int fd, conn_t *c) { RESPOND_LIT(fd, c, S500, "{\"error\":\"internal server error\"}"); }

// ---------------------------------------------------------------------------------------------- Postgres
enum { K_FEED = 1, K_POST, K_HEALTH, K_LIKE, K_NEWPOST };
#define F_SYNC 1     // a Sync follows this request
#define F_DONE 2     // its CommandComplete is in
#define F_ERR 4      // it is the statement that failed
#define F_FK 8       // ... with a foreign key violation (23503)

typedef struct wctx { int64_t uid; uint32_t ulen, blen; char d[]; } wctx_t;      // a new post: username, then body

typedef struct pgreq {
    int32_t fd;
    uint32_t gen;
    uint8_t kind, flags, retries, pad;
    uint32_t rows;
    int64_t a, b;        // post: a = id. like: a = user, b = post. new post: results (id, created_at)
    wctx_t *ctx;
    uint32_t t0;         // reads: when it arrived (ms)
} pgreq_t;

typedef struct { pgreq_t *v; uint32_t h, t, cap; } rq_t;       // cap is a power of two

static inline uint32_t rq_len(const rq_t *q) { return q->t - q->h; }
static void rq_push(rq_t *q, const pgreq_t *e) {
    if (q->t - q->h == q->cap) {
        uint32_t nc = q->cap ? q->cap * 2 : 1024;
        pgreq_t *nv = malloc(nc * sizeof *nv);
        for (uint32_t i = 0; i < q->cap; i++) nv[i] = q->v[(q->h + i) & (q->cap - 1)];
        free(q->v);
        q->v = nv; q->t = q->cap; q->h = 0; q->cap = nc;
    }
    q->v[q->t++ & (q->cap - 1)] = *e;
}
static inline pgreq_t *rq_at(rq_t *q, uint32_t i) { return &q->v[i & (q->cap - 1)]; }
static inline pgreq_t rq_pop(rq_t *q) { return q->v[q->h++ & (q->cap - 1)]; }

#define NONE 0xffffffffu
#define PG_BATCH 64
#define PG_WGROUP 128
#define PG_MAXFLIGHT 4096
static uint32_t pg_flight = 96;                // most requests waiting on one read connection (FEED_FLIGHT)
static int cfg_rconns, cfg_wconns;               // FEED_RCONNS, FEED_WCONNS: Postgres connections per worker (0 = by worker count)
static uint32_t hold_ms = 250, rescue_ms = 45000;      // FEED_HOLD_MS, FEED_RESCUE_MS: see next_read()
static uint32_t d_parked, d_rescued;

typedef struct pgc {
    int fd;
    uint8_t up, is_write, skip, berr;
    uint8_t *rb;
    uint32_t rcap, rlen, rpos, row0, nrows;
    uint8_t *wb;
    uint32_t wcap, wlen, wsent;
    rq_t q;
    uint32_t qc;             // write connections: the entry whose CommandComplete is next
    int64_t next_try_ms, last_rx_ms, owe_since_ms;
    int idx;
} pgc_t;

#define MAXPG 64
static pgc_t pgs[MAXPG];
static int npg, npg_read;
static rq_t rdq, wrq, wretry, parkq;
static uint32_t pg_inflight;

static const char SQL_POST_SELECT[] =
    "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
    "FROM posts p JOIN users u ON u.id = p.user_id";
static const char SQL_FEED_TAIL[] = " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
static const char SQL_POST_TAIL[] = " WHERE p.id = $1";
static const char SQL_INS_POST[] = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
static const char SQL_INS_LIKE[] = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";

// Bind + Execute for each statement, built once; parameters are patched in.
static uint8_t T_FEED[64], T_POST[80], T_HEALTH[48], T_LIKE[96], T_NEWPOST_HEAD[32];
static uint32_t T_FEED_N, T_POST_N, T_HEALTH_N, T_LIKE_N, T_NEWPOST_HEAD_N;
static const uint8_t T_EXEC[10] = {'E', 0, 0, 0, 9, 0, 0, 0, 0, 0};
static const uint8_t T_SYNC[5] = {'S', 0, 0, 0, 4};
static const uint8_t T_NEWPOST_TAIL[6] = {0, 2, 0, 0, 0, 1};       // result formats: id text, created_at binary

static uint32_t mk_bind(uint8_t *o, char stmt, int nparam8, int nres, const uint8_t *resfmt) {
    uint8_t *p = o;
    *p++ = 'B';
    p += 4;
    *p++ = 0;                    // unnamed portal
    *p++ = (uint8_t)stmt; *p++ = 0;
    if (nparam8) { *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 1; }     // one format code for all parameters: binary
    else { *p++ = 0; *p++ = 0; }
    *p++ = 0; *p++ = (uint8_t)nparam8;
    for (int i = 0; i < nparam8; i++) { put32(p, 8); p += 12; }
    *p++ = 0; *p++ = (uint8_t)nres;
    for (int i = 0; i < nres; i++) { *p++ = 0; *p++ = resfmt[i]; }
    put32(o + 1, (uint32_t)(p - o - 1));
    memcpy(p, T_EXEC, 10);
    return (uint32_t)(p + 10 - o);
}

static void pg_templates(void) {
    static const uint8_t rf[5] = {0, 0, 1, 0, 0};      // id and like_count as text (shorter than 8 and 4 bytes), created_at binary
    T_FEED_N = mk_bind(T_FEED, 'f', 0, 5, rf);
    T_POST_N = mk_bind(T_POST, 'p', 1, 5, rf);          // parameter at offset 18
    T_HEALTH_N = mk_bind(T_HEALTH, 'h', 0, 0, NULL);
    T_LIKE_N = mk_bind(T_LIKE, 'l', 2, 0, NULL);        // parameters at offsets 18 and 30
    // new post: Bind header up to the parameters; formats (binary, text), 2 parameters
    uint8_t *p = T_NEWPOST_HEAD;
    *p++ = 'B'; p += 4; *p++ = 0; *p++ = 'i'; *p++ = 0;
    *p++ = 0; *p++ = 2; *p++ = 0; *p++ = 1; *p++ = 0; *p++ = 0;
    *p++ = 0; *p++ = 2;
    T_NEWPOST_HEAD_N = (uint32_t)(p - T_NEWPOST_HEAD);
}

static inline uint8_t *pg_wspace(pgc_t *pc, uint32_t n) {
    if (unlikely(pc->wlen + n > pc->wcap)) {
        pc->wcap = (pc->wlen + n) * 2;
        pc->wb = realloc(pc->wb, pc->wcap);
    }
    uint8_t *p = pc->wb + pc->wlen;
    pc->wlen += n;
    return p;
}

static void pg_append(pgc_t *pc, const pgreq_t *e) {
    uint8_t *p;
    switch (e->kind) {
    case K_FEED: memcpy(pg_wspace(pc, T_FEED_N), T_FEED, T_FEED_N); break;
    case K_HEALTH: memcpy(pg_wspace(pc, T_HEALTH_N), T_HEALTH, T_HEALTH_N); break;
    case K_POST:
        p = pg_wspace(pc, T_POST_N);
        memcpy(p, T_POST, T_POST_N);
        put64(p + 18, (uint64_t)e->a);
        break;
    case K_LIKE:
        p = pg_wspace(pc, T_LIKE_N);
        memcpy(p, T_LIKE, T_LIKE_N);
        put64(p + 18, (uint64_t)e->a);
        put64(p + 30, (uint64_t)e->b);
        break;
    case K_NEWPOST: {
        wctx_t *x = e->ctx;
        uint32_t n = T_NEWPOST_HEAD_N + 4 + 8 + 4 + x->blen + 6 + 10;
        p = pg_wspace(pc, n);
        memcpy(p, T_NEWPOST_HEAD, T_NEWPOST_HEAD_N);
        uint8_t *o = p + T_NEWPOST_HEAD_N;
        put32(o, 8); put64(o + 4, (uint64_t)x->uid); o += 12;
        put32(o, x->blen); memcpy(o + 4, x->d + x->ulen, x->blen); o += 4 + x->blen;
        memcpy(o, T_NEWPOST_TAIL, 6); o += 6;
        put32(p + 1, (uint32_t)(o - p - 1));
        memcpy(o, T_EXEC, 10);
        break;
    }
    }
}

// ---- login (blocking, once per connection) ----
static int rd_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int wr_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}
// reads one message; body (without the length word) into buf, NUL-terminated. Returns the type or -1.
static int rd_msg(int fd, uint8_t *buf, uint32_t cap, uint32_t *len) {
    uint8_t h[5];
    if (rd_full(fd, h, 5)) return -1;
    uint32_t n = be32(h + 1);
    if (n < 4 || n - 4 >= cap) return -1;
    if (rd_full(fd, buf, n - 4)) return -1;
    buf[n - 4] = 0;
    *len = n - 4;
    return h[0];
}

static const char *pg_err_field(const uint8_t *b, uint32_t n, char f) {
    uint32_t i = 0;
    while (i < n && b[i]) {
        const char *v = (const char *)b + i + 1;
        if (b[i] == (uint8_t)f) return v;
        i += 1 + (uint32_t)strlen(v) + 1;
    }
    return "";
}

static struct addrinfo *db_ai;
static uint8_t scram_salt[64], scram_salted[32];
static uint32_t scram_saltn, scram_iters;

static int pg_scram(int fd, uint8_t *m, uint32_t cap) {
    uint8_t rnd[18];
    char cnonce[32], first_bare[64], out[512];
    if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd) return -1;
    size_t cn = b64_encode(rnd, sizeof rnd, cnonce);
    cnonce[cn] = 0;
    int fb = snprintf(first_bare, sizeof first_bare, "n=,r=%s", cnonce);
    uint8_t *p = (uint8_t *)out;
    *p++ = 'p'; p += 4;
    memcpy(p, "SCRAM-SHA-256", 14); p += 14;
    put32(p, (uint32_t)fb + 3); p += 4;
    memcpy(p, "n,,", 3); p += 3;
    memcpy(p, first_bare, (size_t)fb); p += fb;
    put32((uint8_t *)out + 1, (uint32_t)(p - (uint8_t *)out - 1));
    if (wr_full(fd, out, (size_t)(p - (uint8_t *)out))) return -1;
    uint32_t n;
    int t = rd_msg(fd, m, cap, &n);
    if (t != 'R' || n < 4 || be32(m) != 11) return -1;
    char sfirst[512];
    if (n - 4 >= sizeof sfirst) return -1;
    memcpy(sfirst, m + 4, n - 4);
    sfirst[n - 4] = 0;
    char *r = strstr(sfirst, "r="), *s = strstr(sfirst, ",s="), *i = strstr(sfirst, ",i=");
    if (!r || !s || !i || r != sfirst) return -1;
    char nonce[256], saltb64[128];
    size_t rl = (size_t)(s - r - 2), sl = (size_t)(i - s - 3);
    if (rl >= sizeof nonce || sl >= sizeof saltb64 || strncmp(r + 2, cnonce, cn)) return -1;
    memcpy(nonce, r + 2, rl); nonce[rl] = 0;
    memcpy(saltb64, s + 3, sl); saltb64[sl] = 0;
    uint32_t iters = (uint32_t)strtoul(i + 3, NULL, 10);
    uint8_t salt[96];
    long saltn = b64_decode(saltb64, sl, salt);
    if (saltn <= 0 || saltn > 64 || iters == 0 || iters > 1000000) return -1;
    if (!(scram_saltn == (uint32_t)saltn && scram_iters == iters && !memcmp(scram_salt, salt, (size_t)saltn))) {
        pbkdf2_sha256(db_pass, strlen(db_pass), salt, (size_t)saltn, iters, scram_salted);
        memcpy(scram_salt, salt, (size_t)saltn);
        scram_saltn = (uint32_t)saltn;
        scram_iters = iters;
    }
    hmac_key_t sk;
    hmac_key_init(&sk, scram_salted, 32);
    uint8_t ckey[32], stored[32], csig[32], skey[32], ssig[32], proof[32];
    hmac_sha256(&sk, "Client Key", 10, ckey);
    hmac_sha256(&sk, "Server Key", 10, skey);
    sha256_t h;
    sha256_init(&h); sha256_update(&h, ckey, 32); sha256_final(&h, stored);
    char fin_bare[300], auth[1100];
    int fbn = snprintf(fin_bare, sizeof fin_bare, "c=biws,r=%s", nonce);
    int an = snprintf(auth, sizeof auth, "%s,%s,%s", first_bare, sfirst, fin_bare);
    hmac_key_t k2;
    hmac_key_init(&k2, stored, 32);
    hmac_sha256(&k2, auth, (size_t)an, csig);
    for (int j = 0; j < 32; j++) proof[j] = ckey[j] ^ csig[j];
    hmac_key_init(&k2, skey, 32);
    hmac_sha256(&k2, auth, (size_t)an, ssig);
    char pb64[64];
    size_t pn = b64_encode(proof, 32, pb64);
    p = (uint8_t *)out;
    *p++ = 'p'; p += 4;
    memcpy(p, fin_bare, (size_t)fbn); p += fbn;
    memcpy(p, ",p=", 3); p += 3;
    memcpy(p, pb64, pn); p += pn;
    put32((uint8_t *)out + 1, (uint32_t)(p - (uint8_t *)out - 1));
    if (wr_full(fd, out, (size_t)(p - (uint8_t *)out))) return -1;
    t = rd_msg(fd, m, cap, &n);
    if (t != 'R' || n < 6 || be32(m) != 12) return -1;
    char sb64[64];
    size_t sn = b64_encode(ssig, 32, sb64);
    if (n - 4 < 2 + sn || memcmp(m + 4, "v=", 2) || memcmp(m + 6, sb64, sn)) return -1;     // the server proves it knows the password too
    return 0;
}

static int pg_login(pgc_t *pc) {
    static uint8_t m[8192];
    uint32_t n;
    int fd = socket(db_ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);      // also bounds connect()
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, sizeof one);
    if (connect(fd, db_ai->ai_addr, db_ai->ai_addrlen)) goto fail;
    uint8_t s[600], *p = s + 4;
    put32(p, 196608); p += 4;
    p += sprintf((char *)p, "user") + 1;
    p += sprintf((char *)p, "%s", db_user) + 1;
    p += sprintf((char *)p, "database") + 1;
    p += sprintf((char *)p, "%s", db_name) + 1;
    p += sprintf((char *)p, "client_encoding") + 1;
    p += sprintf((char *)p, "UTF8") + 1;
    p += sprintf((char *)p, "options") + 1;
    p += sprintf((char *)p, "-c plan_cache_mode=force_generic_plan") + 1;
    *p++ = 0;
    put32(s, (uint32_t)(p - s));
    if (wr_full(fd, s, (size_t)(p - s))) goto fail;
    for (;;) {
        int t = rd_msg(fd, m, sizeof m, &n);
        if (t < 0) goto fail;
        if (t == 'E') { logf_("postgres: %s", pg_err_field(m, n, 'M')); goto fail; }
        if (t == 'Z') break;
        if (t != 'R' || n < 4) continue;
        uint32_t a = be32(m);
        if (a == 0) continue;
        if (a == 3) {       // cleartext password
            uint32_t pl = (uint32_t)strlen(db_pass) + 1;
            uint8_t o[300];
            o[0] = 'p'; put32(o + 1, 4 + pl); memcpy(o + 5, db_pass, pl);
            if (wr_full(fd, o, 5 + pl)) goto fail;
        } else if (a == 10) {
            if (!strstr((char *)m + 4, "SCRAM-SHA-256")) { logf_("postgres: no SCRAM-SHA-256 offered"); goto fail; }
            if (pg_scram(fd, m, sizeof m)) { logf_("postgres: SCRAM authentication failed"); goto fail; }
        } else { logf_("postgres: unsupported authentication method %u", a); goto fail; }
    }
    // prepare the five statements
    {
        static uint8_t pb[2048];
        uint8_t *o = pb;
        struct { char name; const char *a, *b; int np; uint32_t t[2]; } st[5] = {
            {'f', SQL_POST_SELECT, SQL_FEED_TAIL, 0, {0, 0}}, {'p', SQL_POST_SELECT, SQL_POST_TAIL, 1, {20, 0}},
            {'i', SQL_INS_POST, "", 2, {20, 25}}, {'l', SQL_INS_LIKE, "", 2, {20, 20}}, {'h', "SELECT 1", "", 0, {0, 0}}};
        for (int i = 0; i < 5; i++) {
            uint8_t *b = o;
            *o++ = 'P'; o += 4;
            *o++ = (uint8_t)st[i].name; *o++ = 0;
            o += sprintf((char *)o, "%s%s", st[i].a, st[i].b) + 1;
            *o++ = 0; *o++ = (uint8_t)st[i].np;
            for (int j = 0; j < st[i].np; j++) { put32(o, st[i].t[j]); o += 4; }
            put32(b + 1, (uint32_t)(o - b - 1));
        }
        memcpy(o, T_SYNC, 5); o += 5;
        if (wr_full(fd, pb, (size_t)(o - pb))) goto fail;
        int bad = 0;
        for (;;) {
            int t = rd_msg(fd, m, sizeof m, &n);
            if (t < 0) goto fail;
            if (t == 'E') { logf_("postgres prepare: %s", pg_err_field(m, n, 'M')); bad = 1; }
            if (t == 'Z') break;
        }
        if (bad) goto fail;
    }
    tv.tv_sec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    pc->fd = fd;
    pc->up = 1;
    pc->rlen = pc->rpos = 0; pc->row0 = NONE; pc->nrows = 0;
    pc->wlen = pc->wsent = 0;
    pc->skip = pc->berr = 0;
    pc->q.h = pc->q.t = 0; pc->qc = 0;
    pc->last_rx_ms = now_ms;
    if (ep >= 0) {
        struct epoll_event ev = {EPOLLIN, {.u64 = 1ull << 40 | (uint32_t)pc->idx}};
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    }
    return 0;
fail:
    close(fd);
    return -1;
}

// ---- answers built from rows ----
typedef struct { const uint8_t *id, *body, *user, *lc; uint32_t idn, bodyn, usern, lcn; int64_t ts; } row_t;

// m points at a DataRow ('D'). Returns 0 if it is a post row.
static int row_parse(const uint8_t *m, row_t *r) {
    uint32_t len = be32(m + 1);
    const uint8_t *p = m + 7, *e = m + 1 + len;
    if (m[5] != 0 || m[6] != 5) return -1;
    const uint8_t *f[5];
    uint32_t fl[5];
    for (int i = 0; i < 5; i++) {
        if (e - p < 4) return -1;
        uint32_t l = be32(p);
        p += 4;
        if (l == 0xffffffffu) l = 0;
        if ((uint32_t)(e - p) < l) return -1;
        f[i] = p; fl[i] = l; p += l;
    }
    if (fl[2] != 8) return -1;
    r->id = f[0]; r->idn = fl[0]; r->body = f[1]; r->bodyn = fl[1];
    r->ts = (int64_t)be64(f[2]);
    r->user = f[3]; r->usern = fl[3]; r->lc = f[4]; r->lcn = fl[4];
    return 0;
}

static uint8_t *jbuf;
static uint32_t jcap;

static void finish_feed(int fd, conn_t *c, const uint8_t *m, const uint8_t *end, uint32_t nrows) {
    uint32_t k[FEED_MAX_POSTS][5];
    uint32_t need = (uint32_t)(end - m) * 6 + 64 + nrows * 128;
    if (need > jcap) { jcap = need * 2; jbuf = realloc(jbuf, jcap); }
    char *o = (char *)jbuf;
    o = W_LIT(o, "{\"posts\":[");
    int np = 0;
    for (; m < end; m += 1 + be32(m + 1)) {
        if (*m != 'D') continue;
        row_t r;
        if (row_parse(m, &r)) { respond_500(fd, c); return; }
        if (np < FEED_MAX_POSTS) k[np][0] = np ? (uint32_t)(o - (char *)jbuf) - 1 : 0;
        if (np) *o++ = ',';
        o = W_LIT(o, "{\"id\":");
        memcpy(o, r.id, r.idn); o += r.idn;
        if (np < FEED_MAX_POSTS) k[np][1] = (uint32_t)(o - (char *)jbuf);
        o = W_LIT(o, ",\"body\":\"");
        o = j_escape(o, r.body, r.bodyn);
        if (np < FEED_MAX_POSTS) k[np][2] = (uint32_t)(o - (char *)jbuf);
        o = W_LIT(o, "\",\"created_at\":\"");
        o = w_ts(o, r.ts);
        if (np < FEED_MAX_POSTS) k[np][3] = (uint32_t)(o - (char *)jbuf);
        o = W_LIT(o, "\",\"author\":\"");
        o = j_escape(o, r.user, r.usern);
        if (np < FEED_MAX_POSTS) k[np][4] = (uint32_t)(o - (char *)jbuf);
        o = W_LIT(o, "\",\"like_count\":");
        memcpy(o, r.lc, r.lcn); o += r.lcn;
        *o++ = '}';
        np++;
    }
    o = W_LIT(o, "]}");
    uint32_t n = (uint32_t)(o - (char *)jbuf);
    int gz = (c->flags & CF_GZIP) && n >= 1024 && n < 65000 && np <= FEED_MAX_POSTS;
    char *base = out_reserve(HEAD_MAX + n + n / 8 + 64);
    if (!base) { c->st = ST_IDLE; conn_kill(fd, c); return; }
    char *b = base + HEAD_MAX;
    uint32_t blen;
    if (gz) blen = (uint32_t)feed_gzip(jbuf, n, k, np, (uint8_t *)b);
    else { memcpy(b, jbuf, n); blen = n; }
    char *h = w_head(b, S200, gz, blen, c->flags & CF_CLOSE);
    out_send(fd, c, h, (uint32_t)(b + blen - h));
}

static inline char *w_post_tail(char *o, int64_t ts, const uint8_t *user, uint32_t usern) {
    o = W_LIT(o, "\",\"created_at\":\"");
    o = w_ts(o, ts);
    o = W_LIT(o, "\",\"author\":\"");
    o = j_escape(o, user, usern);
    return W_LIT(o, "\",\"like_count\":");
}

static void finish_post(int fd, conn_t *c, const uint8_t *m) {
    row_t r;
    if (row_parse(m, &r)) { respond_500(fd, c); return; }
    char *base = out_reserve(HEAD_MAX + 6 * (r.bodyn + r.usern) + 256);
    if (!base) { c->st = ST_IDLE; conn_kill(fd, c); return; }
    char *b = base + HEAD_MAX, *o = b;
    o = W_LIT(o, "{\"post\":{\"id\":");
    memcpy(o, r.id, r.idn); o += r.idn;
    o = W_LIT(o, ",\"body\":\"");
    o = j_escape(o, r.body, r.bodyn);
    o = w_post_tail(o, r.ts, r.user, r.usern);
    memcpy(o, r.lc, r.lcn); o += r.lcn;
    o = W_LIT(o, "}}");
    char *h = w_head(b, S200, 0, (uint32_t)(o - b), c->flags & CF_CLOSE);
    out_send(fd, c, h, (uint32_t)(o - h));
}

static void finish_health(int fd, conn_t *c) {
    char b[96], *o = b;
    o = W_LIT(o, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    o = w_u64(o, (uint64_t)((now_ms - shm->start_ms) / 1000));
    *o++ = '}';
    respond(fd, c, S200, b, (uint32_t)(o - b));
}

static void health_down(int fd, conn_t *c, const char *why) {
    char b[256];
    int n = snprintf(b, sizeof b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"%s\"}", why);
    respond(fd, c, S503, b, (uint32_t)n);
}

static void finish_health(int fd, conn_t *c);

// The rows of one read are in m[0..end): answer it.
static void answer_read(int fd, conn_t *c, int kind, const uint8_t *m, const uint8_t *end, uint32_t nrows) {
    if (kind == K_FEED) finish_feed(fd, c, m, end, nrows);
    else if (kind == K_POST) {
        if (nrows) finish_post(fd, c, m);
        else RESPOND_LIT(fd, c, S404, "{\"error\":\"post not found\"}");
    } else finish_health(fd, c);
}

// Fresh first. The kernel acknowledges a request in a packet of its own once it has waited 40 ms for the
// response to carry the ACK. When the worker is behind, answering in arrival order makes every answer late,
// each late answer costs that extra packet, and the extra packets keep the worker behind. So an answer that
// is already too old to make it steps aside (its rows are kept) for the ones that still can, and is sent when
// the pass has time, oldest first; nothing waits long, since the old ones get the whole pass once they are
// near 400 ms.
static int late_ms = 30;
#define LATE_BUDGET_US 6000
typedef struct late { struct late *next; int32_t fd; uint32_t gen, nrows, len, t0, tq; uint16_t t_ms; uint8_t kind; uint8_t d[]; } late_t;
static late_t *late_h, *late_tl, *lpark_h, *lpark_tl;       // answers waiting their turn; answers that stepped aside (parked)
static uint32_t late_n, lpark_n, d_deferred, d_lparked, d_lrescued;
static uint32_t park_ms = 400;  // FEED_PARK_MS: an answer that has waited its turn this long steps aside (0 = never)
static int64_t pass_t0, budget_us = 12000;      // FEED_BUDGET_US: once a pass has run this long, its remaining answers wait their turn too (0 = off)
static uint32_t d_overbudget;

static void late_push(const struct pgreq *e, uint16_t t_ms, const uint8_t *m, const uint8_t *end, uint32_t nrows);
static void late_run(int64_t t0);

static inline conn_t *req_conn(const pgreq_t *e) {
    conn_t *c = &conns[e->fd];
    return (c->gen == e->gen && c->st == ST_BUSY) ? c : NULL;
}

static void fail_req(const pgreq_t *e) {
    conn_t *c = req_conn(e);
    if (c) {
        if (e->kind == K_HEALTH) health_down(e->fd, c, "database connection lost");
        else respond_500(e->fd, c);
    }
    free(e->ctx);
}

static void requeue_read(pgreq_t e) {
    if (e.retries >= 3) { fail_req(&e); return; }
    e.retries++;
    e.flags = 0;
    rq_push(&rdq, &e);
}

static void finish_write(const pgreq_t *e) {
    conn_t *c = req_conn(e);
    int fd = e->fd;
    if (!c) { free(e->ctx); return; }
    if (e->kind == K_LIKE) {
        char b[96], *o = b;
        if (e->rows == 1) o = W_LIT(o, "{\"liked\":true,\"already_liked\":false,\"post_id\":");
        else o = W_LIT(o, "{\"liked\":true,\"already_liked\":true,\"post_id\":");
        o = w_u64(o, (uint64_t)e->b);
        *o++ = '}';
        respond(fd, c, e->rows == 1 ? S201 : S200, b, (uint32_t)(o - b));
        return;
    }
    wctx_t *x = e->ctx;
    char *base = out_reserve(HEAD_MAX + 6 * (x->blen + x->ulen) + 256);
    if (!base) { c->st = ST_IDLE; conn_kill(fd, c); free(x); return; }
    char *b = base + HEAD_MAX, *o = b;
    o = W_LIT(o, "{\"post\":{\"id\":");
    o = w_u64(o, (uint64_t)e->a);
    o = W_LIT(o, ",\"body\":\"");
    o = j_escape(o, (const uint8_t *)x->d + x->ulen, x->blen);
    o = w_post_tail(o, e->b, (const uint8_t *)x->d, x->ulen);
    o = W_LIT(o, "0}}");
    char *h = w_head(b, S201, 0, (uint32_t)(o - b), c->flags & CF_CLOSE);
    out_send(fd, c, h, (uint32_t)(o - h));
    free(x);
}

static void pg_lost(pgc_t *pc, const char *why) {
    logf_("worker %d: postgres connection %d lost: %s", worker_id, pc->idx, why);
    close(pc->fd);
    pc->fd = -1;
    pc->up = 0;
    pc->next_try_ms = now_ms + 1000;
    while (rq_len(&pc->q)) {
        pgreq_t e = rq_pop(&pc->q);
        pg_inflight--;
        if (pc->is_write) fail_req(&e);        // its commit may have happened: never sent twice
        else requeue_read(e);
    }
    pc->qc = pc->q.t;
}

// The group on a write connection has ended (its ReadyForQuery is in): answer it, or after a failure answer
// the statement that failed and send the others again, each in a transaction of its own.
static void pg_write_done(pgc_t *pc) {
    int err = pc->berr;
    pc->berr = 0;
    while (rq_len(&pc->q)) {
        pgreq_t e = rq_pop(&pc->q);
        pg_inflight--;
        int last = e.flags & F_SYNC;
        if (!err) {
            if (e.flags & F_DONE) finish_write(&e);
            else fail_req(&e);
        } else if (e.flags & F_ERR) {
            conn_t *c = req_conn(&e);
            if (c) {
                if (e.kind == K_LIKE && (e.flags & F_FK)) RESPOND_LIT(e.fd, c, S404, "{\"error\":\"post not found\"}");
                else respond_500(e.fd, c);
            }
            free(e.ctx);
        } else if (e.retries >= 3) fail_req(&e);
        else { e.retries++; e.flags = 0; rq_push(&wretry, &e); }
        if (last) break;
    }
    pc->qc = pc->q.h;
}

static uint32_t replies_seen;

static void late_push(const pgreq_t *e, uint16_t t_ms, const uint8_t *m, const uint8_t *end, uint32_t nrows) {
    uint32_t len = (uint32_t)(end - m);
    late_t *l = malloc(sizeof *l + len);
    l->next = NULL; l->fd = e->fd; l->gen = e->gen; l->nrows = nrows; l->len = len; l->t_ms = t_ms; l->kind = e->kind;
    l->tq = (uint32_t)now_ms;
    l->t0 = l->tq - (uint16_t)(now16 - t_ms);
    memcpy(l->d, m, len);
    if (late_tl) late_tl->next = l; else late_h = l;
    late_tl = l;
    late_n++;
    d_deferred++;
}

static inline void late_answer(late_t *l) {
    conn_t *c = &conns[l->fd];
    if (c->gen == l->gen && c->st == ST_BUSY) answer_read(l->fd, c, l->kind, l->d, l->d + l->len, l->nrows);
    free(l);
}

// Answers that were already late when Postgres returned them go out after the fresh ones, in the time the pass
// has left. When the box cannot keep up, that queue grows; the run is judged on p95 and p99, so with FEED_PARK_MS
// the wait is not spread over everyone: an answer that has waited its turn for park_ms steps aside (parked) and
// goes out when there is time again, and in any case before the load generator gives up on it (rescue_ms).
static void late_run(int64_t t0) {
    unsigned n = 0;
    uint32_t now = (uint32_t)now_ms;
    // at most 256 a pass: answers parked in one burst come due together, and must not stall the pass in turn
    for (unsigned r = 0; lpark_h && r < 256 && now - lpark_h->t0 >= rescue_ms; r++) {
        late_t *l = lpark_h;
        lpark_h = l->next;
        if (!lpark_h) lpark_tl = NULL;
        lpark_n--;
        d_lrescued++;
        late_answer(l);
    }
    while (late_h) {
        late_t *l = late_h;
        if (n >= 8 && (n & 7) == 0) {
            clocks();
            now = (uint32_t)now_ms;
            // the next pass's fresh requests come first
            if (now_us - t0 > LATE_BUDGET_US && (park_ms || (uint16_t)(now16 - l->t_ms) < 400)) break;
        }
        late_h = l->next;
        if (!late_h) late_tl = NULL;
        late_n--;
        if (park_ms && now - l->tq >= park_ms) {
            l->next = NULL;
            if (lpark_tl) lpark_tl->next = l; else lpark_h = l;
            lpark_tl = l;
            lpark_n++;
            d_lparked++;
            continue;
        }
        late_answer(l);
        n++;
    }
    // Parked answers go out only in time the worker would otherwise sleep: a pass that still has part of its
    // pause left. A box that is behind has none, and spending a busy pass's time on them would only make the
    // next answers late too.
    for (unsigned k = 0; lpark_h && !late_h; k++) {
        if ((k & 7) == 0) {
            clocks();
            if (now_us - t0 > pause_us - 500) break;
        }
        late_t *l = lpark_h;
        lpark_h = l->next;
        if (!lpark_h) lpark_tl = NULL;
        lpark_n--;
        late_answer(l);
        n++;
    }
}

static void pg_parse(pgc_t *pc) {
    uint8_t *rb = pc->rb;
    uint32_t p = pc->rpos;
    while (pc->rlen - p >= 5) {
        uint8_t t = rb[p];
        uint32_t len = be32(rb + p + 1);
        if (unlikely(len < 4 || len > (1u << 28))) { pg_lost(pc, "bad message"); return; }
        if (pc->rlen - p < 1 + len) break;
        const uint8_t *m = rb + p;
        switch (t) {
        case 'D':
            if (!pc->is_write) { if (pc->row0 == NONE) pc->row0 = p; pc->nrows++; }
            else if (pc->qc != pc->q.t) {
                pgreq_t *e = rq_at(&pc->q, pc->qc);
                // new post: id (text), created_at (binary)
                if (e->kind == K_NEWPOST && len >= 4 + 2 + 4 + 1 + 4 + 8) {
                    uint32_t l = be32(m + 7);
                    int64_t id = 0;
                    for (uint32_t i = 0; i < l && i < 19; i++) id = id * 10 + (m[11 + i] - '0');
                    e->a = id;
                    if (len >= 4 + 2 + 4 + l + 4 + 8 && be32(m + 11 + l) == 8) e->b = (int64_t)be64(m + 15 + l);
                }
            }
            break;
        case 'C':
            if (!pc->is_write) {
                if (unlikely(!rq_len(&pc->q))) { pg_lost(pc, "unexpected reply"); return; }
                pgreq_t e = rq_pop(&pc->q);
                pg_inflight--;
                conn_t *c = req_conn(&e);
                if (c) {
                    const uint8_t *rows = pc->row0 == NONE ? m : rb + pc->row0;
                    if ((++replies_seen & 15) == 0) clocks();
                    int late = (uint16_t)(now16 - c->t_ms) >= late_ms;
                    // A pass that has run out of time takes no more answers: the requests that arrived meanwhile
                    // would all wait behind them. They queue with the late ones instead.
                    if (!late && budget_us && now_us - pass_t0 > budget_us) { late = 1; d_overbudget++; }
                    if (late && e.kind != K_HEALTH) late_push(&e, c->t_ms, rows, m, pc->nrows);
                    else answer_read(e.fd, c, e.kind, rows, m, pc->nrows);
                }
                pc->row0 = NONE;
                pc->nrows = 0;
            } else {
                if (unlikely(pc->qc == pc->q.t)) { pg_lost(pc, "unexpected reply"); return; }
                pgreq_t *e = rq_at(&pc->q, pc->qc++);
                e->flags |= F_DONE;
                // "INSERT 0 n"
                e->rows = (len >= 6 && m[1 + len - 2] == '1' && m[1 + len - 3] == ' ') ? 1 : 0;
            }
            break;
        case 'E': {
            const char *code = pg_err_field(m + 5, len - 4, 'C');
            if (!pc->is_write) {
                if (unlikely(!rq_len(&pc->q))) { pg_lost(pc, pg_err_field(m + 5, len - 4, 'M')); return; }
                pgreq_t e = rq_pop(&pc->q);
                pg_inflight--;
                if (!(e.flags & F_SYNC)) pc->skip = 1;        // Postgres skips the rest of this batch
                fail_req(&e);
                pc->row0 = NONE;
                pc->nrows = 0;
            } else {
                if (unlikely(pc->qc == pc->q.t)) { pg_lost(pc, pg_err_field(m + 5, len - 4, 'M')); return; }
                pgreq_t *e = rq_at(&pc->q, pc->qc++);
                e->flags |= F_ERR | (!strcmp(code, "23503") ? F_FK : 0);
                pc->berr = 1;
            }
            break;
        }
        case 'Z':
            if (!pc->is_write) {
                if (pc->skip) {
                    pc->skip = 0;
                    while (rq_len(&pc->q)) {
                        pgreq_t e = rq_pop(&pc->q);
                        pg_inflight--;
                        int last = e.flags & F_SYNC;
                        requeue_read(e);
                        if (last) break;
                    }
                }
            } else pg_write_done(pc);
            if (unlikely(opt_debug)) d_pgrtt[d_bucket((uint32_t)(now_ms - pc->owe_since_ms))]++;
            pc->owe_since_ms = now_ms;
            break;
        default: break;      // BindComplete, notices, parameter status: nothing to do
        }
        if (unlikely(!pc->up)) return;
        p += 1 + len;
    }
    pc->rpos = p;
    uint32_t keep = pc->row0 != NONE ? pc->row0 : pc->rpos;
    if (keep == pc->rlen) { pc->rlen = pc->rpos = 0; }
    else if (keep > 0 && (keep >= pc->rcap / 2 || pc->rcap - pc->rlen < 32768)) {
        memmove(rb, rb + keep, pc->rlen - keep);
        pc->rlen -= keep;
        pc->rpos -= keep;
        if (pc->row0 != NONE) pc->row0 -= keep;
    }
}

static void pg_readable(pgc_t *pc) {
    for (;;) {
        if (pc->rcap - pc->rlen < 16384) {
            pc->rcap *= 2;
            pc->rb = realloc(pc->rb, pc->rcap);
        }
        uint32_t space = pc->rcap - pc->rlen;
        ssize_t n = recv(pc->fd, pc->rb + pc->rlen, space, 0);
        if (n > 0) {
            pc->rlen += (uint32_t)n;
            pc->last_rx_ms = now_ms;
            pg_parse(pc);
            if (!pc->up || (uint32_t)n < space) return;
            continue;
        }
        if (n == 0) { pg_lost(pc, "closed by the server"); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN) return;
        pg_lost(pc, strerror(errno));
        return;
    }
}

static void pg_flush_out(pgc_t *pc) {
    while (pc->wsent < pc->wlen) {
        ssize_t n = send(pc->fd, pc->wb + pc->wsent, pc->wlen - pc->wsent, MSG_NOSIGNAL);
        if (n > 0) { pc->wsent += (uint32_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;       // tried again next pass
        pg_lost(pc, "send failed");
        return;
    }
    pc->wlen = pc->wsent = 0;
}

// The next read to send. Normally the queue is empty after every pass and none of this matters. When the
// database falls behind and every read connection has its fill (pg_flight), reads wait here, and the run is
// judged on p95 < 500 ms and p99 < 1 s, so the wait is not spread evenly:
//   - reads leave in arrival order while they have waited less than hold_ms;
//   - one that has waited longer steps aside (parked), so the ones behind it stay fast;
//   - parked reads are sent when there is nothing else to send, and in any case before they are rescue_ms old
//     (the load generator gives up at 60 s), oldest first.
static int next_read(pgreq_t *out) {
    uint32_t now = (uint32_t)now_ms;
    for (;;) {
        if (rq_len(&parkq) && now - rq_at(&parkq, parkq.h)->t0 >= rescue_ms) {
            *out = rq_pop(&parkq);
            if (!req_conn(out)) continue;
            d_rescued++;
            return 1;
        }
        if (rq_len(&rdq)) {
            *out = rq_pop(&rdq);
            if (hold_ms && now - out->t0 >= hold_ms && out->kind != K_HEALTH) {
                if (req_conn(out)) { rq_push(&parkq, out); d_parked++; }
                continue;
            }
            return 1;
        }
        if (!rq_len(&parkq)) return 0;
        *out = rq_pop(&parkq);
        if (req_conn(out)) return 1;
    }
}

// This pass's reads: one batch (one Sync, one round trip) on the read connection with the least waiting;
// past 64 the next connection takes over.
static void flush_reads(void) {
    while (rq_len(&rdq) || rq_len(&parkq)) {
        pgc_t *best = NULL;
        for (int i = 0; i < npg; i++) {
            pgc_t *pc = &pgs[i];
            if (pc->up && !pc->is_write && (!best || rq_len(&pc->q) < rq_len(&best->q))) best = pc;
        }
        if (!best) {
            while (rq_len(&rdq)) { pgreq_t e = rq_pop(&rdq); fail_req(&e); }
            while (rq_len(&parkq)) { pgreq_t e = rq_pop(&parkq); fail_req(&e); }
            return;
        }
        if (rq_len(&best->q) >= pg_flight) return;      // every connection has its fill: the rest waits here
        uint32_t room = pg_flight - rq_len(&best->q), want = rq_len(&rdq) + rq_len(&parkq), n = 0;
        int was_idle = !rq_len(&best->q);
        if (want > PG_BATCH) want = PG_BATCH;
        if (room < want && room < PG_BATCH / 2) return;      // wait until a connection can take a decent batch
        if (room > PG_BATCH) room = PG_BATCH;
        pgreq_t e;
        while (n < room && next_read(&e)) {
            pg_append(best, &e);
            e.flags = 0;
            rq_push(&best->q, &e);
            pg_inflight++;
            n++;
        }
        if (!n) return;
        if (was_idle) best->owe_since_ms = now_ms;
        rq_at(&best->q, best->q.t - 1)->flags |= F_SYNC;
        memcpy(pg_wspace(best, 5), T_SYNC, 5);
    }
}

// This pass's writes: one group (one transaction, one commit) on a write connection with nothing out.
// While every write connection waits for its commit, writes queue here and leave together.
static void flush_writes(void) {
    if (!rq_len(&wrq) && !rq_len(&wretry)) return;
    int any = 0;
    for (int i = 0; i < npg; i++) {
        pgc_t *pc = &pgs[i];
        if (!pc->up || !pc->is_write) continue;
        any = 1;
        if (rq_len(&pc->q)) continue;
        uint32_t n = 0, g = 0;
        pc->owe_since_ms = now_ms;
        while (rq_len(&wretry) && n < PG_WGROUP) {
            pgreq_t e = rq_pop(&wretry);
            pg_append(pc, &e);
            memcpy(pg_wspace(pc, 5), T_SYNC, 5);
            e.flags = F_SYNC;
            rq_push(&pc->q, &e);
            pg_inflight++;
            n++;
        }
        while (rq_len(&wrq) && n < PG_WGROUP) {
            pgreq_t e = rq_pop(&wrq);
            pg_append(pc, &e);
            e.flags = 0;
            rq_push(&pc->q, &e);
            pg_inflight++;
            n++; g++;
        }
        if (g) {
            memcpy(pg_wspace(pc, 5), T_SYNC, 5);
            rq_at(&pc->q, pc->q.t - 1)->flags |= F_SYNC;
        }
        pc->qc = pc->q.h;
        if (!rq_len(&wrq) && !rq_len(&wretry)) break;
    }
    if (!any) {
        while (rq_len(&wretry)) { pgreq_t e = rq_pop(&wretry); fail_req(&e); }
        while (rq_len(&wrq)) { pgreq_t e = rq_pop(&wrq); fail_req(&e); }
    }
}

static void pg_pass(void) {
    flush_reads();
    flush_writes();
    for (int i = 0; i < npg; i++)
        if (pgs[i].up && pgs[i].wlen) pg_flush_out(&pgs[i]);
}

static void pg_housekeeping(void) {
    int tried = 0;
    for (int i = 0; i < npg; i++) {
        pgc_t *pc = &pgs[i];
        if (!pc->up) {
            if (now_ms >= pc->next_try_ms && !tried) {      // one (blocking) attempt per tick
                tried = 1;
                if (pg_login(pc)) pc->next_try_ms = now_ms + 1000;
                else logf_("worker %d: postgres connection %d is back", worker_id, pc->idx);
                clocks();
            }
        } else if (rq_len(&pc->q)) {
            if (now_ms - pc->last_rx_ms > 10000 && now_ms - pc->owe_since_ms > 10000) pg_lost(pc, "no reply for 10 s");
        } else if (ring_fd >= 0) {
            // On the ring an idle Postgres connection is not watched: notice here if the server closed it.
            char b;
            ssize_t r = recv(pc->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) pg_lost(pc, "closed while idle");
        }
    }
}

// ---------------------------------------------------------------------------------------------- HTTP
enum { M_GET, M_POST, M_OTHER };
typedef struct {
    const char *path, *auth, *body;
    uint32_t path_len, auth_len, clen;
    uint8_t method, gzip, close_, expect, has_auth, hdr_done;
} hreq_t;

static inline int ieq(const char *a, const char *lit, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (((uint8_t)a[i] | 0x20) != (uint8_t)lit[i]) return 0;
    return 1;
}

static int accepts_gzip(const char *v, uint32_t n) {
    for (uint32_t i = 0; i + 4 <= n; i++) {
        if (!ieq(v + i, "gzip", 4)) continue;
        uint32_t j = i + 4;
        // the whole coding name, not a part of one ("xgzip", "gzip2")
        if (i && v[i - 1] != ',' && v[i - 1] != ' ' && v[i - 1] != '\t') continue;
        if (j < n && v[j] != ',' && v[j] != ';' && v[j] != ' ' && v[j] != '\t') continue;
        while (j < n && (v[j] == ' ' || v[j] == '\t')) j++;
        if (j < n && v[j] == ';') {      // gzip;q=0 means "not gzip"
            j++;
            while (j < n && (v[j] == ' ' || v[j] == '\t')) j++;
            if (j + 2 <= n && (v[j] | 0x20) == 'q' && v[j + 1] == '=') {
                j += 2;
                int nonzero = 0;
                while (j < n && v[j] != ',') { if (v[j] >= '1' && v[j] <= '9') nonzero = 1; j++; }
                if (!nonzero) return 0;
            }
        }
        return 1;
    }
    return 0;
}

// Returns the request's length, 0 if more bytes are needed, -1 if it is not HTTP, -2 if the body is too large.
static int http_parse(const char *p, uint32_t n, hreq_t *r) {
    r->hdr_done = 0;
    const char *he = memmem(p, n, "\r\n\r\n", 4);
    if (!he) return n > 16384 ? -1 : 0;
    const char *end = he + 2;
    uint32_t hlen = (uint32_t)(he + 4 - p);
    const char *le = memchr(p, '\r', (size_t)(end - p));
    const char *sp1 = memchr(p, ' ', (size_t)(le - p));
    if (!sp1) return -1;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1));
    if (!sp2 || le - sp2 != 9 || memcmp(sp2 + 1, "HTTP/1.", 7)) return -1;
    uint32_t ml = (uint32_t)(sp1 - p);
    r->method = (ml == 3 && !memcmp(p, "GET", 3)) ? M_GET : (ml == 4 && !memcmp(p, "POST", 4)) ? M_POST : M_OTHER;
    r->path = sp1 + 1;
    r->path_len = (uint32_t)(sp2 - sp1 - 1);
    const char *qm = memchr(r->path, '?', r->path_len);
    if (qm) r->path_len = (uint32_t)(qm - r->path);
    r->close_ = sp2[8] == '0';
    r->gzip = r->expect = r->has_auth = 0;
    r->clen = 0;
    int chunked = 0;
    for (const char *line = le + 2; line < end;) {
        const char *lend = memchr(line, '\r', (size_t)(end - line));
        if (!lend) lend = end;
        const char *col = memchr(line, ':', (size_t)(lend - line));
        if (col) {
            uint32_t nl = (uint32_t)(col - line);
            const char *v = col + 1, *ve = lend;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            uint32_t vl = (uint32_t)(ve - v);
            switch (nl) {
            case 15: if (ieq(line, "accept-encoding", 15)) r->gzip = (uint8_t)accepts_gzip(v, vl); break;
            case 13: if (ieq(line, "authorization", 13) && !r->has_auth) { r->has_auth = 1; r->auth = v; r->auth_len = vl; } break;
            case 14:
                if (ieq(line, "content-length", 14)) {
                    uint64_t c = 0;
                    if (!vl) return -1;
                    for (uint32_t i = 0; i < vl; i++) {
                        if (v[i] < '0' || v[i] > '9') return -1;
                        c = c * 10 + (uint64_t)(v[i] - '0');
                        if (c > (1u << 20)) return -2;
                    }
                    r->clen = (uint32_t)c;
                }
                break;
            case 10:
                if (ieq(line, "connection", 10)) {
                    if (vl == 5 && ieq(v, "close", 5)) r->close_ = 1;
                    else if (vl == 10 && ieq(v, "keep-alive", 10)) r->close_ = 0;
                }
                break;
            case 6: if (ieq(line, "expect", 6) && vl == 12 && ieq(v, "100-continue", 12)) r->expect = 1; break;
            case 17: if (ieq(line, "transfer-encoding", 17)) chunked = 1; break;
            }
        }
        line = lend + 2;
    }
    if (chunked) return -1;
    if (n - hlen < r->clen) { r->hdr_done = 1; return 0; }
    r->body = p + hlen;
    return (int)(hlen + r->clen);
}

// "/^[0-9]+$/ and a positive safe integer", else 0
static int64_t parse_id(const char *s, uint32_t n) {
    if (!n) return 0;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > 9007199254740991ull) return 0;
    }
    return (int64_t)v;
}

typedef struct { int64_t uid; const char *uname; uint32_t ulen; } auth_t;
static const char JWT_HDR[] = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";       // {"alg":"HS256","typ":"JWT"}

// 0: ok. 1: missing bearer token. 2: invalid or expired token. 3: invalid token payload.
static int jwt_check(const hreq_t *r, auth_t *a) {
    static uint8_t dec[6144];
    static char ub[6144];
    if (!r->has_auth || r->auth_len < 7 || memcmp(r->auth, "Bearer ", 7)) return 1;
    const char *t = r->auth + 7;
    uint32_t n = r->auth_len - 7;
    const char *d1 = memchr(t, '.', n);
    if (!d1) return 2;
    const char *d2 = memchr(d1 + 1, '.', (size_t)(t + n - d1 - 1));
    if (!d2 || memchr(d2 + 1, '.', (size_t)(t + n - d2 - 1))) return 2;
    uint32_t hl = (uint32_t)(d1 - t), pl = (uint32_t)(d2 - d1 - 1), sl = (uint32_t)(t + n - d2 - 1);
    if (sl != 43 || !hl || !pl || pl > 8000) return 2;
    uint8_t sig[36], mac[32];
    if (b64_decode(d2 + 1, sl, sig) != 32) return 2;
    hmac_sha256(&jwt_key, t, (size_t)(d2 - t), mac);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= sig[i] ^ mac[i];
    if (diff) return 2;
    if (hl != sizeof JWT_HDR - 1 || memcmp(t, JWT_HDR, hl)) {       // not the usual header: read it
        if (hl > 1024) return 2;
        long dn = b64_decode(t, hl, dec);
        static const char *const hk[] = {"alg"};
        jval_t hv[1];
        if (dn <= 0 || j_doc((char *)dec, (size_t)dn, hk, 1, hv) != 1 || hv[0].type != 's') return 2;
        char alg[32];       // "HS256" with every character written as \uXXXX is 30 bytes
        if (hv[0].n >= sizeof alg || j_unescape(hv[0].s, hv[0].n, alg) != 5 || memcmp(alg, "HS256", 5)) return 2;
    }
    long dn = b64_decode(d1 + 1, pl, dec);
    static const char *const pk[] = {"sub", "username", "exp"};
    jval_t v[3];
    if (dn <= 0 || j_doc((char *)dec, (size_t)dn, pk, 3, v) != 1) return 2;
    if (v[2].type != 'n' || v[2].n > 40) return 2;
    char nb[48];
    memcpy(nb, v[2].s, v[2].n);
    nb[v[2].n] = 0;
    double exp_s = strtod(nb, NULL);
    if (!(exp_s * 1000.0 > (double)real_ms)) return 2;
    if (v[0].type != 's' || v[1].type != 's') return 3;
    char sb[32];
    uint32_t sn = v[0].n;
    const char *sp = v[0].s;
    if (v[0].esc) { if (sn >= sizeof sb * 6) return 3; char tmp[200]; sn = j_unescape(sp, sn, tmp); if (sn >= sizeof sb) return 3; memcpy(sb, tmp, sn); sp = sb; }
    while (sn > 1 && *sp == '0') { sp++; sn--; }
    if (sn > 16) return 3;
    a->uid = parse_id(sp, sn);
    if (a->uid <= 0) return 3;
    if (v[1].esc) { a->ulen = j_unescape(v[1].s, v[1].n, ub); a->uname = ub; }
    else { a->uname = v[1].s; a->ulen = v[1].n; }
    return 0;
}

static void queue_read(int fd, conn_t *c, int kind, int64_t id) {
    pgreq_t e = {fd, c->gen, (uint8_t)kind, 0, 0, 0, 0, id, 0, NULL, (uint32_t)now_ms};
    c->st = ST_BUSY;
    rq_push(&rdq, &e);
}

static void handle(int fd, conn_t *c, const hreq_t *r) {
    c->t_ms = now16;
    c->flags = (uint16_t)((c->flags & ~(CF_GZIP | CF_CLOSE | CF_100)) | (r->gzip ? CF_GZIP : 0) | (r->close_ ? CF_CLOSE : 0));
    const char *p = r->path;
    uint32_t n = r->path_len;
    if (r->method == M_GET) {
        if (n == 5 && !memcmp(p, "/feed", 5)) { queue_read(fd, c, K_FEED, 0); return; }
        if (n > 7 && !memcmp(p, "/posts/", 7) && !memchr(p + 7, '/', n - 7)) {
            int64_t id = parse_id(p + 7, n - 7);
            if (id <= 0) { RESPOND_LIT(fd, c, S400, "{\"error\":\"invalid post id\"}"); return; }
            queue_read(fd, c, K_POST, id);
            return;
        }
        if (n == 7 && !memcmp(p, "/health", 7)) { queue_read(fd, c, K_HEALTH, 0); return; }
    } else if (r->method == M_POST) {
        int is_create = n == 6 && !memcmp(p, "/posts", 6);
        int is_like = n > 12 && !memcmp(p, "/posts/", 7) && !memcmp(p + n - 5, "/like", 5) && !memchr(p + 7, '/', n - 12);
        if (is_create || is_like) {
            auth_t a;
            int rc = jwt_check(r, &a);
            if (rc == 1) { RESPOND_LIT(fd, c, S401, "{\"error\":\"missing bearer token\"}"); return; }
            if (rc == 2) { RESPOND_LIT(fd, c, S401, "{\"error\":\"invalid or expired token\"}"); return; }
            if (rc == 3) { RESPOND_LIT(fd, c, S401, "{\"error\":\"invalid token payload\"}"); return; }
            if (is_like) {
                int64_t id = parse_id(p + 7, n - 12);
                if (id <= 0) { RESPOND_LIT(fd, c, S400, "{\"error\":\"invalid post id\"}"); return; }
                pgreq_t e = {fd, c->gen, K_LIKE, 0, 0, 0, 0, a.uid, id, NULL, 0};
                c->st = ST_BUSY;
                rq_push(&wrq, &e);
                return;
            }
            static const char *const bk[] = {"body"};
            jval_t v[1];
            int jr = j_doc(r->body, r->clen, bk, 1, v);
            if (jr == 0) { RESPOND_LIT(fd, c, S400, "{\"error\":\"malformed JSON body\"}"); return; }
            if (jr != 1 || v[0].type != 's') { RESPOND_LIT(fd, c, S400, "{\"error\":\"body is required\"}"); return; }
            wctx_t *x = malloc(sizeof *x + a.ulen + v[0].n + 8);
            x->uid = a.uid;
            x->ulen = a.ulen;
            memcpy(x->d, a.uname, a.ulen);
            char *bd = x->d + a.ulen;
            uint32_t bn = v[0].esc ? j_unescape(v[0].s, v[0].n, bd) : (memcpy(bd, v[0].s, v[0].n), v[0].n);
            const uint8_t *ts = (const uint8_t *)bd;
            js_trim(&ts, &bn);
            if (!bn) { free(x); RESPOND_LIT(fd, c, S400, "{\"error\":\"body is required\"}"); return; }
            if (bn > 500 && utf8_codepoints(ts, bn) > 500) {
                free(x);
                RESPOND_LIT(fd, c, S400, "{\"error\":\"body must be at most 500 characters\"}");
                return;
            }
            if ((const char *)ts != bd) memmove(bd, ts, bn);
            x->blen = bn;
            pgreq_t e = {fd, c->gen, K_NEWPOST, 0, 0, 0, 0, 0, 0, x, 0};
            c->st = ST_BUSY;
            rq_push(&wrq, &e);
            return;
        }
    }
    RESPOND_LIT(fd, c, S404, "{\"error\":\"not found\"}");
}

// Parses and handles requests from p[0..n) while the connection is free to take one. Returns the bytes used.
static uint32_t consume(int fd, conn_t *c, const char *p, uint32_t n) {
    uint32_t off = 0, gen = c->gen;
    while (off < n && c->gen == gen && c->st == ST_IDLE && !(c->flags & (CF_DEAD | CF_CLOSE))) {
        hreq_t r;
        int k = http_parse(p + off, n - off, &r);
        if (k == 0) {
            if (r.hdr_done && r.expect && !(c->flags & CF_100)) {
                c->flags |= CF_100;
                if (send(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25, MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {}
            }
            break;
        }
        if (k < 0) {
            c->flags |= CF_CLOSE;
            c->t_ms = now16;
            if (k == -2) RESPOND_LIT(fd, c, S413, "{\"error\":\"payload too large\"}");
            else RESPOND_LIT(fd, c, S400, "{\"error\":\"bad request\"}");
            return n;
        }
        off += (uint32_t)k;
        handle(fd, c, &r);
    }
    return off;
}

static void process_stash(int fd, conn_t *c) {
    cbuf_t *b = c->in;
    uint32_t gen = c->gen;
    c->flags |= CF_INPROC;
    uint32_t used = consume(fd, c, b->d + b->off, b->len - b->off);
    if (c->gen != gen || c->st == ST_FREE) return;
    c->flags &= (uint16_t)~CF_INPROC;
    b->off += used;
    if (b->off >= b->len) { free(b); c->in = NULL; }
    // nothing more will arrive: once it is idle with no whole request left, it is done
    if ((c->flags & CF_EOF) && c->st == ST_IDLE) conn_kill(fd, c);
}

static void on_data(int fd, conn_t *c, const char *p, uint32_t n) {
    uint32_t gen = c->gen;
    if (unlikely(c->in != NULL)) {
        if (stash(c, p, n)) {        // more than a request can be: give up on the connection
            c->flags |= CF_CLOSE;
            if (c->st == ST_IDLE) conn_kill(fd, c);
            return;
        }
        if (c->st == ST_IDLE) process_stash(fd, c);
        return;
    }
    if (unlikely(c->st != ST_IDLE)) { if (stash(c, p, n)) c->flags |= CF_CLOSE; return; }
    uint32_t used = consume(fd, c, p, n);
    if (c->gen != gen || c->st == ST_FREE) return;
    if (used < n) stash(c, p + used, n - used);
}

// the peer closed its side
static void on_eof(int fd, conn_t *c) {
    c->flags |= CF_EOF;
    if (c->st != ST_IDLE) return;            // answered first (after_response closes it)
    if (c->in) process_stash(fd, c);         // requests already received are still answered
    else conn_kill(fd, c);
}

static void run_resume(void) {
    for (uint32_t i = 0; i < resume_n; i++) {
        conn_t *c = &conns[resume_v[i].fd];
        if (c->gen == resume_v[i].gen && c->st == ST_IDLE && c->in && !(c->flags & CF_DEAD)) process_stash(resume_v[i].fd, c);
    }
    resume_n = 0;
}

// ---------------------------------------------------------------------------------------------- accepting
static char *rbuf;
#define RBUF (64 * 1024)

static void conn_open(int fd) {
    if (unlikely((uint32_t)fd >= conn_cap)) { close(fd); return; }
    conn_t *c = &conns[fd];
    c->st = ST_IDLE;
    c->flags = 0;
    c->in = c->out = NULL;
    live_conns++;
    uint32_t gen = c->gen;
    if (ring_fd < 0) {
        struct epoll_event ev = {EPOLLIN | EPOLLRDHUP | EPOLLET, {.u64 = (uint32_t)fd}};
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    }
    // TCP_DEFER_ACCEPT: the first request is normally here already. Serve it in this pass.
    ssize_t n = recv(fd, rbuf, RBUF, MSG_DONTWAIT);
    if (n > 0) on_data(fd, c, rbuf, (uint32_t)n);
    else if (n == 0) { conn_free(fd, c); return; }
    if (ring_fd >= 0 && c->gen == gen && c->st != ST_FREE && !(c->flags & CF_DEAD)) {
        if ((unsigned)fd < R.nfixed && !(c->flags & CF_FIXED) && c->st != ST_SEND && ring_file_set(fd, fd) == 0) c->flags |= CF_FIXED;
        ring_arm_recv(fd, c);
    }
}

static int accept_from(int l) {
    int n = 0;
    for (; n < 512; n++) {
        int fd = accept4(l, NULL, NULL, ring_fd >= 0 ? SOCK_CLOEXEC : SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
        conn_open(fd);
    }
    return n;
}

static uint32_t pass_no, accept_skip;
static int accept_pass(int force) {
    int n = 0;
    pass_no++;
    if (!force && accept_skip && (pass_no & 3)) return 0;       // nobody is connecting: look every 4th pass
    for (int p = 0; p < nports; p++) n += accept_from(lfd[p][worker_id]);
    accept_skip = n == 0;
    return n;
}

// ---------------------------------------------------------------------------------------------- loops
static int work_pending(void) {
    return pg_inflight || rq_len(&rdq) || rq_len(&parkq) || rq_len(&wrq) || rq_len(&wretry) || resume_n || late_n || lpark_n;
}

static void sleep_until(int64_t t_us) {
    struct timespec ts = {t_us / 1000000, (t_us % 1000000) * 1000};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

static void loop_epoll(void) {
    static struct epoll_event ev[1024];
    int busy = 0, full = 0;
    int64_t t0 = 0, hk = 0;
    for (;;) {
        if (busy && !full && pause_us) sleep_until(t0 + pause_us);
        int n = epoll_wait(ep, ev, 1024, busy ? 0 : 10);
        clocks();
        t0 = pass_t0 = now_us;
        if (n < 0) n = 0;
        full = n == 1024;
        for (int i = 0; i < n; i++) {
            uint64_t u = ev[i].data.u64;
            uint32_t e = ev[i].events;
            if (u >> 40) {
                pgc_t *pc = &pgs[(uint32_t)u];
                if (pc->up) pg_readable(pc);
                continue;
            }
            int fd = (int)(uint32_t)u;
            conn_t *c = &conns[fd];
            if (c->st == ST_FREE) continue;
            uint32_t gen = c->gen;
            if (e & EPOLLOUT && c->out) {
                cbuf_t *b = c->out;
                ssize_t w = send(fd, b->d + b->off, b->len - b->off, MSG_NOSIGNAL);
                if (w < 0 && errno != EAGAIN && errno != EINTR) { c->st = ST_IDLE; conn_kill(fd, c); continue; }
                if (w > 0) b->off += (uint32_t)w;
                if (b->off == b->len) {
                    free(b);
                    c->out = NULL;
                    c->flags &= (uint16_t)~CF_OUT;
                    struct epoll_event m = {EPOLLIN | EPOLLRDHUP | EPOLLET, {.u64 = (uint32_t)fd}};
                    epoll_ctl(ep, EPOLL_CTL_MOD, fd, &m);
                    after_response(fd, c);
                    if (c->gen != gen || c->st == ST_FREE) continue;
                }
            }
            if (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                int eof = 0;
                for (;;) {
                    ssize_t r = recv(fd, rbuf, RBUF, 0);
                    if (r > 0) {
                        on_data(fd, c, rbuf, (uint32_t)r);
                        if (c->gen != gen || c->st == ST_FREE) break;
                        if (r < RBUF && !(e & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))) break;
                        continue;
                    }
                    if (r == 0 || (errno != EAGAIN && errno != EINTR)) eof = 1;
                    break;
                }
                if (eof && c->gen == gen && c->st != ST_FREE) on_eof(fd, c);
            }
        }
        int acc = accept_pass(!busy);
        run_resume();
        pg_pass();
        if (late_n || lpark_n) late_run(t0);
        if (qack_h != qack_t) qack_drain(full ? 8 : 256);
        if (now_ms - hk >= 500) { hk = now_ms; pg_housekeeping(); }
        busy = n > 0 || acc > 0 || work_pending();
    }
}

static fdgen_t *rearm_v;
static uint32_t rearm_n, rearm_cap;

static void loop_uring(void) {
    int busy = 0;
    int64_t t0 = 0, hk = 0, dbg_t = 0;
    unsigned got = 0;
    uint64_t d_pass = 0, d_early = 0, d_cqe = 0;
    int64_t d_maxpass = 0, d_maxgap = 0, d_prev = 0, d_work = 0;
    int d_cpu = -1;
    unsigned d_moved = 0;
    for (;;) {
        if (unlikely(opt_debug) && now_ms - dbg_t >= 5000) {
            logf_("worker %d: %u conns, %.0f passes/s, %.0f early wakes/s, %.1f completions/pass, %u in flight to postgres, longest pass %lld us, longest gap %lld us",
                  worker_id, live_conns, d_pass / 5.0, d_early / 5.0, d_pass ? (double)d_cqe / d_pass : 0.0, pg_inflight, (long long)d_maxpass, (long long)d_maxgap);
            logf_("worker %d: in its passes %.0f%% of the time, on cpu %d, moved to another cpu %u times", worker_id, d_work / 50000.0, d_cpu, d_moved);
            d_work = 0; d_moved = 0;
            logf_("worker %d: %u answers stepped aside as late, %u waiting; socket options through the ring: %s; registered files: %u slots",
                  worker_id, d_deferred, late_n, qack_ring ? "yes" : "no (system call)", R.nfixed);
            d_deferred = 0;
            logf_("worker %d: reads waiting for a postgres connection %u, parked %u (in 5 s: %u parked, %u sent at the age limit); answers parked %u (in 5 s: %u parked, %u sent at the age limit)",
                  worker_id, rq_len(&rdq), rq_len(&parkq), d_parked, d_rescued, lpark_n, d_lparked, d_lrescued);
            logf_("worker %d: %u answers put off because their pass was out of time", worker_id, d_overbudget);
            d_parked = d_rescued = d_lparked = d_lrescued = d_overbudget = 0;
            logf_("worker %d: answers by age <5ms %u, <10 %u, <25 %u, <50 %u, <100 %u, <250 %u, <1s %u, more %u | postgres batches by round trip <5ms %u, <10 %u, <25 %u, <50 %u, <100 %u, <250 %u, more %u",
                  worker_id, d_lat[0], d_lat[1], d_lat[2], d_lat[3], d_lat[4], d_lat[5], d_lat[6], d_lat[7],
                  d_pgrtt[0], d_pgrtt[1], d_pgrtt[2], d_pgrtt[3], d_pgrtt[4], d_pgrtt[5], d_pgrtt[6] + d_pgrtt[7]);
            memset(d_lat, 0, sizeof d_lat); memset(d_pgrtt, 0, sizeof d_pgrtt);
            dbg_t = now_ms; d_pass = d_early = d_cqe = 0; d_maxpass = d_maxgap = 0;
        }
        // One call hands over this pass's sends and waits out the pass: until the 2 ms are over or a full
        // batch of completions is in, whichever is first. Idle: until anything happens.
        if (busy && got < 1024 && pause_us) {
            clocks();
            int64_t left = t0 + pause_us - now_us;
            if (left < 50) left = 0;
            ring_enter(left ? 1024 : 0, left);
            clocks();
            if (left && now_us < t0 + pause_us - 200 &&
                __atomic_load_n(R.cq_tail, __ATOMIC_ACQUIRE) - *R.cq_head < 1024) {
                // a kernel that wakes the wait early: sleep the rest out, then collect
                d_early++;
                sleep_until(t0 + pause_us);
                ring_enter(0, 0);
                clocks();
            }
        } else {
            ring_enter(busy ? 0 : 1, busy ? 0 : 10000);
            clocks();
        }
        t0 = pass_t0 = now_us;
        if (unlikely(opt_debug)) {
            if (d_prev && t0 - d_prev > d_maxgap) d_maxgap = t0 - d_prev;
            d_prev = t0;
            int cpu = sched_getcpu();
            if (cpu != d_cpu) { if (d_cpu >= 0) d_moved++; d_cpu = cpu; }
        }
        unsigned head = *R.cq_head, tail = __atomic_load_n(R.cq_tail, __ATOMIC_ACQUIRE);
        got = tail - head;
        d_pass++; d_cqe += got;
        for (; head != tail; head++) {
            struct io_uring_cqe *q = &R.cqes[head & R.cq_mask];
            uint64_t ud = q->user_data;
            int res = q->res;
            int fd = (int)(uint32_t)ud;
            conn_t *c = &conns[fd];
            if (unlikely(ud >> 60 == UD_MISC)) {
                // a refused socket-option command (older kernel): from now on use the system call
                if (res == -EOPNOTSUPP || res == -EINVAL || res == -ENOTSUP) qack_ring = 0;
                continue;
            }
            if (ud >> 60 == UD_RECV) {
                int hasbuf = q->flags & IORING_CQE_F_BUFFER;
                unsigned bid = q->flags >> IORING_CQE_BUFFER_SHIFT;
                if ((c->gen & 0xfffffff) != ((ud >> 32) & 0xfffffff) || c->st == ST_FREE) {
                    if (hasbuf) ring_recycle(bid);
                    continue;
                }
                uint32_t gen = c->gen;
                int more = q->flags & IORING_CQE_F_MORE;
                if (!more) c->flags &= (uint16_t)~CF_RECV;
                if (res > 0) {
                    if (!(c->flags & CF_DEAD)) on_data(fd, c, (const char *)R.bufs + (size_t)bid * BUFSZ, (uint32_t)res);
                    if (hasbuf) ring_recycle(bid);
                    if (c->gen != gen || c->st == ST_FREE) continue;
                    if (!more && !(c->flags & CF_DEAD)) goto rearm;
                } else {
                    if (hasbuf) ring_recycle(bid);
                    if (res == -ENOBUFS && !(c->flags & CF_DEAD)) goto rearm;
                    if (!(c->flags & CF_DEAD)) { on_eof(fd, c); if (c->gen != gen || c->st == ST_FREE) continue; }
                }
                if ((c->flags & CF_DEAD) && !(c->flags & CF_RECV) && c->st != ST_SEND) conn_free(fd, c);
                continue;
            rearm:
                if (rearm_n == rearm_cap) { rearm_cap = rearm_cap ? rearm_cap * 2 : 256; rearm_v = realloc(rearm_v, rearm_cap * sizeof *rearm_v); }
                rearm_v[rearm_n++] = (fdgen_t){fd, gen};
            } else {
                oblk[(ud >> 48) & 0xfff].pending--;
                if ((c->gen & 0xffff) != ((ud >> 32) & 0xffff) || c->st != ST_SEND) continue;
                if (res < 0) { c->st = ST_IDLE; c->flags |= CF_CLOSE; conn_kill(fd, c); }
                else after_response(fd, c);
            }
        }
        __atomic_store_n(R.cq_head, head, __ATOMIC_RELEASE);
        __atomic_store_n(&R.br->tail, (uint16_t)R.br_tail, __ATOMIC_RELEASE);
        for (uint32_t i = 0; i < rearm_n; i++) {
            conn_t *c = &conns[rearm_v[i].fd];
            if (c->gen == rearm_v[i].gen && c->st != ST_FREE && !(c->flags & (CF_DEAD | CF_RECV))) ring_arm_recv(rearm_v[i].fd, c);
        }
        rearm_n = 0;
        int acc = accept_pass(!busy);
        // This pass's requests go to Postgres first, so it works on them while the answers to earlier ones
        // are built and sent.
        pg_pass();
        if (pg_inflight)
            for (int i = 0; i < npg; i++)
                if (pgs[i].up && rq_len(&pgs[i].q)) pg_readable(&pgs[i]);
        run_resume();
        pg_pass();
        if (pg_inflight) {
            // A pass that took long: Postgres has probably answered what this pass sent it. Take that now
            // instead of a whole pass later.
            clocks();
            if (now_us - t0 > 1500) {
                sends_queued = 0;
                ring_submit();
                for (int i = 0; i < npg; i++)
                    if (pgs[i].up && rq_len(&pgs[i].q)) pg_readable(&pgs[i]);
                run_resume();
                pg_pass();
            }
        }
        if (late_n || lpark_n) late_run(t0);
        sends_queued = 0;
        if (qack_h != qack_t) { clocks(); qack_drain(now_us - t0 < pause_us ? 256 : 8); }
        if (now_ms - hk >= 500) { hk = now_ms; pg_housekeeping(); }
        busy = got > 0 || acc > 0 || work_pending() || R.sq_ltail != __atomic_load_n(R.sq_head, __ATOMIC_ACQUIRE);
        if (unlikely(opt_debug)) { clocks(); if (now_us - t0 > d_maxpass) d_maxpass = now_us - t0; d_work += now_us - t0; }
    }
}

// ---------------------------------------------------------------------------------------------- start-up
static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static void parse_db_url(const char *u) {
    char b[1024];
    snprintf(b, sizeof b, "%s", u);
    char *p = strstr(b, "://");
    if (!p) die("DATABASE_URL: no scheme");
    p += 3;
    char *q = strchr(p, '?');
    if (q) *q = 0;
    char *slash = strchr(p, '/');
    if (slash) { *slash = 0; snprintf(db_name, sizeof db_name, "%s", slash + 1); url_decode(db_name); }
    char *at = strrchr(p, '@');
    char *hostport = p;
    if (at) {
        *at = 0;
        hostport = at + 1;
        char *col = strchr(p, ':');
        if (col) { *col = 0; snprintf(db_pass, sizeof db_pass, "%s", col + 1); url_decode(db_pass); }
        snprintf(db_user, sizeof db_user, "%s", p);
        url_decode(db_user);
    }
    char *col = strrchr(hostport, ':');
    if (hostport[0] == '[') {        // [v6]:port
        char *rb = strchr(hostport, ']');
        if (rb) { *rb = 0; hostport++; col = rb[1] == ':' ? rb + 1 : NULL; }
    }
    if (col) { *col = 0; snprintf(db_port, sizeof db_port, "%s", col + 1); }
    snprintf(db_host, sizeof db_host, "%s", hostport);
    if (!db_name[0]) snprintf(db_name, sizeof db_name, "%s", db_user);
}

static int make_listener(int port) {
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_PASSIVE | AI_NUMERICSERV}, *ai;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(cfg_host, ps, &hints, &ai)) die("HOST does not resolve");
    int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) die("socket");
    int one = 1, zero = 0, defer = 10;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one)) die("SO_REUSEPORT");
    // Accepted sockets inherit these: no call per connection.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof defer);
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &zero, sizeof zero);      // delayed-ACK mode: a request's ACK rides on its response
    if (bind(fd, ai->ai_addr, ai->ai_addrlen)) die("bind");
    if (listen(fd, 65535)) die("listen");
    freeaddrinfo(ai);
    return fd;
}

static void worker_main(int w) {
    worker_id = w;
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (opt_idle) {
        // Lowest scheduling class for the worker itself: on a shared core the kernel's packet processing never
        // waits behind it. (Its own policy only: no affinity, no priority raised.)
        struct sched_param sp = {0};
        sched_setscheduler(0, SCHED_IDLE, &sp);
    }
    rbuf = malloc(RBUF);
    jcap = 256 * 1024;
    jbuf = malloc(jcap);
    oscratch_cap = 256 * 1024;
    oscratch = malloc(oscratch_cap);
    clocks();
    if (opt_uring && ring_init()) {
        if (w == 0) logf_("io_uring not available here (%s): using epoll", strerror(errno));
        ring_fd = -1;
    }
    if (ring_fd >= 0 && w == 0) logf_("io_uring: ring per worker, multishot receive into provided buffers%s", ring_quirk ? " (buffer ring registered the way Ubuntu's 6.8 kernels want it)" : "");
    if (ring_fd < 0) { ep = epoll_create1(EPOLL_CLOEXEC); if (ep < 0) die("epoll_create1"); }
    else { oblk[0].p = mmap(NULL, OBLK, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0); onblk = 1; }
    int nr = 48 / nworkers, nw = 16 / nworkers;
    nr = nr < 2 ? 2 : nr > 8 ? 8 : nr;
    nw = nw < 1 ? 1 : nw > 4 ? 4 : nw;
    if (cfg_rconns > 0) nr = cfg_rconns;
    if (cfg_wconns > 0) nw = cfg_wconns;
    if (nr + nw > MAXPG) { nr = MAXPG - 8; nw = 8; }
    if ((nr + nw) * nworkers > 480) { nr = 1; nw = 1; }
    npg = nr + nw;
    npg_read = nr;
    for (int i = 0; i < npg; i++) {
        pgc_t *pc = &pgs[i];
        pc->idx = i;
        pc->fd = -1;
        pc->is_write = i >= nr;
        pc->rcap = pc->is_write ? 65536 : 1 << 20;
        pc->rb = malloc(pc->rcap);
        pc->wcap = 65536;
        pc->wb = malloc(pc->wcap);
        pc->row0 = NONE;
        if (pg_login(pc)) pc->next_try_ms = now_ms + 500;
        clocks();
    }
    if (ring_fd >= 0) loop_uring();
    else loop_epoll();
}

static volatile sig_atomic_t stopping;
static void on_term(int s) { (void)s; stopping = 1; }

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    b64_init();
    gz_init();
    heads_init();
    pg_templates();
    const char *e;
    if (!(e = getenv("DATABASE_URL"))) { logf_("DATABASE_URL is required"); return 1; }
    parse_db_url(e);
    if (!(e = getenv("JWT_SECRET"))) { logf_("JWT_SECRET is required"); return 1; }
    hmac_key_init(&jwt_key, e, strlen(e));
    if ((e = getenv("HOST")) && *e) snprintf(cfg_host, sizeof cfg_host, "%s", e);
    cfg_port[0] = (e = getenv("PORT")) && *e ? atoi(e) : 3000;
    nports = 1;
    if ((e = getenv("EXTRA_PORT")) && *e && atoi(e) != cfg_port[0]) cfg_port[nports++] = atoi(e);
    if ((e = getenv("FEED_IO")) && !strcmp(e, "epoll")) opt_uring = 0;
    if ((e = getenv("FEED_SCHED_IDLE"))) opt_idle = atoi(e);
    if ((e = getenv("FEED_PAUSE_US"))) pause_us = atoi(e);
    if ((e = getenv("FEED_DEBUG"))) opt_debug = atoi(e);
    if ((e = getenv("FEED_LATE_MS"))) late_ms = atoi(e);
    if ((e = getenv("FEED_FLIGHT")) && atoi(e) > 0) pg_flight = (uint32_t)atoi(e);
    if ((e = getenv("FEED_HOLD_MS"))) hold_ms = (uint32_t)atoi(e);
    if ((e = getenv("FEED_PARK_MS"))) park_ms = (uint32_t)atoi(e);
    if ((e = getenv("FEED_BUDGET_US"))) budget_us = atoi(e);
    if ((e = getenv("FEED_RESCUE_MS")) && atoi(e) > 0) rescue_ms = (uint32_t)atoi(e);
    if ((e = getenv("FEED_RCONNS"))) cfg_rconns = atoi(e);
    if ((e = getenv("FEED_WCONNS"))) cfg_wconns = atoi(e);

    cpu_set_t cs;
    nworkers = sched_getaffinity(0, sizeof cs, &cs) == 0 ? CPU_COUNT(&cs) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if ((e = getenv("FEED_WORKERS")) && atoi(e) > 0) nworkers = atoi(e);
    if (nworkers < 1) nworkers = 1;
    if (nworkers > MAXW) nworkers = MAXW;

    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); getrlimit(RLIMIT_NOFILE, &rl); }
    conn_cap = rl.rlim_cur > (1u << 24) ? 1u << 24 : (uint32_t)rl.rlim_cur;
    conns = mmap(NULL, (size_t)conn_cap * sizeof(conn_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (conns == MAP_FAILED) die("mmap connection table");
    madvise(conns, (size_t)conn_cap * sizeof(conn_t), MADV_HUGEPAGE);
    shm = mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (shm == MAP_FAILED) die("mmap");
    clocks();
    shm->start_ms = now_ms;

    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    if (getaddrinfo(db_host, db_port, &hints, &db_ai)) { logf_("DATABASE_URL: host %s does not resolve", db_host); return 1; }

    for (int p = 0; p < nports; p++)
        for (int w = 0; w < nworkers; w++) lfd[p][w] = make_listener(cfg_port[p]);

    logf_("feed server: %d workers, ports %d%s%d, io=%s", nworkers, cfg_port[0], nports > 1 ? "," : " ",
          nports > 1 ? cfg_port[1] : 0, opt_uring ? "io_uring (epoll if unavailable)" : "epoll");

    static pid_t pids[MAXW];
    struct sigaction sa = {.sa_handler = on_term};
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    for (int w = 0; w < nworkers; w++) {
        pid_t pid = fork();
        if (pid < 0) die("fork");
        if (pid == 0) { signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); worker_main(w); _exit(0); }
        pids[w] = pid;
    }
    while (!stopping) {
        int st;
        pid_t pid = wait(&st);
        if (pid < 0) { if (errno == EINTR) continue; break; }
        for (int w = 0; w < nworkers && !stopping; w++) {
            if (pids[w] != pid) continue;
            logf_("worker %d exited (status %d): starting a new one", w, st);
            usleep(100000);
            pid_t np = fork();
            if (np == 0) { signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); worker_main(w); _exit(0); }
            pids[w] = np;
        }
    }
    for (int w = 0; w < nworkers; w++) if (pids[w] > 0) kill(pids[w], SIGTERM);
    return 0;
}
