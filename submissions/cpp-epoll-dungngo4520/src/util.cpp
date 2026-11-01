// Small utilities: monotonic clock and fatal error reporting.

#include "server.h"

uint64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void die(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

// writes the decimal v ending at e (no NUL); returns the start
char* u64toa(uint64_t v, char* e) {
  do {
    *--e = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  return e;
}
