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
  static const char DD[] =
      "0001020304050607080910111213141516171819"
      "2021222324252627282930313233343536373839"
      "4041424344454647484950515253545556575859"
      "6061626364656667686970717273747576777879"
      "8081828384858687888990919293949596979899";
  while (v >= 100) {  // two digits per divide: u64 div is ~20-40cy
    unsigned q = (unsigned)(v % 100);
    *--e = DD[2 * q + 1];
    *--e = DD[2 * q];
    v /= 100;
  }
  if (v >= 10) {
    *--e = DD[2 * v + 1];
    *--e = DD[2 * v];
  } else {
    *--e = (char)('0' + v);
  }
  return e;
}
