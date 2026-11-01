// io_uring backend: per-worker ring, multishot accept on both listeners,
// provided-buffer multishot recv per conn, SEND SQEs for backpressured
// writes, one-shot polls for PG sockets. Selected by URING=1; any
// setup failure falls back to the epoll worker loop.
//
// user_data packs (16-bit generation << 48) | (object ptr & ~7) | op tag:
// pointers are >=8 aligned so the low 3 bits carry the op, and the top 16
// bits carry a generation that lets stale CQEs be dropped (used for PG
// sockets, where a poll can outlive the PGconn it was armed for; Conn
// lifetime is instead governed by ur_inflight so its generation is 0).

#include "server.h"

#ifdef HAVE_URING

#include <liburing.h>
#include <poll.h>

#include <new>

// pre-6.6 userspace headers may lack this flag; passing the real value is
// safe: kernels that reject it fall into the reduced-flags retry below.
#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif

// knob defaults: SQ 2048 (staging only - armed multishot ops hold no SQ
// slot), CQ 8192, provided-buffer pool 4096 x 4KB demand-paged. Sized so
// all workers' rings + buf rings fit under the 8MB shared memlock budget
// on kernels that charge ring regions to RLIMIT_MEMLOCK (~350KB/worker).
static int UR_SQ, UR_CQ, UR_BN, UR_BS;

static void ur_knobs() {
  if (UR_SQ) return;
  UR_SQ = getenv("URING_SQ") ? atoi(getenv("URING_SQ")) : 2048;
  UR_CQ = getenv("URING_CQ") ? atoi(getenv("URING_CQ")) : 8192;
  UR_BN = getenv("URING_BN") ? atoi(getenv("URING_BN")) : 4096;
  UR_BS = getenv("URING_BS") ? atoi(getenv("URING_BS")) : 4096;
}

static inline uint64_t ud_of(const void* p, uint64_t tag, uint16_t gen) {
  return ((uint64_t)gen << 48) | ((uintptr_t)p & 0xFFFFFFFFFFF8ULL) | tag;
}

static inline void* ud_ptr(uint64_t ud) {
  return (void*)(uintptr_t)(ud & 0xFFFFFFFFFFF8ULL);
}

io_uring_sqe* ur_sqe(Worker* w) {
  io_uring_sqe* s = io_uring_get_sqe(w->ur);
  if (s) return s;
  io_uring_submit(w->ur);
  return io_uring_get_sqe(w->ur);
}

