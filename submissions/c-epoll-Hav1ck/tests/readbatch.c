#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static jmp_buf finished;
static int waits, sleeps, clocks, read_count, send_mode, send_calls;
static int64_t fake_ns, elapsed_ns, initial_delay;
static unsigned char transcript[2][8192], expected[2][8192];
static size_t sent[2], expected_size[2];
static int fixture_wait(struct epoll_event *events, int limit, int64_t delay);
static int fixture_sleep(const struct timespec *request, struct timespec *remaining);

static int fixture_clock(clockid_t clock, struct timespec *result) {
  assert(clock == CLOCK_MONOTONIC);
  if (++clocks == 3) fake_ns += elapsed_ns;
  result->tv_sec = fake_ns / 1000000000LL;
  result->tv_nsec = fake_ns % 1000000000LL;
  return 0;
}

static long fixture_syscall(long number, ...) {
#ifdef SYS_epoll_pwait2
  assert(number == SYS_epoll_pwait2);
  va_list arguments;
  va_start(arguments, number);
  (void)va_arg(arguments, int);
  struct epoll_event *events = va_arg(arguments, struct epoll_event *);
  int limit = va_arg(arguments, int);
  const struct timespec *delay = va_arg(arguments, const struct timespec *);
  va_end(arguments);
  return fixture_wait(events, limit, delay->tv_sec * 1000000000LL + delay->tv_nsec);
#else
  (void)number;
  abort();
#endif
}

static int fixture_epoll_wait(int ep, struct epoll_event *events, int limit, int delay) {
  (void)ep;
  return fixture_wait(events, limit, (int64_t)delay * 1000000LL);
}

static int fixture_epoll_ctl(int ep, int operation, int fd, struct epoll_event *event) {
  (void)ep; (void)operation; (void)event;
  assert(fd == 100 || fd == 103);
  return 0;
}

static ssize_t fixture_write(int fd, const void *bytes, size_t length) {
  assert(fd == 100 || fd == 103);
  int lane = fd == 103;
  if (!lane && send_calls++ == 0) {
    if (send_mode == 1) length = length > 7 ? 7 : length;
    if (send_mode == 2) { errno = EAGAIN; return -1; }
  }
  assert(sent[lane] + length <= sizeof transcript[lane]);
  memcpy(transcript[lane] + sent[lane], bytes, length);
  sent[lane] += length;
  return (ssize_t)length;
}

#define NO_IDLE
#define clock_gettime fixture_clock
#define syscall fixture_syscall
#define epoll_wait fixture_epoll_wait
#define epoll_ctl fixture_epoll_ctl
#define nanosleep fixture_sleep
#define write fixture_write
#define main frozen_server_main
#include "../server.c"
#undef main

_Static_assert(READ_BATCH_NS == 0 || READ_BATCH_NS == 2000000LL, "isolated read deadline");
_Static_assert(WRITE_BATCH_NS == 2000000LL && LOOP_BATCH_NS == 2000000LL, "unchanged pacing");
_Static_assert(PACK == 64 && GROUP == 128 && FOUR_CORE_WORKERS == 2 && PG_FIRST == 1, "unchanged policy");
#ifndef NO_HEDGE
#error "retain submitted NO_HEDGE"
#endif

static th_t worker;
static pg_t lanes[4];

static void snapshot(int index, pg_t *lane) {
  assert(lane->olen + 5 <= sizeof expected[index]);
  memcpy(expected[index], lane->ob, lane->olen);
  expected_size[index] = lane->olen;
  if (lane->unsynced) {
    memcpy(expected[index] + expected_size[index], "S\0\0\0\4", 5);
    expected_size[index] += 5;
  }
}

