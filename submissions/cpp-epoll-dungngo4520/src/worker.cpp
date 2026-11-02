// Worker: one per thread. Owns the epoll set, both listen sockets,
// the pg pool, and the client-connection LRU. The main loop dispatches
// events, then frees conns closed during the batch and runs the periodic
// idle sweep / pg reconnect.

#include "server.h"

#include <sched.h>

#ifndef SO_INCOMING_CPU
#define SO_INCOMING_CPU 49
#endif
#ifndef TCP_THIN_LINEAR_TIMEOUTS
#define TCP_THIN_LINEAR_TIMEOUTS 16
#endif
#ifndef TCP_THIN_DUPACK
#define TCP_THIN_DUPACK 17
#endif

static int mk_lfd(const char* host, int port, int cpu) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) die("socket: %m");
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // inherited
  int uto = 80000;
  setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &uto, sizeof(uto));
  setsockopt(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, sizeof(one));
  setsockopt(fd, IPPROTO_TCP, TCP_THIN_DUPACK, &one, sizeof(one));
  // deferred accept: a conn reaches the accept queue only once its first
  // bytes arrive (or after the timeout), merging the accept and the
  // first-request wakeup into one during the connect ramp
  int defer = 10;
  setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof(defer));
  // explicit buffers (inherited by accepted sockets) cap per-conn kernel
  // memory at million-conn scale and disable autotuning
  int rb = 8192, sb = 16384;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb));
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));
  int linger = 30;
  setsockopt(fd, IPPROTO_TCP, TCP_LINGER2, &linger, sizeof(linger));
  if (cpu >= 0) setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(cpu));
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = host ? inet_addr(host) : INADDR_ANY;
  if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) die("bind :%d: %m", port);
  if (listen(fd, 65535) < 0) die("listen: %m");
  return fd;
}

// shared by both backends: pg submit pump, zombie recycle, idle sweep.
// called once per event batch, after all events were dispatched.
void worker_batch_end(Worker* w) {
  // submits queued during this batch go out as one flush per pg conn
  for (Pg* p : w->pg_flush) {
    p->want_flush = 0;
    pg_pump(w, p);
  }
  w->pg_flush.clear();
  // results parked by the dispatch budget resume here, one pass per conn
  // per batch, so a giant Sync group can't stall event dispatch
  if (!w->pg_more.empty()) {
    auto todo = std::move(w->pg_more);
    w->pg_more.clear();
    for (Pg* p : todo) {
      p->more = 0;
      if (p->up) pgw_recv_parse(w, p);
    }
  }
  // conns closed during this batch are safe to recycle now; in uring mode a
  // conn whose ops can still post CQEs has to linger until they drain
  for (size_t i = 0; i < w->zombies.size();) {
    Conn* z = w->zombies[i];
    if (z->ur && z->ur->inflight) {
      i++;
      continue;
    }
    if (z->ur) {
      free(z->ur->sbuf);
      z->ur->sbuf = nullptr;
      if (w->ufree.size() < 65536) w->ufree.push_back(z->ur);
      else free(z->ur);
      z->ur = nullptr;
    }
    w->zombies[i] = w->zombies.back();
    w->zombies.pop_back();
    if (w->cfree.size() < 65536) w->cfree.push_back(z);
    // beyond the freelist cap the arena slot is simply never recycled
  }
  // a listener whose accept arm lost the SQ lottery would otherwise go deaf
  if (w->uring)
    for (int i = 0; i < 2; i++)
      if (w->lfd[i].ur_pend) {
        w->lfd[i].ur_pend = 0;
        ur_accept_arm(w, &w->lfd[i]);
      }
  // idle sweep + pg reconnect, every ~4 s
  if (w->now - w->last_sweep > 4000) {
    w->last_sweep = w->now;
    // SO_INCOMING_CPU goes stale whenever the scheduler migrates the
    // worker (no pinning allowed) - refresh the steering hint
    int cpu = sched_getcpu();
    for (int i = 0; i < 2; i++)
      setsockopt(w->lfd[i].fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu,
                 sizeof(cpu));
    // incremental idle sweep: bounded per pass so a million-conn scan
    // never shows up as one long stall
    uint32_t steps = (uint32_t)w->clist.size() / 8 + 1024;
    while (steps-- && !w->clist.empty()) {
      if (w->cpos >= w->clist.size()) w->cpos = 0;
      Conn* c = w->clist[w->cpos];
      if (w->now - c->last_ms <= g_idle_ms) {
        w->cpos++;
        continue;
      }
      conn_close(w, c);  // swap-removes; same slot needs a recheck
    }
    for (Pg& p : w->pg)
      if (!p.up && w->now >= p.retry_at) {
        p.retry_at = w->now + 5000;
        pg_connect(w, &p);
      }
  }
}

