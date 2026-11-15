// The thin C under server.goose: ISA-L gzip, and the libpq calls that
// need arrays or C strings. The sockets are ring.c. Everything else (HTTP, JSON, JWT, routing, responses, connection table, event loop,
// batching policy) is Goose. No Goose header is needed: only plain integers and pointers cross.
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include "igzip_lib.h"
#include "libpq-fe.h"

typedef struct { const uint8_t *data; int64_t len; } slice_t;  // how Goose passes a u8[:]

int32_t shim_cpus(void) { cpu_set_t cs; sched_getaffinity(0, sizeof cs, &cs); return CPU_COUNT(&cs); }

// Ignores SIGPIPE and raises RLIMIT_NOFILE to its hard limit.
void shim_init(void) {
  signal(SIGPIPE, SIG_IGN);
  struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
  if (rl.rlim_cur < rl.rlim_max) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
}


// ISA-L igzip at level 0 (its fastest), stateless. Returns the gzip size, or 0 if it did not fit.
int64_t shim_gzip(uint8_t *out, int64_t cap, slice_t in) {
  struct isal_zstream s; isal_deflate_stateless_init(&s);
  s.next_in = (uint8_t *)in.data; s.avail_in = in.len; s.next_out = out; s.avail_out = cap;
  s.gzip_flag = IGZIP_GZIP; s.level = 0;
  return isal_deflate_stateless(&s) == COMP_OK ? cap - s.avail_out : 0;
}


// ---- Postgres ----

// url is NUL-terminated. Connects, prepares the n statements of the configured API (text holds them, NUL-terminated, at
// offs[i]; np[i] says how many parameters statement i has; the parameter types are left to the server to infer), sets the
// session to UTC (the API's timestamps are), enters pipeline mode and goes non-blocking. Returns the PGconn (0 on
// failure; the reason goes to stderr), or -1 if Postgres refuses one of the statements.
uint64_t shim_pg_connect_n(const char *url, int32_t n, const char *text, const int32_t *offs, const int32_t *np) {
  static const char *const kw[] = {"dbname", "connect_timeout", NULL};
  const char *const val[] = {url, "3", NULL};  // a down database stalls this loop for at most 3 s a try
  PGconn *c = PQconnectdbParams(kw, val, 1);
  if (PQstatus(c) != CONNECTION_OK) { fprintf(stderr, "connect: %s", PQerrorMessage(c)); PQfinish(c); return 0; }
  PGresult *r = PQexec(c, "SET TimeZone TO 'UTC'");
  int ok = PQresultStatus(r) == PGRES_COMMAND_OK; PQclear(r);
  for (int k = 0; ok && k < n; k++) {
    char name[16]; snprintf(name, sizeof name, "s%d", k);
    r = PQprepare(c, name, text + offs[k], np[k], NULL);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {              // Postgres refuses the statement: no connection will ever take it
      fprintf(stderr, "openapi: Postgres refuses statement %d (%s): %s", k, text + offs[k], PQerrorMessage(c));
      PQclear(r); PQfinish(c);
      return (uint64_t)-1;
    }
    PQclear(r);
  }
  if (!ok || !PQenterPipelineMode(c) || PQsetnonblocking(c, 1)) { PQfinish(c); return 0; }
  return (uint64_t)c;
}
// Sends statement stmt with n text parameters: value i is the NUL-terminated string at buf + offs[i]. libpq copies them
// before it returns. Returns 0 if the statement could not be queued.
int32_t shim_pg_send_n(uint64_t conn, int32_t stmt, int32_t n, const char *buf, const int32_t *offs) {
  const char *v[32];
  if (n > 32) return 0;
  for (int i = 0; i < n; i++) v[i] = buf + offs[i];
  char name[16]; snprintf(name, sizeof name, "s%d", stmt);
  return PQsendQueryPrepared((PGconn *)conn, name, n, v, NULL, NULL, 0);
}
int64_t shim_affected(uint64_t res) { return strtoll(PQcmdTuples((PGresult *)res), NULL, 10); }
int64_t shim_cstr_len(uint64_t p) { return p ? (int64_t)strlen((const char *)p) : 0; }
void shim_pg_lost(uint64_t conn) { fprintf(stderr, "postgres connection lost: %s", PQerrorMessage((PGconn *)conn)); }