static void first_pass(void) {
  pg_t *reader = &lanes[0], *writer = &lanes[3];
  int read_due = READ_BATCH_NS == 0 || read_count == PACK || elapsed_ns >= READ_BATCH_NS;
  int write_due = elapsed_ns >= WRITE_BATCH_NS;
  assert(writer->flush_at == 1000000000LL + WRITE_BATCH_NS);
  assert(writer->dirty == !write_due && writer->unsynced == !write_due);
  assert(writer->n == (unsigned)(write_due ? 2 : 1));
  assert(sent[1] == (write_due ? expected_size[1] : 0));
  if (read_due) {
    assert(!reader->dirty && !reader->unsynced && reader->n == (unsigned)read_count + 1);
    assert(reader->wout == (send_mode != 0));
    assert(sent[0] == (send_mode == 2 ? 0 : send_mode == 1 ? 7 : expected_size[0]));
  } else {
    assert(reader->dirty && reader->unsynced && reader->n == (unsigned)read_count);
    assert(!sent[0] && reader->flush_at == 1000000000LL + READ_BATCH_NS);
  }
}

static int fixture_wait(struct epoll_event *events, int limit, int64_t delay) {
  assert(limit == NEV);
  if (waits++ == 0) {
    assert(worker.npg == 0);
    worker.npg = 4;
    initial_delay = delay;
    now_ns = fake_ns;
    for (int count = 0; count < read_count; count++)
      assert(enqueue(&worker, 1, Q_HEALTH, 0, 0, 0, NULL, 0, NULL, 0));
    assert(enqueue(&worker, 1, Q_LIKE, 0, 10, 1, NULL, 0, NULL, 0));
    snapshot(0, &lanes[0]); snapshot(1, &lanes[3]);
    events[0] = (struct epoll_event){.events = EPOLLIN, .data.u64 = 0};
    return 1;
  }
  if (waits == 2) {
    if (!sleeps) first_pass();
    if (send_mode) {
      assert(delay == 0 && lanes[0].wout);
      events[0] = (struct epoll_event){.events = EPOLLOUT, .data.u64 = TAG_PG};
      return 1;
    }
  }
  longjmp(finished, 1);
}

static int fixture_sleep(const struct timespec *request, struct timespec *remaining) {
  (void)remaining;
  if (!sleeps++) first_pass();
  int64_t duration = request->tv_sec * 1000000000LL + request->tv_nsec;
  assert(duration > 0 && duration <= LOOP_BATCH_NS);
  fake_ns += duration;
  return 0;
}

static void trial(int count, int mode, int64_t elapsed) {
  memset(&worker, 0, sizeof worker); memset(lanes, 0, sizeof lanes);
  memset(sent, 0, sizeof sent); memset(expected_size, 0, sizeof expected_size);
  for (int index = 0; index < 4; index++) lanes[index].sock = 100 + index;
  worker.pg = lanes; worker.wd_at = INT64_MAX;
  fake_ns = 1000000000LL; elapsed_ns = elapsed;
  read_count = count; send_mode = mode; waits = sleeps = clocks = send_calls = 0;
  if (!setjmp(finished)) run(&worker);
  assert(initial_delay == 50000000LL);
  assert(sleeps == (elapsed ? 0 : mode ? 2 : 1));
  for (int index = 0; index < 2; index++) {
    if (sent[index]) {
      assert(sent[index] == expected_size[index]);
      assert(!memcmp(transcript[index], expected[index], sent[index]));
    }
  }
  if (mode) assert(!lanes[0].wout && !lanes[0].olen && !lanes[0].ooff);
  for (int index = 0; index < 4; index++) { free(lanes[index].ob); free(lanes[index].q); }
  free(arena); free(zbuf); arena = zbuf = NULL;
}

int main(void) {
  maxfd = 2; conns = calloc(maxfd, sizeof *conns); assert(conns);
  trial(1, 0, 0);
  trial(PACK, 0, 0);
  trial(1, 0, WRITE_BATCH_NS);
  if (READ_BATCH_NS == 0) { trial(1, 1, 0); trial(1, 2, 0); }
  free(conns); conns = NULL;
  puts("ok actual run/enqueue/Sync/flush: sparse/full/mixed/deadline/partial/EAGAIN/EPOLLOUT; inherited regression gates commit");
  return 0;
}