void worker_run(int idx) {
  Worker w;
  // Conn arena: reserved VA, lazily committed, huge-page backed. At
  // million-conn scale a flat THP arena is TLB-resident while a heap of
  // individually calloc'd Conns is a page-walk farm. 512M covers ~5M
  // conns per worker, far past the fd limit.
  w.carena = (char*)mmap(nullptr, 512u << 20, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (w.carena == MAP_FAILED) die("mmap carena: %m");
  madvise(w.carena, 512u << 20, MADV_HUGEPAGE);
  // uring must be set up before pg_connect so it can arm the right backend
  if (g_uring && !ur_setup(&w)) {
    fprintf(stderr, "worker %d: io_uring unavailable, using epoll\n", idx);
    w.uring = 0;
  }
  w.lfd[0].ev = EV_LFD;
  w.lfd[0].fd = mk_lfd(g_host, g_port, sched_getcpu());
  w.lfd[1].ev = EV_LFD;
  w.lfd[1].fd = mk_lfd(g_host, g_xport, sched_getcpu());
  if (!w.uring) {
    w.ep = epoll_create1(EPOLL_CLOEXEC);
    if (w.ep < 0) die("epoll_create1: %m");
    for (int i = 0; i < 2; i++) {
      epoll_event e;
      e.events = EPOLLIN;
      e.data.ptr = &w.lfd[i];
      epoll_ctl(w.ep, EPOLL_CTL_ADD, w.lfd[i].fd, &e);
    }
  }
  w.pg.resize(g_pg_per);
  {
    // last quarter prefers writes: a commit's fsync stall then never holds
    // up the reads pipelined behind it on the same conn
    int npg = (int)w.pg.size();
    int nw = npg >= 4 ? npg / 4 : 1;
    for (int i = npg - nw; i < npg; i++) w.pg[i].wr = 1;
  }
  for (Pg& p : w.pg) pg_connect(&w, &p);  // failures retry in sweep
  w.now = now_ms();
  w.rfree.reserve(256);
  if (w.uring) {
    ur_loop(&w);
    return;
  }

  std::vector<epoll_event> evs(4096);
  for (;;) {
    int n = epoll_wait(w.ep, evs.data(), (int)evs.size(), 1000);
    w.now = now_ms();
    if (n < 0) {
      if (errno == EINTR) continue;
      die("epoll_wait: %m");
    }
    for (int i = 0; i < n; i++) {
      if (i + 1 < n) __builtin_prefetch(evs[i + 1].data.ptr);
      void* ptr = evs[i].data.ptr;
      uint32_t ev = evs[i].events;
      switch (*(int*)ptr) {
        case EV_LFD:
          on_accept(&w, (Lfd*)ptr);
          break;
        case EV_CONN: {
          Conn* c = (Conn*)ptr;
          if (ev & (EPOLLERR | EPOLLHUP)) {
            conn_close(&w, c);
            break;
          }
          if (ev & EPOLLOUT) {
            conn_drain(&w, c);
            if (c->fd < 0) break;
          }
          if (ev & (EPOLLIN | EPOLLRDHUP)) conn_on_read(&w, c);
          break;
        }
        case EV_PG: {
          Pg* p = (Pg*)ptr;
          if (!p->up) break;
          if (ev & EPOLLOUT) {
            pg_on_write(&w, p);
            if (!p->up) break;
          }
          if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP)) pg_on_read(&w, p);
          break;
        }
      }
    }
    worker_batch_end(&w);
  }
}
