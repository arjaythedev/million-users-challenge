// Client connection io: nonblocking read/parse/dispatch, response writes
// (direct send() fast path, buffered fallback), keep-alive LRU tracking,
// and accept.

#include "server.h"

#ifdef HAVE_URING
#include <liburing.h>
#endif

static void lru_push(Worker* w, Conn* c) {
  c->lprev = nullptr;
  c->lnext = w->lh;
  if (w->lh) w->lh->lprev = c;
  else w->lt = c;
  w->lh = c;
}

static void lru_del(Worker* w, Conn* c) {
  if (c->lprev) c->lprev->lnext = c->lnext;
  else w->lh = c->lnext;
  if (c->lnext) c->lnext->lprev = c->lprev;
  else w->lt = c->lprev;
}

static void lru_touch(Worker* w, Conn* c) {
  if (w->lh == c) return;
  lru_del(w, c);
  lru_push(w, c);
}

void ep_mod(Worker* w, int fd, void* ptr, uint32_t ev) {
  epoll_event e;
  e.events = ev;
  e.data.ptr = ptr;
  epoll_ctl(w->ep, EPOLL_CTL_MOD, fd, &e);
}

static void conn_arm_out(Worker* w, Conn* c, bool on) {
  if (c->out_armed == on) return;
  c->out_armed = on;
  ep_mod(w, c->fd, c,
         EPOLLIN | EPOLLRDHUP | (on ? (uint32_t)EPOLLOUT : 0u));
}

static void conn_flush(Worker* w, Conn* c) {
  while (c->woff < c->wlen) {
    ssize_t n = send(c->fd, c->wbuf + c->woff, c->wlen - c->woff,
                     MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      c->woff += n;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      conn_arm_out(w, c, true);
      return;
    }
    if (n < 0 && errno == EINTR) continue;
    conn_close(w, c);
    return;
  }
  c->woff = c->wlen = 0;
  conn_arm_out(w, c, false);
}

// append bytes to the conn write buffer; false if it can't grow right now
static bool wbuf_put(Conn* c, const char* d, uint32_t n) {
  if (c->wlen + n > c->wcap) {
    if (c->wcap > (1u << 20)) return false;  // cap: retry after EPOLLOUT
    uint32_t keep = c->wlen - c->woff;
    uint32_t need = keep + n + 4096;
    uint32_t nc = c->wcap ? c->wcap : 8192;
    while (nc < need) nc *= 2;
    char* nb;
    if (c->woff) {
      nb = (char*)malloc(nc);
      if (nb) memcpy(nb, c->wbuf + c->woff, keep);
      free(c->wbuf);
    } else {
      nb = (char*)realloc(c->wbuf, nc);
    }
    if (!nb) return false;
    c->wbuf = nb;
    c->wcap = nc;
    c->wlen = keep;
    c->woff = 0;
  }
  memcpy(c->wbuf + c->wlen, d, n);
  c->wlen += n;
  return true;
}

// move finished responses out: direct send() when nothing is pending
// (the common case, costs zero buffering), wbuf only on partial writes.
static void conn_drain_ep(Worker* w, Conn* c) {
  if (c->fd < 0) return;
  for (;;) {
    while (c->rq_head && c->rq_head->done) {
      Req* r = c->rq_head;
      const char* d = r->resp;
      uint32_t dl = r->rlen;
      if (c->wlen == c->woff) {
        ssize_t n;
        for (;;) {
          n = send(c->fd, d, dl, MSG_NOSIGNAL | MSG_DONTWAIT);
          if (n < 0 && errno == EINTR) continue;
          break;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          conn_close(w, c);
          return;
        }
        if (n > 0) {
          d += n;
          dl -= n;
        }
        if (dl == 0) goto popped;
      }
      if (dl == 0 || !wbuf_put(c, d, dl)) break;
    popped:
      c->rq_head = r->c_next;
      if (!c->rq_head) c->rq_tail = nullptr;
      req_free(w, r);
    }
    conn_flush(w, c);
    if (c->fd < 0) return;
    if (!c->rq_head || !c->rq_head->done || c->wlen > c->woff) break;
  }
  if (c->closing && !c->rq_head && c->wlen == c->woff) conn_close(w, c);
}