void ur_accept_arm(Worker* w, Lfd* l) {
  io_uring_sqe* s = ur_sqe(w);
  if (!s) {
    l->ur_pend = 1;  // batch end retries
    return;
  }
  io_uring_prep_multishot_accept(s, l->fd, nullptr, nullptr,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
  s->user_data = ud_of(l, UD_ACCEPT, 0);
}

void ur_pg_in(Worker* w, Pg* p) {
  io_uring_sqe* s = ur_sqe(w);
  if (!s) {
    // a dropped arm is silent pipeline death: nothing else ever polls
    // this socket, so the lost flag forces a retry at batch end
    p->pgin_lost = 1;
    return;
  }
  p->pgin_lost = 0;
  io_uring_prep_poll_add(s, p->fd, POLLIN);
  s->user_data = ud_of(p, UD_PGIN, p->pgen);
}

void ur_pg_out_arm(Worker* w, Pg* p) {
  io_uring_sqe* s = ur_sqe(w);
  if (!s) {
    p->pgout_lost = 1;
    return;
  }
  p->pgout_lost = 0;
  io_uring_prep_poll_add(s, p->fd, POLLOUT);
  s->user_data = ud_of(p, UD_PGOUT, p->pgen);
}

int ur_setup(Worker* w) {
  ur_knobs();
  w->ur = new (std::nothrow) io_uring;
  if (!w->ur) return 0;
  memset(w->ur, 0, sizeof(*w->ur));

  io_uring_params p;
  memset(&p, 0, sizeof(p));
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
            IORING_SETUP_COOP_TASKRUN | IORING_SETUP_NO_SQARRAY |
            IORING_SETUP_CQSIZE;
  p.cq_entries = (unsigned)UR_CQ;
  int r = io_uring_queue_init_params((unsigned)UR_SQ, w->ur, &p);
  if (r < 0) {
    fprintf(stderr, "io_uring full-flag init failed: %s; retrying reduced\n",
            strerror(-r));
    // peel back the progressive flags for older kernels
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN |
              IORING_SETUP_CQSIZE;
    r = io_uring_queue_init_params(1024, w->ur, &p);
    if (r < 0) {
      memset(&p, 0, sizeof(p));
      r = io_uring_queue_init_params(1024, w->ur, &p);
      if (r < 0) {
        fprintf(stderr, "io_uring init failed: %s\n", strerror(-r));
        delete w->ur;
        w->ur = nullptr;
        return 0;
      }
    }
  }

  // provided-buffer ring: a shared pool the kernel picks recv targets
  // from; buffers are only checked out while request bytes are in
  // flight, so a million idle conns cost nothing here
  int err = 0;
  w->br = io_uring_setup_buf_ring(w->ur, (unsigned)UR_BN, UR_BGID, 0, &err);
  if (!w->br) {
    fprintf(stderr, "io_uring buf_ring setup failed: %s\n", strerror(-err));
    io_uring_queue_exit(w->ur);
    delete w->ur;
    w->ur = nullptr;
    return 0;
  }
  w->bsz = (uint32_t)UR_BS;
  w->bmask = (uint32_t)UR_BN - 1;
  w->barena = (char*)aligned_alloc(4096, (size_t)UR_BN * w->bsz);
  if (!w->barena) {
    io_uring_queue_exit(w->ur);
    delete w->ur;
    w->ur = nullptr;
    return 0;
  }
  io_uring_buf_ring_init(w->br);
  for (int i = 0; i < UR_BN; i++)
    io_uring_buf_ring_add(w->br, w->barena + (size_t)i * w->bsz, w->bsz,
                          (unsigned short)i, w->bmask, i);
  io_uring_buf_ring_advance(w->br, UR_BN);
  w->uring = 1;
  return 1;
}

void ur_loop(Worker* w) {
  for (int i = 0; i < 2; i++) ur_accept_arm(w, &w->lfd[i]);

  __kernel_timespec ts;
  ts.tv_sec = 1;
  ts.tv_nsec = 0;
  io_uring_cqe* cqe;
  for (;;) {
    // submits + one enter: under DEFER_TASKRUN this is also where the
    // kernel runs the task work that produces most CQEs
    io_uring_submit_and_wait_timeout(w->ur, &cqe, 1, &ts, nullptr);
    w->now = now_ms();
    unsigned head;
    io_uring_cqe* c;
    unsigned seen = 0;
    io_uring_for_each_cqe(w->ur, head, c) {
      seen++;
      uint64_t ud = c->user_data;
      switch ((int)(ud & 7)) {
        case UD_ACCEPT:
          ur_on_accept(w, (Lfd*)ud_ptr(ud), c->res, c->flags);
          break;
        case UD_RECV:
          ur_on_recv(w, (Conn*)ud_ptr(ud), c->res, c->flags);
          break;
        case UD_SEND:
          ur_on_send(w, (Conn*)ud_ptr(ud), c->res);
          break;
        case UD_PGIN: {
          Pg* p = (Pg*)ud_ptr(ud);
          // a poll armed for a previous PGconn on this slot is stale
          if (p->up && (uint16_t)(ud >> 48) == p->pgen) {
            pg_on_read(w, p);
            if (p->up) ur_pg_in(w, p);
          }
          break;
        }
        case UD_PGOUT: {
          Pg* p = (Pg*)ud_ptr(ud);
          if ((uint16_t)(ud >> 48) != p->pgen) break;
          p->out_armed = 0;
          if (p->up) pg_on_write(w, p);
          break;
        }
        default:
          break;
      }
    }
    io_uring_cq_advance(w->ur, seen);
    // return every consumed provided buffer; a missed return leaks ring
    // capacity and degrades to -ENOBUFS. buf_ring_add's last arg is the
    // offset of THIS batch: every entry must land at tail+i, not all at
    // tail+0, or advance() publishes stale entries that hand the same
    // buffer to several conns at once.
    if (!w->bidret.empty()) {
      size_t n = w->bidret.size();
      for (size_t i = 0; i < n; i++) {
        uint16_t bid = w->bidret[i];
        io_uring_buf_ring_add(w->br, w->barena + (size_t)bid * w->bsz,
                              w->bsz, bid, w->bmask, (int)i);
      }
      io_uring_buf_ring_advance(w->br, (int)n);
      w->bidret.clear();
    }
    // retry queues: swap first - ur_recv_arm/ur_kick_send can push right
    // back onto these vectors on a full SQ, so range-for over the live
    // vector would mutate it during iteration
    std::vector<Conn*> rx;
    rx.swap(w->rxrearm);
    for (Conn* c : rx) {
      c->ur_rxq = 0;
      if (c->fd >= 0) ur_recv_arm(w, c);
    }
    std::vector<Conn*> sq;
    sq.swap(w->sendq);
    for (Conn* c : sq) {
      c->ur_sendq = 0;
      if (c->fd >= 0) ur_kick_send(w, c);
    }
    // pg arms that lost the SQ lottery: without this the pipeline never
    // sees another readiness notification and queued requests stall
    for (Pg& p : w->pg) {
      if (!p.up) continue;
      if (p.pgin_lost) ur_pg_in(w, &p);
      if (p.pgout_lost) ur_pg_out_arm(w, &p);
    }
    worker_batch_end(w);
  }
}

#else  // !HAVE_URING: setup fails so worker_run falls back to epoll

int ur_setup(Worker*) { return 0; }
void ur_loop(Worker*) {}
io_uring_sqe* ur_sqe(Worker*) { return nullptr; }
void ur_accept_arm(Worker*, Lfd*) {}
void ur_pg_in(Worker*, Pg*) {}
void ur_pg_out_arm(Worker*, Pg*) {}

#endif
