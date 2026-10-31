#ifndef COMPACT_URING_H
#define COMPACT_URING_H
#include <errno.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int fd;
    unsigned sq_entries, sq_mask, cq_mask, sq_tail, sq_flushed;
    unsigned *sq_head, *sq_ktail, *cq_khead, *cq_ktail;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
} Ring;

typedef struct {
    struct io_uring_buf_ring *ring; /* NULL: classic provided buffers, recycled with PROVIDE_BUFFERS entries */
    char *base;
    unsigned entries, size, tail, group;
    Ring *owner;
} BufRing;

static inline int ring_setup(Ring *r, unsigned entries, unsigned cq_entries) {
    /* Newest flags first; older kernels (before 6.6 / 6.1) reject them, so retry without. */
    static const unsigned optional[] = {
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_NO_SQARRAY,
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
        0
    };
    struct io_uring_params p;
    int fd = -1;
    for (unsigned i = 0; i < sizeof(optional) / sizeof(*optional) && fd < 0; i++) {
        memset(&p, 0, sizeof(p));
        p.flags = optional[i] | IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL;
        p.cq_entries = cq_entries;
        fd = (int)syscall(__NR_io_uring_setup, entries, &p);
        if (fd < 0 && errno != EINVAL) return -errno;
    }
    if (fd < 0) return -errno;
    if (!(p.features & IORING_FEAT_SINGLE_MMAP) || !(p.features & IORING_FEAT_EXT_ARG)) { close(fd); return -EINVAL; }
    size_t sq_size = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t size = sq_size > cq_size ? sq_size : cq_size;
    char *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (ptr == MAP_FAILED) { close(fd); return -errno; }
    void *sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) { close(fd); return -errno; }
    r->fd = fd;
    r->sq_entries = p.sq_entries;
    r->sq_mask = *(unsigned *)(ptr + p.sq_off.ring_mask);
    r->cq_mask = *(unsigned *)(ptr + p.cq_off.ring_mask);
    r->sq_head = (unsigned *)(ptr + p.sq_off.head);
    r->sq_ktail = (unsigned *)(ptr + p.sq_off.tail);
    r->cq_khead = (unsigned *)(ptr + p.cq_off.head);
    r->cq_ktail = (unsigned *)(ptr + p.cq_off.tail);
    r->cqes = (struct io_uring_cqe *)(ptr + p.cq_off.cqes);
    r->sqes = sqes;
    if (!(p.flags & IORING_SETUP_NO_SQARRAY)) {
        unsigned *array = (unsigned *)(ptr + p.sq_off.array);
        for (unsigned i = 0; i < p.sq_entries; i++) array[i] = i;
    }
    r->sq_tail = r->sq_flushed = *r->sq_ktail;
    return 0;
}

/* A sparse registered file table: accepted sockets live there instead of the fd table. */
static inline int ring_register_files(Ring *r, unsigned count) {
    struct io_uring_rsrc_register reg;
    memset(&reg, 0, sizeof(reg));
    reg.nr = count;
    reg.flags = IORING_RSRC_REGISTER_SPARSE;
    if (syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_FILES2, &reg, sizeof(reg)) < 0) return -errno;
    return 0;
}

static inline unsigned ring_pending(const Ring *r) { return r->sq_tail - r->sq_flushed; }

/* Submits queued entries and optionally waits for one completion or the timeout. */
static inline int ring_enter(Ring *r, unsigned wait, long timeout_ns) {
    __atomic_store_n(r->sq_ktail, r->sq_tail, __ATOMIC_RELEASE);
    unsigned submit = r->sq_tail - r->sq_flushed;
    unsigned flags = wait ? IORING_ENTER_GETEVENTS : 0;
    struct __kernel_timespec ts = { .tv_sec = timeout_ns / 1000000000, .tv_nsec = timeout_ns % 1000000000 };
    struct io_uring_getevents_arg arg = { .ts = (uint64_t)(uintptr_t)&ts };
    int ret;
    if (wait && timeout_ns >= 0)
        ret = (int)syscall(__NR_io_uring_enter, r->fd, submit, wait, flags | IORING_ENTER_EXT_ARG, &arg, sizeof(arg));
    else
        ret = (int)syscall(__NR_io_uring_enter, r->fd, submit, wait, flags, NULL, 0);
    int error = ret < 0 ? -errno : ret;
    /* Without SQPOLL the kernel consumes entries synchronously, so its head is what was submitted. */
    r->sq_flushed = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    return error;
}

