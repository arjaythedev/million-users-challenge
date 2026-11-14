// Run the actual worker loop with deterministic clock/wait/sleep boundaries.
// No sockets, database, scheduler changes or wall-clock timing assertions.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static jmp_buf finished;
static int events_returned, waits, sleeps, clock_reads, interrupt_once;
static int64_t fake_ns, work_ns, requested_ns[2], wait_delay_ns;
static int fake_clock_gettime(clockid_t clock, struct timespec *ts) {
  assert(clock == CLOCK_MONOTONIC);
  if (++clock_reads == 3) fake_ns += work_ns;
  ts->tv_sec = fake_ns / 1000000000LL;
  ts->tv_nsec = fake_ns % 1000000000LL;
  return 0;
}
static int fake_wait(struct epoll_event *ev, int limit, int64_t delay) {
  wait_delay_ns = delay;
  if (waits++) longjmp(finished, 1);
  assert(events_returned <= limit);
  for (int i = 0; i < events_returned; i++) {
    ev[i].events = EPOLLIN; ev[i].data.u64 = 1; // closed client: ignored
  }
  return events_returned;
}
static long fake_syscall(long number, ...) {
#ifdef SYS_epoll_pwait2
  assert(number == SYS_epoll_pwait2);
  va_list ap; va_start(ap, number);
  (void)va_arg(ap, int);
  struct epoll_event *events = va_arg(ap, struct epoll_event *);
  int limit = va_arg(ap, int);
  const struct timespec *delay = va_arg(ap, const struct timespec *);
  int64_t ns = delay->tv_sec * 1000000000LL + delay->tv_nsec;
  va_end(ap);
  return fake_wait(events, limit, ns);
#else
  (void)number; abort();
#endif
}
static int fake_epoll_wait(int ep, struct epoll_event *events, int limit, int delay) {
  (void)ep; return fake_wait(events, limit, (int64_t)delay * 1000000LL);
}
static int fake_nanosleep(const struct timespec *req, struct timespec *rem) {
  int64_t ns = req->tv_sec * 1000000000LL + req->tv_nsec;
  assert(ns > 0 && sleeps < 2);
  requested_ns[sleeps++] = ns;
  if (interrupt_once) {
    interrupt_once = 0;
    int64_t remaining = ns / 2;
    fake_ns += ns - remaining;
    rem->tv_sec = remaining / 1000000000LL;
    rem->tv_nsec = remaining % 1000000000LL;
    errno = EINTR; return -1;
  }
  fake_ns += ns; return 0;
}
#define NO_IDLE
#define clock_gettime fake_clock_gettime
#define epoll_wait fake_epoll_wait
#define syscall fake_syscall
#define nanosleep fake_nanosleep
#define main feed_server_main
#include "../server.c"
#undef main

static th_t worker;
static void trial(int events, int64_t work, int interrupted, int expected_sleeps) {
  memset(&worker, 0, sizeof worker);
  fake_ns = 1000000000LL; work_ns = work;
  waits = sleeps = clock_reads = 0; interrupt_once = interrupted;
  memset(requested_ns, 0, sizeof requested_ns);
  events_returned = events;
  if (!setjmp(finished)) run(&worker);
  assert(waits == 2 && sleeps == expected_sleeps);
  assert(wait_delay_ns == 50000000LL); // idle wait retains its blocking timeout
  if (expected_sleeps) {
    assert(requested_ns[0] == LOOP_BATCH_NS - work);
    assert(fake_ns == 1000000000LL + LOOP_BATCH_NS);
    if (interrupted) assert(requested_ns[1] == requested_ns[0] / 2);
  } else assert(fake_ns == 1000000000LL + work);
  free(arena); free(zbuf); arena = zbuf = NULL;
}
int main(void) {
  maxfd = 2; conns = calloc(maxfd, sizeof *conns); assert(conns);
  trial(0, 0, 0, 0);                   // idle: no extra polling/sleep
  trial(NEV, 0, 0, 0);                 // full batch drains immediately
  trial(1, 500000, 0, 1);              // partial turn sleeps only remainder
  trial(NEV - 1, 0, 1, 2);             // EINTR resumes remaining duration
  trial(1, LOOP_BATCH_NS, 0, 0);        // already spent budget
  trial(1, LOOP_BATCH_NS + 1000, 0, 0); // over budget: no negative sleep
  free(conns); conns = NULL;
  puts("ok cadence: actual worker idle/full/partial/elapsed/EINTR paths");
  return 0;
}
