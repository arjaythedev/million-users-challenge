// PostgreSQL access: a per-worker pool of libpq connections in pipeline
// mode. Each client request maps to exactly one query (prepared at
// connect time) plus a pipeline sync marker, so results pair 1:1 with the
// sent queue.

#include "server.h"

static const char* PREP[] = {
    "feedq",
    "SELECT p.id, p.body, p.created_at, u.username,"
    " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count"
    " FROM posts p JOIN users u ON u.id = p.user_id"
    " ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
    "postq",
    "SELECT p.id, p.body, p.created_at, u.username,"
    " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count"
    " FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1",
    "insq",
    "INSERT INTO posts (user_id, body) VALUES ($1, $2)"
    " RETURNING id, created_at",
    "likeq",
    "INSERT INTO likes (user_id, post_id) VALUES ($1, $2)"
    " ON CONFLICT (user_id, post_id) DO NOTHING",
    "healthq",
    "SELECT 1",
};

static void pg_down(Worker* w, Pg* p);

bool pg_connect(Worker* w, Pg* p) {
  p->db = PQconnectdb(g_dburl);
  if (PQstatus(p->db) != CONNECTION_OK) {
    PQfinish(p->db);
    p->db = nullptr;
    return false;
  }
  PQsetnonblocking(p->db, 1);
  PGresult* r = PQexec(
      p->db,
      "SET TIME ZONE 'UTC'; SET plan_cache_mode = force_generic_plan");
  PQclear(r);
  for (size_t i = 0; i + 1 < sizeof(PREP) / sizeof(PREP[0]); i += 2) {
    r = PQprepare(p->db, PREP[i], PREP[i + 1], 0, nullptr);
    ExecStatusType st = r ? PQresultStatus(r) : PGRES_FATAL_ERROR;
    PQclear(r);
    if (st != PGRES_COMMAND_OK) {
      PQfinish(p->db);
      p->db = nullptr;
      return false;
    }
  }
  if (PQenterPipelineMode(p->db) != 1) {
    PQfinish(p->db);
    p->db = nullptr;
    return false;
  }
  p->fd = PQsocket(p->db);
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

// pop one sent request per completed result. Sync markers release write
// results held for their implicit commit: CommandComplete arrives before
// the Sync, but the commit (and WAL flush) only happens at the Sync, and
// the rules want writes committed before responding.
// libpq reads from the socket inside PQsend*/PQflush calls, so results can
// be sitting in userspace with no EPOLLIN pending: callers must run this
// after any interaction with the PGconn, not only on read events.
static void pg_drain(Worker* w, Pg* p) {
  for (;;) {
    if (PQisBusy(p->db)) break;
    PGresult* res = PQgetResult(p->db);
    if (!res) {
      // NULL ends one command's result stream; the next queued result (a
      // pipeline SYNC follows every boundary NULL) needs another call, and
      // an empty stream just keeps returning NULL
      if (PQisBusy(p->db)) break;
      res = PQgetResult(p->db);
      if (!res) break;
    }
    if (PQresultStatus(res) == PGRES_PIPELINE_SYNC) {
      PQclear(res);
      if (p->held) {
        Req* h = p->held;
        p->held = nullptr;
        h->held = 0;
        if (h->c) conn_drain(w, h->c);
        else req_free(w, h);
      }
      continue;
    }
    if (p->sent.empty()) {
      PQclear(res);
      continue;
    }
    Req* r = p->sent.front();
    p->sent.pop_front();
    if (r->c && (r->type == R_CREATE || r->type == R_LIKE)) {
      r->held = 1;
      p->held = r;
    }
    req_pg_done(w, r, res);
    PQclear(res);
  }
}

// send every queued request: one prepared query + pipeline sync each
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
    char a[24], b[24];
    const char* pv[2];
    int ok;
    switch (r->type) {
      case R_FEED:
        ok = PQsendQueryPrepared(p->db, "feedq", 0, nullptr, nullptr, nullptr, 0);
        break;
      case R_GET:
        a[sizeof(a) - 1] = 0;
        pv[0] = u64toa((uint64_t)r->id, a + sizeof(a) - 1);
        ok = PQsendQueryPrepared(p->db, "postq", 1, pv, nullptr, nullptr, 0);
        break;
      case R_CREATE:
        a[sizeof(a) - 1] = 0;
        pv[0] = u64toa((uint64_t)r->uid, a + sizeof(a) - 1);
        pv[1] = r->body;
        ok = PQsendQueryPrepared(p->db, "insq", 2, pv, nullptr, nullptr, 0);
        break;
      case R_LIKE:
        a[sizeof(a) - 1] = 0;
        b[sizeof(b) - 1] = 0;
        pv[0] = u64toa((uint64_t)r->uid, a + sizeof(a) - 1);
        pv[1] = u64toa((uint64_t)r->id, b + sizeof(b) - 1);
        ok = PQsendQueryPrepared(p->db, "likeq", 2, pv, nullptr, nullptr, 0);
        break;
      case R_HEALTH:
      default:
        ok = PQsendQueryPrepared(p->db, "healthq", 0, nullptr, nullptr, nullptr, 0);
        break;
    }
    if (ok != 1 || PQsendPipelineSync(p->db) != 1) {
      // conn is broken; drop everything
      pg_down(w, p);
      return;
    }
    p->sent.push_back(r);
  }
  int f = PQflush(p->db);
  if (f < 0) {
    pg_down(w, p);
    return;
  }
  pg_arm_out(w, p, f == 1);
  pg_drain(w, p);
}

static void pg_down(Worker* w, Pg* p) {
  if (p->db) {
    if (!w->uring) epoll_ctl(w->ep, EPOLL_CTL_DEL, p->fd, nullptr);
    // uring mode: pending poll CQEs on the dead fd resolve themselves to
    // -ECANCELED and are dropped by the !p->db check in the handler
    PQfinish(p->db);
    p->db = nullptr;
  }
  p->fd = -1;
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

void pg_on_read(Worker* w, Pg* p) {
  if (!PQconsumeInput(p->db)) {
    pg_down(w, p);
    return;
  }
  pg_drain(w, p);
}

void pg_on_write(Worker* w, Pg* p) {
  int f = PQflush(p->db);
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
    if (!p.db) continue;
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