void conn_drain(Worker* w, Conn* c) {
  if (w->uring) {
    conn_drain_ur(w, c);
    return;
  }
  conn_drain_ep(w, c);
}

// mark dead + release resources. The struct itself is freed after the
// current epoll batch (a dispatch in flight may still hold the pointer).
static void conn_close_ep(Worker* w, Conn* c) {
  if (c->fd < 0) return;
  close(c->fd);  // close() also drops the fd from the epoll set
  c->fd = -1;
  w->nconn--;
  lru_del(w, c);
  while (c->rq_head) {
    Req* r = c->rq_head;
    c->rq_head = r->c_next;
    r->c = nullptr;
    if (!r->on_pg && !r->held) req_free(w, r);  // pg/held ones free themselves
  }
  c->rq_tail = nullptr;
  if (c->rbuf && c->rcap == 4096 && w->bfree.size() < 256) w->bfree.push_back(c->rbuf);
  else free(c->rbuf);
  free(c->wbuf);
  c->rbuf = c->wbuf = nullptr;
  c->rlen = c->rpos = c->rcap = 0;
  w->zombies.push_back(c);
}

void conn_close(Worker* w, Conn* c) {
  if (w->uring) {
    conn_close_ur(w, c);
    return;
  }
  conn_close_ep(w, c);
}

static char* rbuf_new(Worker* w, uint32_t cap) {
  if (cap <= 4096 && !w->bfree.empty()) {
    char* p = w->bfree.back();
    w->bfree.pop_back();
    return p;
  }
  return (char*)malloc(cap);
}

// parse all complete requests in buf[..len]; returns consumed bytes
static size_t conn_parse(Worker* w, Conn* c, const char* buf, size_t len) {
  size_t pos = 0;
  while (len - pos >= 8) {  // shortest possible request line
    int method, minor;
    const char *path, *auth, *body;
    size_t plen, authn, blen;
    bool cc, gz;
    long got = http_parse(buf + pos, len - pos, &method, &path, &plen,
                          &auth, &authn, &body, &blen, &minor, &cc, &gz);
    if (got == 0) {
      if (len - pos > 65536 || (len - pos > 16384 && !memchr(buf + pos, '\n', len - pos)))
        conn_close(w, c);  // garbage / header section too large
      break;
    }
    if (got < 0) {
      conn_close(w, c);
      break;
    }
    if (cc) c->closing = 1;
    dispatch(w, c, method, path, plen, auth, authn, body, blen, gz);
    // dispatch may have closed the conn (zombie: memory still valid)
    if (c->fd < 0) return len;
    pos += got;
  }
  return pos;
}

// keep whatever the parser could not consume in a lazily grown heap buffer
static void conn_keep(Worker* w, Conn* c, const char* d, size_t n) {
  if (!n) return;
  if (c->rlen + n > c->rcap) {
    uint32_t nc = c->rcap ? c->rcap : 4096;
    while (nc < c->rlen + n) nc *= 2;
    if (nc > 1 << 20) nc = 1 << 20;
    char* nb = c->rbuf ? (char*)realloc(c->rbuf, nc) : rbuf_new(w, nc);
    if (!nb) return;
    c->rbuf = nb;
    c->rcap = nc;
  }
  memcpy(c->rbuf + c->rlen, d, n);
  c->rlen += n;
}

// parse data that accumulated in c->rbuf, compact the kept tail, and
// release the buffer back to bufferless when fully consumed
static void conn_parse_rbuf(Worker* w, Conn* c) {
  size_t used = conn_parse(w, c, c->rbuf + c->rpos, c->rlen - c->rpos);
  if (c->fd < 0) return;
  c->rpos += used;
  if (c->rpos) {
    memmove(c->rbuf, c->rbuf + c->rpos, c->rlen - c->rpos);
    c->rlen -= c->rpos;
    c->rpos = 0;
  }
  // fully consumed: the conn goes back to bufferless
  if (!c->rlen && c->rbuf) {
    if (c->rcap == 4096 && w->bfree.size() < 256) w->bfree.push_back(c->rbuf);
    else free(c->rbuf);
    c->rbuf = nullptr;
    c->rcap = 0;
  }
}

