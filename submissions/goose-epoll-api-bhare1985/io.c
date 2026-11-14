// The sockets under server.goose: one level-triggered epoll set per worker thread, behind a small event interface.
//
// Goose sees a flat array of events (client accepted, bytes received, response sent, Postgres socket ready) and calls
// send, close and poll. A client is a slot number; slots are dense, so the client table in Goose stays small. One read per
// ready client, one write per response; what the socket will not take waits for EPOLLOUT. The pause that batches light
// iterations (let events pile up for a moment, so one wake-up serves many requests and one Postgres Sync covers many reads)
// is in io_wait.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { EV_ACCEPT = 1, EV_RECV = 2, EV_SEND = 3, EV_POLL = 4 };
typedef struct { uint32_t kind, id; int32_t res; uint32_t off; } event_t;     // the layout of io::Event in io.goose

#define MAX_PG 256

static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }

static int listen_on(const char *host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   // inherited by the accepted sockets
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { fprintf(stderr, "bad HOST %s\n", host); exit(1); }
  if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 65535)) { perror("bind/listen"); exit(1); }
  return fd;
}

#define EP_BATCH 512
#define EP_READ 16384
enum { TAG_CLIENT = 0, TAG_LISTEN = 1, TAG_PG = 2 };
static inline uint64_t tag(uint32_t kind, uint32_t id) { return (uint64_t)kind << 32 | id; }

typedef struct { int32_t fd; uint8_t live, notify; uint8_t pad[2]; uint8_t *out; uint32_t out_len, out_pos; } eslot_t;

static __thread int ep;
static __thread int elisteners[2], nelisteners;
static __thread eslot_t *eslots;                          // by slot
static __thread uint32_t eslots_cap, eslots_used;
static __thread uint32_t *efree;                          // slots free for reuse
static __thread uint32_t nefree;
static __thread int64_t paused_until;
static __thread int32_t last_n;
static __thread struct { int fd; uint32_t mask; uint8_t armed; } epolls[MAX_PG];

static void ep_ctl(int op, int fd, uint64_t data, uint32_t events) {
  struct epoll_event e = {.events = events, .data.u64 = data};
  epoll_ctl(ep, op, fd, &e);
}

static void ep_watch_listeners(int on) {
  for (int i = 0; i < nelisteners; i++) ep_ctl(on ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, elisteners[i], tag(TAG_LISTEN, i), EPOLLIN);
}

int32_t io_start(const char *host, int32_t port1, int32_t port2) {
  ep = epoll_create1(EPOLL_CLOEXEC);
  if (ep < 0) return -errno;
  elisteners[nelisteners++] = listen_on(host, port1);
  if (port2 >= 0) elisteners[nelisteners++] = listen_on(host, port2);
  ep_watch_listeners(1);
  return 0;
}

static void eslot_drop(uint32_t slot) {
  eslot_t *s = &eslots[slot];
  free(s->out);
  close(s->fd);                                           // also leaves the epoll set
  memset(s, 0, sizeof *s);
  s->fd = -1;
  efree[nefree++] = slot;                                 // room: efree is as large as eslots
}

static uint32_t eslot_new(int fd) {
  uint32_t slot;
  if (nefree) slot = efree[--nefree];
  else {
    if (eslots_used == eslots_cap) {
      uint32_t cap = eslots_cap ? eslots_cap * 2 : 64;
      eslots = realloc(eslots, sizeof(eslot_t) * cap);
      efree = realloc(efree, sizeof(uint32_t) * cap);
      eslots_cap = cap;
    }
    slot = eslots_used++;
  }
  memset(&eslots[slot], 0, sizeof(eslot_t));
  eslots[slot].fd = fd; eslots[slot].live = 1;
  return slot;
}

// 1: all out already; 0: the rest waits for the socket (a SEND event follows if notify); -1: the connection failed.
int32_t io_send(uint32_t slot, const uint8_t *buf, int64_t n, uint8_t notify) {
  eslot_t *s = &eslots[slot];
  if (!s->live) return -1;
  if (s->out) {                                           // an earlier response is still waiting: queue behind it
    s->out = realloc(s->out, s->out_len + (size_t)n);
    memcpy(s->out + s->out_len, buf, (size_t)n);
    s->out_len += (uint32_t)n; s->notify |= notify;
    return 0;
  }
  ssize_t w = write(s->fd, buf, (size_t)n);
  if (w < 0) { if (errno != EAGAIN && errno != EINTR) return -1; w = 0; }
  if (w == n) return 1;
  s->out = malloc((size_t)(n - w));
  memcpy(s->out, buf + w, (size_t)(n - w));
  s->out_len = (uint32_t)(n - w); s->out_pos = 0; s->notify = notify;
  ep_ctl(EPOLL_CTL_MOD, s->fd, tag(TAG_CLIENT, slot), EPOLLOUT);
  return 0;
}

