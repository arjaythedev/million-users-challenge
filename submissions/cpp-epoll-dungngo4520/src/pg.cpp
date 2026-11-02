// PostgreSQL pool layer: per-worker pool of wire-protocol connections
// (pgw.cpp owns the socket framing, auth, encode and decode; this file
// owns queueing, conn lifecycle and pool load balancing).
//
// Reads (feed/get/health) batch up to PACK statements under one Sync per
// pump pass: they cannot violate each other, so sharing an implicit
// transaction is safe. Writes (create/like) commit in bounded WGRP Sync
// groups: the Sync is the response barrier (held until ReadyForQuery),
// and a failed write rolls back at most WGRP-1 neighbours. Results pair
// 1:1 with the sent queue by wire order.

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

// drain any input already buffered in the kernel/userspace. Skipped when
// nothing is owed and when nothing was left over from the last pass: fresh
// results are an RTT away, so a recv here is a guaranteed EAGAIN - real
// arrivals come through EPOLLIN anyway.
static void pg_drain(Worker* w, Pg* p) {
  if (p->sent_h && p->in_len > p->in_pos) pgw_recv_parse(w, p);
}

#define PACK 128  // group only closes at pass end or a write anyway; a
                  // bigger cap just amortizes more Sync round-trips under
                  // saturation. Drain budget still time-slices results.
#define WGRP 16   // writes commit in bounded batches: one fsync per group
                  // instead of one per write. Small enough that a failed
                  // write rolls back at most WGRP-1 neighbors, not a flood.
static inline int is_write(const Req* r) {
  return r->type == R_CREATE || r->type == R_LIKE;
}

// encode queued requests as Bind+Execute, closing each Sync group at a
// write or the pass end, then flush once for the whole batch
void pg_pump(Worker* w, Pg* p) {
  int in_grp = 0;
  int wgrp = 0;  // open group is a write group: reads close it, it closes
                 // itself at WGRP so PG still commits bounded batches
  while (p->inq_h) {
    Req* r = p->inq_h;
    p->inq_h = r->pg_next;
    p->inq_n--;
    if (!p->inq_h) p->inq_t = nullptr;
    if (!r->c) {  // conn died before we sent it
      r->on_pg = 0;
      req_free(w, r);
      continue;
    }
    int wr = is_write(r);
    if (in_grp && (wr != wgrp)) {  // group is one class only: a write never
      if (pgw_encode_sync(p)) goto dead;  // joins a read group's transaction
      if (++p->grp_next == 0) p->grp_next = 1;  // never collide with the err_grp=0 sentinel
      in_grp = 0;
    }
    wgrp = wr;
    if (pgw_encode_req(p, r)) goto dead;
    // publish only after encoding: a req in `sent` always owns the
    // next terminal result on the wire
    r->pg_next = nullptr;
    if (p->sent_t) p->sent_t->pg_next = r;
    else p->sent_h = r;
    p->sent_t = r;
    p->sent_n++;
    r->grp = p->grp_next;
    // write groups commit WGRP writes under one Sync (one fsync for the
    // batch); the request still answers only after its Sync's ReadyForQuery
    if (++in_grp == (wgrp ? WGRP : PACK)) {
      if (pgw_encode_sync(p)) goto dead;
      if (++p->grp_next == 0) p->grp_next = 1;
      in_grp = 0;
    }
  }
  if (in_grp) {
    if (pgw_encode_sync(p)) goto dead;
    if (++p->grp_next == 0) p->grp_next = 1;
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
    fprintf(stderr, "pg_down fd=%d sent=%u inq=%p held=%p\n", p->fd,
            p->sent_n, (void*)p->inq_h, (void*)p->held_h);
  if (p->up) {
    if (!w->uring) epoll_ctl(w->ep, EPOLL_CTL_DEL, p->fd, nullptr);
    // uring mode: pending poll CQEs on the dead fd resolve themselves to
    // -ECANCELED and are dropped by the !p->up check in the handler
    p->up = 0;
  }
  pgw_close(p);
  p->in_len = 0;
  p->in_pos = 0;
  p->fld_base = 0;
  p->out_len = p->out_off = 0;
  p->flds.clear();
  p->fcols = 0;
  p->res.nrows = 0;
  p->pgen++;
  p->out_armed = 0;
  p->want_flush = 0;
  p->err_grp = 0;
  p->grp_next = 1;
  p->more = 0;
  p->retry_at = w->now + 2000;
  while (p->held_h) {
    Req* h = p->held_h;
    p->held_h = h->pg_next;
    h->held = 0;
    h->on_pg = 0;
    if (h->c) {
      req_err(h, 500, "internal server error");
      conn_drain(w, h->c);
    } else {
      req_free(w, h);
    }
  }
  p->held_t = nullptr;
  p->err_req = nullptr;
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
  p->inq_n = 0;
  while (p->sent_h) {
    Req* r = p->sent_h;
    p->sent_h = r->pg_next;
    r->on_pg = 0;
    if (r->c) {
      req_err(r, 500, "internal server error");
      conn_drain(w, r->c);
    } else {
      req_free(w, r);
    }
  }
  p->sent_t = nullptr;
  p->sent_n = 0;
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

// Concentrate each class on a sticky conn: a batch's requests merge into
// one Sync group (one round trip, one implicit tx) instead of spreading
// across idle conns at one Sync each. The cursor only advances when the
// current conn's pipeline is full (>= PACK in flight) or down; a least-
// busy sweep across the whole pool is the fallback for conn-loss windows.
void pg_submit(Worker* w, Req* r) {
  int wr = is_write(r);
  // pool layout: reads fill pg[0, npg-nw), writes the last quarter
  int npg = (int)w->pg.size();
  int nw = npg >= 4 ? npg / 4 : 1;
  int nrd = npg - nw;
  int base = wr ? nrd : 0, span = wr ? nw : nrd;
  Pg* best = nullptr;
  if (LIKELY(span > 0)) {
    int rel = wr ? w->pg_wcur : w->pg_rcur;
    for (int i = 0; i < span; i++) {
      int off = (rel + i) % span;
      Pg& p = w->pg[base + off];
      if (p.up && p.sent_n + p.inq_n < PACK) {
        best = &p;
        rel = off;
        break;
      }
    }
    if (wr) w->pg_wcur = rel;
    else w->pg_rcur = rel;
  }
  if (!best) {  // class exhausted: least-busy anywhere
    // ...but never past a hard in-flight ceiling: queued Reqs are ~19KB
    // each, and under a PG stall an unbounded queue is just slow OOM.
    // Beyond the cap a request fails fast instead of waiting minutes.
    size_t bestq = SIZE_MAX;
    for (int i = 0; i < npg; i++) {
      Pg& p = w->pg[i];
      if (!p.up) continue;
      size_t q = p.sent_n + p.inq_n;
      if (q < 256 && q < bestq) {
        bestq = q;
        best = &p;
      }
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
  best->inq_n++;
  // flush at the end of the epoll batch: several submits share one send()
  if (!best->want_flush) {
    best->want_flush = 1;
    w->pg_flush.push_back(best);
  }
}
