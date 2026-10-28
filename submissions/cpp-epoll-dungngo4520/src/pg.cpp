// PostgreSQL pool layer: per-worker pool of wire-protocol connections
// (pgw.cpp owns the socket framing, auth, encode and decode; this file
// owns queueing, conn lifecycle and pool load balancing).
//
// Each client request maps to exactly one Bind+Execute+Sync triple, so
// results pair 1:1 with the sent queue by wire order alone.

#include "server.h"

bool pgw_connect(Pg* p);
int pgw_encode_req(Pg* p, Req* r);
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

// encode every queued request as Bind+Execute+Sync, then one flush
void pg_pump(Worker* w, Pg* p) {
  while (p->inq_h) {
    Req* r = p->inq_h;
    p->inq_h = r->pg_next;
    if (!p->inq_h) p->inq_t = nullptr;
    if (!r->c) {  // conn died before we sent it
      r->on_pg = 0;
      req_free(w, r);
      continue;
    }
    if (pgw_encode_req(p, r)) {
      pg_down(w, p);
      return;
    }
    // publish only after the full triple encoded: a req in `sent`
    // always owns the next terminal result on the wire
    p->sent.push_back(r);
  }
  int f = pgw_flush(p);
  if (f < 0) {
    pg_down(w, p);
    return;
  }
  pg_arm_out(w, p, f == 1);
  pg_drain(w, p);
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

// pick the least busy connected Pg and queue the request
void pg_submit(Worker* w, Req* r) {
  Pg* best = nullptr;
  size_t bestq = SIZE_MAX;
  for (Pg& p : w->pg) {
    if (!p.up) continue;
    size_t q = p.sent.size() + (p.inq_h ? 1 : 0);
    if (q < bestq) {
      bestq = q;
      best = &p;
    }
  }
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