void conn_on_read(Worker* w, Conn* c) {
  if (!c->rbuf) {
    // common case: the whole request lands in the shared scratch and no
    // per-conn buffer ever exists
    for (;;) {
      ssize_t n = recv(c->fd, w->scratch, sizeof(w->scratch), 0);
      if (n > 0) {
        c->last_ms = w->now;
        size_t used = conn_parse(w, c, w->scratch, (size_t)n);
        if (c->fd < 0) return;
        conn_keep(w, c, w->scratch + used, (size_t)n - used);
        lru_touch(w, c);
        return;
      }
      if (n == 0) {
        conn_close(w, c);
        return;
      }
      if (errno == EINTR) continue;
      return;  // EAGAIN
    }
  }
  for (;;) {
    if (c->rlen == c->rcap) {
      uint32_t nc = c->rcap * 2;
      if (nc > 1 << 20) break;
      char* nb = (char*)realloc(c->rbuf, nc);
      if (!nb) break;
      c->rbuf = nb;
      c->rcap = nc;
    }
    size_t want = c->rcap - c->rlen;
    ssize_t n = recv(c->fd, c->rbuf + c->rlen, want, 0);
    if (n > 0) {
      c->rlen += n;
      c->last_ms = w->now;
      if ((size_t)n < want) break;  // short read: socket drained
      continue;
    }
    if (n == 0) {
      conn_close(w, c);
      return;
    }
    if (errno == EINTR) continue;
    break;  // EAGAIN
  }
  lru_touch(w, c);
  conn_parse_rbuf(w, c);
}

void on_accept(Worker* w, Lfd* l) {
  int taken = 0;
  while (taken++ < 128) {  // a SYN flood can't starve the whole worker
    int fd = accept4(l->fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      return;
    }
    Conn* c;
    if (!w->cfree.empty()) {
      c = w->cfree.back();
      w->cfree.pop_back();
      *c = Conn{};
    } else {
      c = (Conn*)calloc(1, sizeof(Conn));
      if (!c) {
        close(fd);
        return;
      }
    }
    c->ev = EV_CONN;
    c->fd = fd;
    c->last_ms = w->now;
    epoll_event e;
    e.events = EPOLLIN | EPOLLRDHUP;
    e.data.ptr = c;
    if (epoll_ctl(w->ep, EPOLL_CTL_ADD, fd, &e) < 0) {
      close(fd);
      w->cfree.push_back(c);
      continue;
    }
    lru_push(w, c);
    w->nconn++;
  }
}

// ================================================================== io_uring
//
// In uring mode a conn always has at most two ops outstanding: one
// multishot recv (armed once at accept) and one send (staging through
// ur_sbuf so the kernel never references wbuf, which realloc can move).
// ur_inflight counts them; conn_close only marks the conn dead - the
// zombie pass recycles it once every op posted its final CQE.

#ifdef HAVE_URING

constexpr int UR_SBUF = 16384;  // send staging chunk size

static Conn* conn_alloc(Worker* w) {
  Conn* c;
  if (!w->cfree.empty()) {
    c = w->cfree.back();
    w->cfree.pop_back();
    *c = Conn{};
  } else {
    c = (Conn*)calloc(1, sizeof(Conn));
  }
  return c;
}

void ur_recv_arm(Worker* w, Conn* c) {
  io_uring_sqe* s = ur_sqe(w);
  if (!s) {
    if (!c->ur_rxq) {
      c->ur_rxq = 1;
      w->rxrearm.push_back(c);
    }
    return;
  }
  c->ur_rxq = 0;
  io_uring_prep_recv_multishot(s, c->fd, nullptr, 0, 0);
  s->buf_group = UR_BGID;
  s->flags |= IOSQE_BUFFER_SELECT;
  // poll first: the buffer is only picked once data actually arrives, so
  // an idle conn never holds a provided buffer (and arming on a dry ring
  // does not instantly fail with -ENOBUFS)
  s->ioprio |= IORING_RECVSEND_POLL_FIRST;
  s->user_data = (uintptr_t)c | UD_RECV;
  c->ur_inflight++;
}