static inline struct io_uring_sqe *ring_sqe(Ring *r) {
    if (r->sq_tail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >= r->sq_entries) {
        ring_enter(r, 0, -1);
        if (r->sq_tail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE) >= r->sq_entries) return NULL;
    }
    struct io_uring_sqe *sqe = &r->sqes[r->sq_tail++ & r->sq_mask];
    memset(sqe, 0, sizeof(*sqe));
    return sqe;
}

static inline struct io_uring_cqe *ring_peek(Ring *r, unsigned head) {
    unsigned tail = __atomic_load_n(r->cq_ktail, __ATOMIC_ACQUIRE);
    return head == tail ? NULL : &r->cqes[head & r->cq_mask];
}
static inline unsigned ring_cq_head(Ring *r) { return *r->cq_khead; }
static inline void ring_cq_advance(Ring *r, unsigned head) { __atomic_store_n(r->cq_khead, head, __ATOMIC_RELEASE); }

static inline int provide_buffers(BufRing *b, unsigned bid, unsigned count) {
    struct io_uring_sqe *sqe = ring_sqe(b->owner);
    if (!sqe) return -EBUSY;
    sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
    sqe->fd = (int)count;
    sqe->addr = (uint64_t)(uintptr_t)(b->base + (size_t)bid * b->size);
    sqe->len = b->size;
    sqe->off = bid;
    sqe->buf_group = (uint16_t)b->group;
    sqe->flags = IOSQE_CQE_SKIP_SUCCESS;
    sqe->user_data = UINT64_MAX;
    return 0;
}
static inline int bufring_setup(Ring *r, BufRing *b, unsigned entries, unsigned size, unsigned group) {
    char *base = mmap(NULL, (size_t)entries * size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return -ENOMEM;
    b->base = base;
    b->entries = entries;
    b->size = size;
    b->group = group;
    b->owner = r;
    b->tail = 0;
    b->ring = NULL;
    size_t ring_bytes = entries * sizeof(struct io_uring_buf);
    void *ring = mmap(NULL, ring_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (ring == MAP_FAILED) return -ENOMEM;
    struct io_uring_buf_reg reg = { .ring_addr = (uint64_t)(uintptr_t)ring, .ring_entries = entries, .bgid = (uint16_t)group };
    if (syscall(__NR_io_uring_register, r->fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) {
        /* Some distribution kernels reject mapped buffer rings; classic provided buffers work everywhere. */
        munmap(ring, ring_bytes);
        return provide_buffers(b, 0, entries);
    }
    b->ring = ring;
    for (unsigned i = 0; i < entries; i++) {
        struct io_uring_buf *buf = &b->ring->bufs[i];
        buf->addr = (uint64_t)(uintptr_t)(base + (size_t)i * size);
        buf->len = size;
        buf->bid = (uint16_t)i;
    }
    b->tail = (uint16_t)entries;
    __atomic_store_n(&b->ring->tail, (uint16_t)b->tail, __ATOMIC_RELEASE);
    return 0;
}
static inline char *bufring_data(BufRing *b, unsigned bid) { return b->base + (size_t)bid * b->size; }
/* Returns a buffer to the kernel; becomes visible at the next bufring_publish or ring submission. */
static inline void bufring_recycle(BufRing *b, unsigned bid) {
    if (!b->ring) { provide_buffers(b, bid, 1); return; }
    struct io_uring_buf *buf = &b->ring->bufs[b->tail & (b->entries - 1)];
    buf->addr = (uint64_t)(uintptr_t)bufring_data(b, bid);
    buf->len = b->size;
    buf->bid = (uint16_t)bid;
    b->tail++;
}
static inline void bufring_publish(BufRing *b) {
    if (b->ring) __atomic_store_n(&b->ring->tail, (uint16_t)b->tail, __ATOMIC_RELEASE);
}
#endif
