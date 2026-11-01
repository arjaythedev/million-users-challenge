// PostgreSQL pool layer: per-worker pool of wire-protocol connections
// (pgw.cpp owns the socket framing, auth, encode and decode; this file
// owns queueing, conn lifecycle and pool load balancing).
//
// Reads (feed/get/health) batch up to PACK statements under one Sync per
// pump pass: they cannot violate each other, so sharing an implicit
// transaction is safe. Writes (create/like) always own a Sync: their
// implicit commit is the response barrier, and an error must not roll
// back a neighbour. Results pair 1:1 with the sent queue by wire order.

#include "server.h"

bool pgw_connect(Pg* p);
int pgw_encode_req(Pg* p, Req* r);
int pgw_encode_sync(Pg* p);
int pgw_flush(Pg* p);
void pgw_recv_parse(Worker* w, Pg* p);
void pgw_close(Pg* p);
void pg_down(Worker* w, Pg* p);

bool pg_connect(Worker* w, Pg* p) {
  if (!pgw_connect(p)) return false;
  p->up = 1;
  if (w->uring) {
    ur_pg_in(w, p);  // one-shot POLLIN, re-armed by its own CQE handler
  } else {
    epoll_event e;
    e.events = EPOLLIN;
    e.data.ptr = p;
    epoll_ctl(w->ep, EPOLL_CTL_ADD, p->fd, &e);
  }
  p->out_armed = 0;
  return true;
}

static void pg_arm_out(Worker* w, Pg* p, bool on) {
  if (p->out_armed == on) return;
  p->out_armed = on;
  if (w->uring) {
    // one-shot POLLOUT; the CQE clears out_armed itself, so disarming is
    // just letting the event fire once and not re-arming
    if (on) ur_pg_out_arm(w, p);
    return;
  }
  ep_mod(w, p->fd, p, EPOLLIN | (on ? (uint32_t)EPOLLOUT : 0u));
}

// drain any input already buffered in the kernel/userspace. The wire
// client never reads inside sends (unlike libpq), but running the parser
// after writes keeps the old contract and costs nothing when idle.
static void pg_drain(Worker* w, Pg* p) { pgw_recv_parse(w, p); }

#define PACK 64
static inline int is_write(const Req* r) {
  return r->type == R_CREATE || r->type == R_LIKE;
}

// encode queued requests as Bind+Execute, closing each Sync group at a
// write or the pass end, then flush once for the whole batch
void pg_pump(Worker* w, Pg* p) {
  int in_grp = 0;
  while (p->inq_h) {
    Req* r = p->inq_h;
    p->inq_h = r->pg_next;
    if (!p->inq_h) p->inq_t = nullptr;
    if (!r->c) {  // conn died before we sent it
      r->on_pg = 0;
      req_free(w, r);
      continue;
    }
    int wr = is_write(r);
    if (wr && in_grp) {  // a write never joins a read group
      if (pgw_encode_sync(p)) goto dead;
      p->grp_next++;
      in_grp = 0;
    }
    if (pgw_encode_req(p, r)) goto dead;
    // publish only after encoding: a req in `sent` always owns the
    // next terminal result on the wire
    p->sent.push_back(r);
    r->grp = p->grp_next;
    if (wr || ++in_grp == PACK) {
      if (pgw_encode_sync(p)) goto dead;
      p->grp_next++;
      in_grp = 0;
    }
  }
  if (in_grp) {
    if (pgw_encode_sync(p)) goto dead;
    p->grp_next++;
  }
  {
    int f = pgw_flush(p);
    if (f < 0) goto dead;
    pg_arm_out(w, p, f == 1);
    pg_drain(w, p);
  }
  return;
dead:
  pg_down(w, p);
}

void pg_down(Worker* w, Pg* p) {
  if (getenv("PGDBG"))
    fprintf(stderr, "pg_down fd=%d sent=%zu inq=%p held=%p\n", p->fd,
            p->sent.size(), (void*)p->inq_h, (void*)p->held);
  if (p->up) {
    if (!w->uring) epoll_ctl(w->ep, EPOLL_CTL_DEL, p->fd, nullptr);
    // uring mode: pending poll CQEs on the dead fd resolve themselves to
    // -ECANCELED and are dropped by the !p->up check in the handler
    p->up = 0;
  }
  pgw_close(p);
  p->in_len = 0;
  p->out_len = p->out_off = 0;
  p->flds.clear();
  p->fcols = 0;
  p->res.nrows = 0;
  p->pgen++;
  p->out_armed = 0;
  p->want_flush = 0;
  p->err_grp = 0;
  p->grp_next = 1;
  p->retry_at = w->now + 2000;
  if (p->held) {
    Req* h = p->held;
    p->held = nullptr;
    h->held = 0;
    h->on_pg = 0;
    if (h->c) {
      req_err(h, 500, "internal server error");
      conn_drain(w, h->c);
    } else {
      req_free(w, h);
    }
  }
  while (p->inq_h) {
    Req* r = p->inq_h;
    p->inq_h = r->pg_next;
    r->on_pg = 0;
    if (r->c) {
      req_err(r, 500, "internal server error");
      conn_drain(w, r->c);
    } else {
      req_free(w, r);
    }
  }
  p->inq_t = nullptr;
  while (!p->sent.empty()) {
    Req* r = p->sent.front();
    p->sent.pop_front();
    r->on_pg = 0;
    if (r->c) {
      req_err(r, 500, "internal server error");
      conn_drain(w, r->c);
    } else {
      req_free(w, r);
    }
  }
}

void pg_on_read(Worker* w, Pg* p) { pgw_recv_parse(w, p); }

void pg_on_write(Worker* w, Pg* p) {
  int f = pgw_flush(p);
  if (f < 0) {
    pg_down(w, p);
    return;
  }
  pg_arm_out(w, p, f == 1);
  pg_drain(w, p);
}

// pick the least busy connected Pg of the request's class and queue the
// request; the other class is only a fallback for conn-loss windows
void pg_submit(Worker* w, Req* r) {
  int wr = is_write(r);
  Pg* best = nullptr;
  size_t bestq = SIZE_MAX;
  Pg* fb = nullptr;
  size_t fbq = SIZE_MAX;
  for (Pg& p : w->pg) {
    if (!p.up) continue;
    size_t q = p.sent.size() + (p.inq_h ? 1 : 0);
    if (p.wr == wr) {
      if (q < bestq) {
        bestq = q;
        best = &p;
      }
    } else if (q < fbq) {
      fbq = q;
      fb = &p;
    }
  }
  if (!best) best = fb;
  if (!best) {
    if (r->type == R_HEALTH) {
      const char* m =
          "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"
          "\"no database connection\"}";
      req_resp(r, 503, m, strlen(m));
    } else {
      req_err(r, 500, "internal server error");
    }
    conn_drain(w, r->c);
    return;
  }
  r->on_pg = 1;
  r->pg_next = nullptr;
  if (best->inq_t) best->inq_t->pg_next = r;
  else best->inq_h = r;
  best->inq_t = r;
  // flush at the end of the epoll batch: several submits share one send()
  if (!best->want_flush) {
    best->want_flush = 1;
    w->pg_flush.push_back(best);
  }
}
