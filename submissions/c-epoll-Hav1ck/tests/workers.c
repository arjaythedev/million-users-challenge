#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static jmp_buf startup_done;
static int available_cpus, sparse_affinity, created_workers, wall_reads;
static void *(*worker_entries[256])(void *);
static void *worker_arguments[256];
static int fake_affinity(pid_t process, size_t size, cpu_set_t *affinity) {
  assert(process == 0 && size == sizeof *affinity);
  CPU_ZERO(affinity);
  for (int index = 0; index < available_cpus; index++)
    CPU_SET(sparse_affinity ? CPU_SETSIZE - index - 1 : index, affinity);
  return 0;
}
static int fake_getrlimit(__rlimit_resource_t resource, struct rlimit *limit) {
  assert(resource == RLIMIT_NOFILE);
  limit->rlim_cur = limit->rlim_max = 64;
  return 0;
}
static int fake_setrlimit(__rlimit_resource_t resource, const struct rlimit *limit) {
  (void)resource; (void)limit;
  abort();
}
static char *fake_getenv(const char *name) {
  if (!strcmp(name, "DATABASE_URL")) return "postgres://test:test@127.0.0.1:1/test";
  if (!strcmp(name, "JWT_SECRET")) return "round11-worker-test-secret";
  if (!strcmp(name, "EXTRA_PORT") && sparse_affinity) return "3001";
  return NULL;
}
static time_t fake_time(time_t *result) {
  assert(!result);
  if (++wall_reads == 2) longjmp(startup_done, 1);
  return 2000000000;
}
static int fake_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*entry)(void *), void *argument);

#define NO_IDLE
#define sched_getaffinity fake_affinity
#define getrlimit fake_getrlimit
#define setrlimit fake_setrlimit
#define getenv fake_getenv
#define time fake_time
#define pthread_create fake_pthread_create
#define main experiment_server_main
#include "../server.c"
#undef main

static int fake_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                               void *(*entry)(void *), void *argument) {
  assert(thread && !attributes && entry == run);
  created_workers++;
  assert(created_workers < 256);
  worker_entries[created_workers] = entry;
  worker_arguments[created_workers] = argument;
  th_t *worker = argument;
  assert(worker == &g_ts[created_workers] && worker->ep >= 0);
  assert(worker->npg == npg_per && worker->pg);
  for (int index = 0; index < worker->npg; index++) assert(worker->pg[index].sock == -1);
  return 0;
}

static void lane_case(th_t *worker) {
  int writers = worker->npg / 4 ? worker->npg / 4 : 1;
  int readers = worker->npg - writers;
  assert(!pick(worker, 0) && !pick(worker, 1));
  for (int index = 0; index < worker->npg; index++) worker->pg[index].sock = 0;
  for (int lane = 0; lane < 2; lane++) {
    int start = lane ? readers : 0;
    int count = lane ? writers : readers;
    worker->pick_next[lane] = 0;
    for (int index = 0; index < count * 2; index++)
      assert(pick(worker, lane) == &worker->pg[start + index % count]);
    for (int index = start; index < start + count; index++) worker->pg[index].wout = 1;
    assert(!pick(worker, lane));
    pg_t *other = pick(worker, !lane);
    assert(other && (lane ? other < worker->pg + readers : other >= worker->pg + readers));
    for (int index = start; index < start + count; index++) worker->pg[index].wout = 0;
  }
  for (int index = 0; index < worker->npg; index++) worker->pg[index].sock = -1;
  assert(!pick(worker, 0) && !pick(worker, 1));
}

static void release_buffers(void) {
  assert(arena && zbuf);
  free(arena); arena = NULL;
  free(zbuf); zbuf = NULL;
}

static void worker_entry_case(int worker_index) {
  wall_reads = 1;
  assert(!arena && !zbuf);
  if (!setjmp(startup_done)) {
    worker_entries[worker_index](worker_arguments[worker_index]);
    abort();
  }
  assert(wall_reads == 2);
  release_buffers();
}

static void startup_case(int cpu_count, int sparse) {
  available_cpus = cpu_count; sparse_affinity = sparse;
  created_workers = wall_reads = g_nports = 0;
  if (!setjmp(startup_done)) {
    experiment_server_main();
    abort();
  }
  int expected_workers = cpu_count < 1 ? 1 : cpu_count > 256 ? 256 : cpu_count;
  if (cpu_count == 4 && FOUR_CORE_WORKERS == 2) expected_workers = 2;
  int expected_pg = 64 / expected_workers > 2 ? 64 / expected_workers : 2;
  assert(nthreads == expected_workers && npg_per == expected_pg);
  assert(created_workers == expected_workers - 1 && wall_reads == 2);
  assert(nthreads >= 1 && nthreads <= 256 && npg_per >= 2);
  assert(nthreads * npg_per <= 512);
  assert(g_nports == (sparse ? 2 : 1) && g_ports[0] == 3000);
  if (sparse) assert(g_ports[1] == 3001);
  if (cpu_count == 2 || cpu_count == 1 || cpu_count == 4 || cpu_count == 8 || cpu_count == 16)
    assert(nthreads * npg_per == 64);
  if (cpu_count == 4) {
    assert(nthreads == (FOUR_CORE_WORKERS == 2 ? 2 : 4));
    assert(npg_per == (FOUR_CORE_WORKERS == 2 ? 32 : 16));
    assert(nthreads * (npg_per / 4) == 16);
    assert(nthreads * (npg_per - npg_per / 4) == 48);
  }
  if (cpu_count == 2) assert(nthreads == 2 && npg_per == 32);
  if (cpu_count == 3) assert(nthreads == 3 && npg_per == 21 && nthreads * npg_per == 63);
  if (cpu_count >= 256) assert(nthreads == 256 && npg_per == 2 && nthreads * npg_per == 512);
  release_buffers();
  for (int worker_index = 1; worker_index < nthreads; worker_index++)
    worker_entry_case(worker_index);
  for (int worker_index = 0; worker_index < nthreads; worker_index++) {
    th_t *worker = &g_ts[worker_index];
    assert(worker->ep >= 0 && worker->npg == npg_per && worker->nl == 0);
    for (int socket_index = 0; socket_index < npg_per; socket_index++) {
      pg_t *pg = &worker->pg[socket_index];
      assert(pg->sock == -1 && !pg->n && !pg->nheld && !pg->dirty && !pg->q && !pg->ib && !pg->ob);
    }
    lane_case(worker);
    assert(!close(worker->ep));
    assert(fcntl(worker->ep, F_GETFD) == -1 && errno == EBADF);
    free(worker->pg);
  }
  free(g_ts); g_ts = NULL;
  free(conns); conns = NULL;
}

int main(void) {
  _Static_assert(CPU_SETSIZE == 1024, "round11 expects the Linux 1024-bit cpu_set_t");
  for (int cpu_count = 0; cpu_count <= CPU_SETSIZE; cpu_count++)
    for (int sparse = 0; sparse < 2; sparse++) startup_case(cpu_count, sparse);
  printf("ok worker startup/lane/allocation lifecycle: FOUR_CORE_WORKERS=%d; 2050 actual-main cases, contiguous/sparse affinity, counts 0..1024\n",
         FOUR_CORE_WORKERS);
  return 0;
}