static char* ur_sbuf_get(Worker* w) {
  if (!w->sfree.empty()) {
    char* p = w->sfree.back();
    w->sfree.pop_back();
    return p;
  }
  return (char*)malloc(UR_SBUF);
}

// stage the head of wbuf into the pinned send chunk and submit a SEND.
// at most one send in flight per conn: responses stay ordered by rq.
void ur_kick_send(Worker* w, Conn* c) {
  if (c->ur_send_pend || c->wlen == c->woff || c->fd < 0) return;
  io_uring_sqe* s = ur_sqe(w);
  if (!s) {
    if (!c->ur_sendq) {
      c->ur_sendq = 1;
      w->sendq.push_back(c);
    }
    return;
  }
  c->ur_sendq = 0;
  uint32_t n = c->wlen - c->woff;
  if (n > UR_SBUF) n = UR_SBUF;
  if (!c->ur_sbuf) c->ur_sbuf = ur_sbuf_get(w);
  memcpy(c->ur_sbuf, c->wbuf + c->woff, n);
  // MSG_WAITALL: the kernel retries short sends internally; res < len
  // then only happens after a partial-progress error
  io_uring_prep_send(s, c->fd, c->ur_sbuf, n, MSG_NOSIGNAL | MSG_WAITALL);
  s->user_data = (uintptr_t)c | UD_SEND;
  c->ur_send_pend = 1;
  c->ur_inflight++;
}

void conn_drain_ur(Worker* w, Conn* c) {
  if (c->fd < 0) return;
  while (c->rq_head && c->rq_head->done) {
    Req* r = c->rq_head;
    const char* d = r->resp;
    uint32_t dl = r->rlen;
    if (c->wlen == c->woff && !c->ur_send_pend) {
      // fast path: direct send() beats an SQE round-trip; gated on
      // !send_pend because a raw send must never overtake the SQE bytes
      ssize_t n;
      for (;;) {
        n = send(c->fd, d, dl, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        break;
      }
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        conn_close_ur(w, c);
        return;
      }
      if (n > 0) {
        d += n;
        dl -= n;
      }
      if (dl == 0) goto popped;
    }
    if (dl == 0 || !wbuf_put(c, d, dl)) break;
  popped:
    c->rq_head = r->c_next;
    if (!c->rq_head) c->rq_tail = nullptr;
    req_free(w, r);
  }
  if (c->wlen == c->woff) c->woff = c->wlen = 0;
  ur_kick_send(w, c);
  if (c->closing && !c->rq_head && c->wlen == c->woff && !c->ur_send_pend)
    conn_close_ur(w, c);
}

void conn_close_ur(Worker* w, Conn* c) {
  if (c->fd < 0) return;
  int fd = c->fd;
  c->fd = -1;
  c->ur_dead = 1;
  w->nconn--;
  lru_del(w, c);
  while (c->rq_head) {
    Req* r = c->rq_head;
    c->rq_head = r->c_next;
    r->c = nullptr;
    if (!r->on_pg && !r->held) req_free(w, r);
  }
  c->rq_tail = nullptr;
  if (c->rbuf && c->rcap == 4096 && w->bfree.size() < 256)
    w->bfree.push_back(c->rbuf);
  else
    free(c->rbuf);
  free(c->wbuf);
  c->rbuf = c->wbuf = nullptr;
  c->rlen = c->rpos = c->rcap = c->woff = c->wlen = c->wcap = 0;
  // close() alone does NOT cancel io_uring requests - they hold a file
  // ref and a silent peer would pin this conn (and keep consuming
  // provided buffers) forever. shutdown() forces every pending op to a
  // terminal CQE, then the zombie drain frees the conn at inflight == 0.
  if (c->ur_inflight) shutdown(fd, SHUT_RDWR);
  close(fd);
  w->zombies.push_back(c);
}

