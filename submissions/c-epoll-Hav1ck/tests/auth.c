#define _GNU_SOURCE
// Independent Python-generated signatures; deterministic fractional wall clock.
#include <time.h>
#include <assert.h>
static struct timespec test_wall = {2000000000, 250000000};
static unsigned wall_reads;
static int auth_clock(clockid_t clock, struct timespec *ts) {
  if (clock == CLOCK_REALTIME) { *ts = test_wall; wall_reads++; return 0; }
  return clock_gettime(clock, ts);
}
#define clock_gettime auth_clock
#define main round2_server_main
#include "../server.c"
#undef main
#undef clock_gettime

int main(int argc, char **argv) {
  assert(argc == 2);
  memset(b64t, -1, sizeof b64t);
  for (int i = 0; i < 64; i++) b64t[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]] = i;
  hmac_key("round2-test-secret"); now_s = 0; // intentionally stale event-loop sample
  FILE *f = fopen(argv[1], "r"); assert(f);
  char *line = NULL; size_t cap = 0; unsigned cases = 0;
  while (getline(&line, &cap, f) > 0) {
    char *tab = strchr(line, '\t'); assert(tab); *tab++ = 0;
    size_t n = strlen(tab); if (n && tab[n-1] == '\n') tab[--n] = 0;
    int64_t uid = 0; char username[MAXREQ]; size_t unl = 0;
    const char *result = auth(tab, n, &uid, username, &unl);
    if (!strcmp(line, "ok")) {
      assert(!result && uid == 7 && unl == 5 && !memcmp(username, "alice", 5));
      // Same token after expiry, without advancing the event-loop clock.
      test_wall.tv_sec += 2;
      assert(auth(tab, n, &uid, username, &unl));
      test_wall.tv_sec -= 2;
      now_s = 2100000000; // a stale future loop sample must not reject it either
      assert(!auth(tab, n, &uid, username, &unl));
      now_s = 0;
      // Same token, different key: every call must hash again.
      hmac_key("different-key");
      assert(auth(tab, n, &uid, username, &unl));
      hmac_key("round2-test-secret");
      assert(!auth(tab, n, &uid, username, &unl));
    } else { assert(result && !strcmp(result, line)); }
    cases++;
  }
  assert(cases >= 20 && wall_reads > 0);
  free(line); fclose(f);
  puts("ok independent JWT signatures, header fallback, claims, fresh key/time on repeated tokens");
}