void io_close(uint32_t slot) { if (eslots[slot].live) eslot_drop(slot); }

void io_poll(int32_t i, int32_t fd, uint32_t mask) {
  if (epolls[i].armed && epolls[i].fd == fd && epolls[i].mask == mask) return;
  ep_ctl(epolls[i].armed && epolls[i].fd == fd ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, tag(TAG_PG, i), mask);
  epolls[i].fd = fd; epolls[i].mask = mask; epolls[i].armed = 1;
}

void io_poll_cancel(int32_t i) {
  if (!epolls[i].armed) return;
  ep_ctl(EPOLL_CTL_DEL, epolls[i].fd, 0, 0);
  epolls[i].armed = 0;
}

int32_t io_wait(event_t *ev, int32_t max, uint8_t *inbox, int64_t cap, int32_t min_events, int64_t timeout_us) {
  if (paused_until && now_us() >= paused_until) { paused_until = 0; ep_watch_listeners(1); }
  int timeout_ms = 1000;
  if (min_events > 1) {                                   // after a light iteration let events pile up for a moment
    if (last_n > 0 && last_n < min_events) { struct timespec ts = {timeout_us / 1000000, (timeout_us % 1000000) * 1000}; nanosleep(&ts, NULL); }
  } else timeout_ms = (int)(timeout_us / 1000);
  struct epoll_event evs[EP_BATCH];
  int got = epoll_wait(ep, evs, EP_BATCH, timeout_ms);
  int n = 0;
  int64_t used = 0;
  for (int k = 0; k < got && n < max; k++) {
    uint64_t d = evs[k].data.u64;
    uint32_t id = (uint32_t)d, events = evs[k].events;
    switch (d >> 32) {
    case TAG_LISTEN:
      for (int a = 0; a < 256 && n < max; a++) {
        int fd = accept4(elisteners[id], NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
          if (errno == EMFILE || errno == ENFILE) { ep_watch_listeners(0); paused_until = now_us() + 1000000; }
          break;
        }
        uint32_t slot = eslot_new(fd);
        ep_ctl(EPOLL_CTL_ADD, fd, tag(TAG_CLIENT, slot), EPOLLIN);
        ev[n++] = (event_t){EV_ACCEPT, slot, 0, 0};
      }
      break;
    case TAG_PG:
      ev[n++] = (event_t){EV_POLL, id, (int32_t)events, 0};
      break;
    default: {
      eslot_t *s = &eslots[id];
      if (!s->live) break;
      if (s->out && (events & EPOLLOUT)) {
        ssize_t w = write(s->fd, s->out + s->out_pos, s->out_len - s->out_pos);
        if (w < 0) {
          if (errno == EAGAIN || errno == EINTR) break;
          if (s->notify) ev[n++] = (event_t){EV_SEND, id, -1, 0};
          free(s->out); s->out = NULL;
          break;
        }
        s->out_pos += (uint32_t)w;
        if (s->out_pos < s->out_len) break;
        int notify = s->notify;
        uint32_t len = s->out_len;
        free(s->out); s->out = NULL; s->out_len = s->out_pos = 0; s->notify = 0;
        ep_ctl(EPOLL_CTL_MOD, s->fd, tag(TAG_CLIENT, id), EPOLLIN);
        if (notify) ev[n++] = (event_t){EV_SEND, id, (int32_t)len, 0};
      } else if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        if (cap - used < EP_READ) break;                  // no room in the inbox: level-triggered, so it comes again
        ssize_t r = read(s->fd, inbox + used, EP_READ);
        if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (r <= 0) { ev[n++] = (event_t){EV_RECV, id, r == 0 ? 0 : -errno, 0}; break; }
        ev[n++] = (event_t){EV_RECV, id, (int32_t)r, (uint32_t)used};
        used += r;
      }
    }
    }
  }
  last_n = n;
  return n;
}

