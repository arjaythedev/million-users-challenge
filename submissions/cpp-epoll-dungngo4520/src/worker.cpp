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
  // results parked by the dispatch budget resume here, a few passes per
  // batch so a giant Sync group can't stall event dispatch - and never
  // wait for the next batch: buffered frames would otherwise stall up to
  // the epoll timeout between passes
  int rpass = 4;
  while (!w->pg_more.empty() && rpass--) {
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
      if (w->now - (uint64_t)c->last_s * 1000 <= g_idle_ms) {
        w->cpos++;
        continue;
      }
      conn_close(w, c);  // swap-removes; same slot needs a recheck
    }
    // reconnects are serialized inside the event loop and each handshake
    // can block ~10-20s on a blackholed host: bound the work per pass so
    // a full-pool outage can't freeze the worker for minutes
    int retries = 2;
    for (Pg& p : w->pg) {
      if (!p.up && w->now >= p.retry_at) {
        // stagger retries so a pool-wide outage doesn't reconnect as a herd
        p.retry_at = w->now + 5000 + (uint64_t)(&p - w->pg.data()) * 997;
        pg_connect(w, &p);
        if (!--retries) break;
      }
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
  // Req arena: freelist refills bump-carve here instead of malloc. 512M
  // covers ~27k live Reqs; calloc stays the overflow path beyond that.
  w.rarena = (char*)mmap(nullptr, 512u << 20, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (w.rarena == MAP_FAILED) die("mmap rarena: %m");
  madvise(w.rarena, 512u << 20, MADV_HUGEPAGE);
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
  g_now_s.store((uint64_t)time(nullptr), std::memory_order_relaxed);
  w.rfree.reserve(256);
  if (w.uring) {
    ur_loop(&w);
    return;
  }

  std::vector<epoll_event> evs(4096);
  for (;;) {
    // parked result work means immediate reservice, not a 1s block
    int n = epoll_wait(w.ep, evs.data(), (int)evs.size(),
                       w.pg_more.empty() ? 1000 : 0);
    w.now = now_ms();
    // second-resolution: publish only on change - an unconditional store
    // makes every worker's batch ping-pong the same cache line
    uint64_t ts = (uint64_t)time(nullptr);
    if (ts != w.pub_s) {
      w.pub_s = ts;
      g_now_s.store(ts, std::memory_order_relaxed);
    }
    if (UNLIKELY(n < 0)) {
      if (errno == EINTR) continue;
      die("epoll_wait: %m");
    }
    int saw_lfd = 0;
    w.batch_reqs = 0;
    for (int i = 0; i < n; i++) {
      if (i + 1 < n) __builtin_prefetch(evs[i + 1].data.ptr);
      void* ptr = evs[i].data.ptr;
      uint32_t ev = evs[i].events;
      int tag = *(uint8_t*)ptr;
      if (LIKELY_P(tag == EV_CONN, 0.97)) {
        Conn* c = (Conn*)ptr;
        if (UNLIKELY(ev & (EPOLLERR | EPOLLHUP))) {
          conn_close(&w, c);
          continue;
        }
        if (UNLIKELY_P(ev & EPOLLOUT, 0.15)) {
          conn_drain(&w, c);
          if (UNLIKELY(c->fd < 0)) continue;
        }
        if (LIKELY(ev & (EPOLLIN | EPOLLRDHUP))) conn_on_read(&w, c);
        continue;
      }
      if (tag == EV_LFD) {
        on_accept(&w, (Lfd*)ptr);
        saw_lfd = 1;
        continue;
      }
      if (tag == EV_PG) {
        Pg* p = (Pg*)ptr;
        if (UNLIKELY(!p->up)) continue;
        if (ev & EPOLLOUT) {
          pg_on_write(&w, p);
          if (!p->up) continue;
        }
        if (ev & (EPOLLIN | EPOLLERR | EPOLLHUP)) pg_on_read(&w, p);
      }
    }
    worker_batch_end(&w);
    // Sparse request supply: sleeping briefly lets arrivals accumulate so
    // each wakeup dispatches a real batch - requests merge into one Sync
    // group per pump instead of one each, and per-wakeup costs amortize.
    // The gate counts produced requests, not raw events: PG-side event
    // churn must not suppress batching. Never pause with accepts in
    // flight, parked results pending, or a genuine event flood.
    if (w.batch_reqs < 64 && n < 256 && !saw_lfd && w.pg_more.empty()) {
      struct timespec ts = {0, 6000000};  // 6ms
      nanosleep(&ts, nullptr);
    }
  }
}
