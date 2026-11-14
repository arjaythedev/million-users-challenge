#define _GNU_SOURCE
#include <stdlib.h>
#include <assert.h>
static int fail_next_allocation;
static void *retention_malloc(size_t n) {
  if (fail_next_allocation) { fail_next_allocation = 0; return NULL; }
  return malloc(n);
}
#define malloc retention_malloc
#define main auth_retention_production_main
#include "../server.c"
#undef main
#undef malloc
int main(void) {
  conn_t client = {.open = 1, .gen = 23}; conns = &client;
  pg_t lanes[2] = {{.sock = 1}, {.sock = 1}};
  th_t worker = {.npg = 2, .pg = lanes};
  char body[] = "body"; static char name[48000]; memset(name, 'a', sizeof name);
  now_ns = 1000000000;
  fail_next_allocation = 1;
  assert(!enqueue(&worker, 0, Q_INS, 0, 0, 1, body, 4, name, sizeof name));
  assert(!fail_next_allocation && !lanes[1].n && !lanes[1].olen && !lanes[1].dirty && !lanes[1].unsynced);
  assert(enqueue(&worker, 0, Q_INS, 0, 0, 1, body, 4, name, sizeof name));
  assert(lanes[1].n == 1 && lanes[1].dirty && lanes[1].unsynced);
  q_t *q = &lanes[1].q[0]; assert(q->blen == 4 && q->ulen == sizeof name && q->gen == 23);
  memset(body, 'x', 4); memset(name, 'z', sizeof name);
  assert(!memcmp(q->extra, "body", 4));
  for (size_t i = 0; i < sizeof name; i++) assert(q->extra[4+i] == 'a');
  free(q->extra); free(lanes[1].q); free(lanes[1].ob);
  puts("ok real enqueue long-name retention and failed-allocation queue integrity");
}