// recv CQE. res > 0 = data in a provided buffer, res == 0 = EOF,
// res < 0 = terminal error (the multishot dies whenever F_MORE is unset)
void ur_on_recv(Worker* w, Conn* c, int res, unsigned flags) {
  bool more = (flags & IORING_CQE_F_MORE) != 0;
  if (!more) c->ur_inflight--;
  if (flags & IORING_CQE_F_BUFFER)
    w->bidret.push_back((uint16_t)(flags >> IORING_CQE_BUFFER_SHIFT));
  if (c->fd < 0) return;  // zombie: accounting + buffer return was enough
  if (res > 0) {
    if (!(flags & IORING_CQE_F_BUFFER)) {
      conn_close(w, c);  // data with no buffer cannot be parsed
      return;
    }
    c->last_ms = w->now;
    const char* buf =
        w->barena + (size_t)(flags >> IORING_CQE_BUFFER_SHIFT) * w->bsz;
    if (!c->rbuf) {
      size_t used = conn_parse(w, c, buf, (size_t)res);
      if (c->fd < 0) return;
      conn_keep(w, c, buf + used, (size_t)res - used);
    } else {
      conn_keep(w, c, buf, (size_t)res);
      conn_parse_rbuf(w, c);
      if (c->fd < 0) return;
    }
    lru_touch(w, c);
    if (!more) ur_recv_arm(w, c);  // multishot fell out; keep it alive
    return;
  }
  if (res == 0) {
    conn_close(w, c);  // peer closed
    return;
  }
  if (res == -ENOBUFS || res == -EAGAIN || res == -EINTR ||
      res == -ECANCELED || res == -ENOMEM) {
    // soft failure: the multishot recv died (F_MORE is unset for these)
    // but the conn is fine - queue a re-arm which runs after this
    // batch's buffers are handed back to the ring
    if (!more && !c->ur_rxq) {
      c->ur_rxq = 1;
      w->rxrearm.push_back(c);
    }
    return;
  }
  conn_close(w, c);
}

void ur_on_send(Worker* w, Conn* c, int res) {
  c->ur_send_pend = 0;
  c->ur_inflight--;
  if (c->ur_sbuf) {
    if (w->sfree.size() < 256) w->sfree.push_back(c->ur_sbuf);
    else free(c->ur_sbuf);
    c->ur_sbuf = nullptr;
  }
  if (c->fd < 0) return;  // zombie drains itself when inflight hits 0
  if (res > 0) {
    c->woff += (uint32_t)res;
    conn_drain_ur(w, c);
    return;
  }
  if (res == -EAGAIN || res == -EINTR || res == -ECANCELED) {
    // the send did not happen (io-wq punt / signal); the staged bytes
    // are still covered by wbuf[woff..wlen), so just requeue the kick
    if (!c->ur_sendq) {
      c->ur_sendq = 1;
      w->sendq.push_back(c);
    }
    return;
  }
  conn_close(w, c);
}

void ur_on_accept(Worker* w, Lfd* l, int res, unsigned flags) {
  if (!(flags & IORING_CQE_F_MORE)) {
    // multishot accept terminated on error or CQ overflow: re-arm or the
    // listener silently stops dequeuing. a res<0 CQE that still has
    // F_MORE is a transient error with the accept still armed - do NOT
    // re-arm or a second multishot piles onto the same listener.
    // on EMFILE/ENFILE make room by reaping the oldest idle conns first.
    if (res == -EMFILE || res == -ENFILE)
      while (w->lt && w->now - w->lt->last_ms > 30000) conn_close(w, w->lt);
    ur_accept_arm(w, l);
  }
  if (res < 0) return;
  Conn* c = conn_alloc(w);
  if (!c) {
    close(res);
    return;
  }
  c->ev = EV_CONN;
  c->fd = res;
  c->last_ms = w->now;
  lru_push(w, c);
  w->nconn++;
  ur_recv_arm(w, c);
}

#else  // !HAVE_URING: stubs so the wrappers link

void conn_drain_ur(Worker*, Conn*) {}
void conn_close_ur(Worker*, Conn*) {}
void ur_on_accept(Worker*, Lfd*, int, unsigned) {}
void ur_on_recv(Worker*, Conn*, int, unsigned) {}
void ur_on_send(Worker*, Conn*, int) {}
void ur_recv_arm(Worker*, Conn*) {}
void ur_kick_send(Worker*, Conn*) {}

#endif
