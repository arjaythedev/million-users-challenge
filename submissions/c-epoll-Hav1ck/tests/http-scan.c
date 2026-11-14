// Independent scalar HTTP scan oracles and frozen-baseline parser comparisons.
#define main feed_server_main
#include "../server.c"
#undef main
#include <sys/mman.h>

static size_t span_checks, line_checks, parser_checks;
static int oracle_tchar(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
  switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
    case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~': return 1;
    default: return 0;
  }
}
static size_t oracle_span(const unsigned char *s, size_t n) {
  size_t i = 0;
  while (i < n && (s[i] == 9 || (s[i] >= 32 && s[i] != 127))) i++;
  return i;
}
static int oracle_line(char *h, char *end, char **col, char **hl) {
  char *line = h;
  while (end - line >= 2 && !(line[0] == '\r' && line[1] == '\n')) line++;
  if (end - line < 2) return 0;
  char *c = h;
  while (c < line && *c != ':') c++;
  if (c == h || c == line) return 0;
  for (char *p = h; p < c; p++) if (!oracle_tchar((unsigned char)*p)) return 0;
  for (char *p = c + 1; p < line; p++) {
    unsigned b = (unsigned char)*p;
    if (b != 9 && (b < 32 || b == 127)) return 0;
  }
  *col = c; *hl = line; return 1;
}
static void check_span(unsigned char *s, size_t n) {
  assert(http_value_span(s, n) == oracle_span(s, n)); span_checks++;
}
static void check_line(char *s, size_t n) {
  char *ac = NULL, *al = NULL, *bc = NULL, *bl = NULL;
  int a = http_header_line(s, s + n, &ac, &al), b = oracle_line(s, s + n, &bc, &bl);
  assert(a == b); if (a) assert(ac == bc && al == bl); line_checks++;
}
static uint32_t rng = 0x597b62e1;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void scanners(void) {
  for (unsigned c = 0; c < 256; c++) assert(!!tchar(c) == oracle_tchar(c));
  size_t page = sysconf(_SC_PAGESIZE);
  char *mem = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mem != MAP_FAILED && !mprotect(mem + page, page, PROT_READ | PROT_WRITE));
  for (int side = 0; side < 2; side++) {
    // Both page edges: no lookbehind or lookahead; every byte at every position.
    for (size_t n = 0; n <= 65; n++) {
      unsigned char *s = (unsigned char *)mem + page + (side ? page - n : 0);
      memset(s, 'a', n); check_span(s, n); check_line((char *)s, n);
      for (size_t pos = 0; pos < n; pos++) for (unsigned b = 0; b < 256; b++) {
        s[pos] = b; check_span(s, n); s[pos] = 'a';
      }
    }
    const size_t sizes[] = {0, 1, 7, 15, 16, 17, 31, 32, 33, 63, 64, 65};
    for (size_t ni = 0; ni < 9; ni++) for (size_t vi = 0; vi < sizeof sizes / sizeof *sizes; vi++) {
      size_t nn = sizes[ni], vn = sizes[vi], n = nn + 1 + vn + 2;
      char *s = mem + page + (side ? page - n : 0);
      memset(s, 'X', nn); s[nn] = ':'; memset(s + nn + 1, 'v', vn); memcpy(s + n - 2, "\r\n", 2);
      check_line(s, n);
      for (size_t pos = 0; pos < n; pos++) {
        char old = s[pos];
        for (unsigned b = 0; b < 256; b++) { s[pos] = b; check_line(s, n); }
        s[pos] = old;
      }
      // Truncation anywhere includes bare CR, absent LF, and missing colon.
      for (size_t cut = 0; cut < n; cut++) check_line(s, cut);
    }
    for (int trial = 0; trial < 20000; trial++) {
      size_t n = random32() % 257; char *s = mem + page + (side ? page - n : 0);
      for (size_t i = 0; i < n; i++) s[i] = random32();
      check_span((unsigned char *)s, n); check_line(s, n);
    }
  }
  assert(!munmap(mem, page * 3));
  printf("ok HTTP scalar oracles: 256 tchar bytes, %zu spans, %zu lines; both guard-page edges\n", span_checks, line_checks);
}

#include "http-baseline-serve.inc"
typedef struct {
  long used;
  int open, busy, closing, continued, gz;
  size_t bytes;
  char reply[4096];
} observation;
static th_t t;
static observation observe(char *s, size_t n, int baseline) {
  int fds[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds));
  int fd = fds[0]; assert(fd < maxfd && !conns[fd].open);
  uint32_t gen = conns[fd].gen; memset(&conns[fd], 0, sizeof conns[fd]); conns[fd].gen = gen; conns[fd].open = 1;
  want_gz = 0; alen = zlen = 0;
  observation o = {0};
  o.used = baseline ? baseline_serve(&t, fd, s, n) : serve(&t, fd, s, n);
  conn_t *c = &conns[fd];
  o.open = c->open; o.busy = c->busy; o.closing = c->closing; o.continued = c->continued; o.gz = want_gz;
  assert(!c->out && !c->busy); // corpus uses immediate unknown/auth/error routes, no DB
  for (;;) {
    ssize_t r = read(fds[1], o.reply + o.bytes, sizeof o.reply - o.bytes);
    if (!r || (r < 0 && errno == EAGAIN)) break;
    assert(r > 0); o.bytes += r; assert(o.bytes < sizeof o.reply);
  }
  conn_close(&t, fd); close_pending(&t); close(fds[1]); return o;
}
static void check_parser(char *s, size_t n) {
  observation a = observe(s, n, 0), b = observe(s, n, 1);
  assert(a.used == b.used && a.open == b.open && a.busy == b.busy && a.closing == b.closing);
  assert(a.continued == b.continued && a.gz == b.gz && a.bytes == b.bytes && !memcmp(a.reply, b.reply, a.bytes));
  parser_checks++;
}
static void parsers(void) {
  signal(SIGPIPE, SIG_IGN); maxfd = 8192; conns = calloc(maxfd, sizeof *conns);
  arena = malloc(OBUF); zbuf = malloc(OBUF); assert(conns && arena && zbuf);
  now_s = started = 2000000000; now_ns = 1000000000; t.ep = epoll_create1(EPOLL_CLOEXEC); assert(t.ep >= 0);
  const char *corpus[] = {
    "GET /unknown HTTP/1.1\r\n\r\n", "GET /unknown HTTP/1.0\r\nConnection: keep-alive, close\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nX!#$%&'*+-.^_`|~: \tvalue\t\r\nContent-Length: 2\r\ncontent-length: 02\r\n\r\n{}",
    "GET /unknown HTTP/1.1\r\nAccept-Encoding: gzip;q=0, *;q=1\r\nAccept-Encoding: GZip\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nAccept-Encoding: gzip; q=0.5;q=1\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nAccept-Encoding: gzip; unrelated=yes, br;q=0\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nAccept-Encoding: xgzip\r\n\r\nGET /unknown HTTP/1.1\r\n\r\n",
    "POST /posts HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n{",
    "GET /unknown HTTP/1.1\r\nAuthorization: x\r\nauthorization: y\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}x",
    "GET /unknown HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
    "GET /unknown HTTP/1.1\r\nContent-Length: +0\r\n\r\n", "GET /unknown HTTP/1.1\r\nContent-Length: 9999999999999999999999999999999\r\n\r\n"
  };
  for (size_t i = 0; i < sizeof corpus / sizeof *corpus; i++) {
    char *s = strdup(corpus[i]); size_t n = strlen(s);
    for (size_t cut = 0; cut <= n; cut++) check_parser(s, cut);
    free(s);
  }
  // Every byte at every position of the entire request: binary/NUL included.
  char req[] = "GET /unknown HTTP/1.1\r\nX-Header: abcdefghijklmnopqrstuvwxyz0123456789\r\n\r\n";
  for (size_t pos = 0; pos < sizeof req - 1; pos++) {
    char old = req[pos];
    for (unsigned b = 0; b < 256; b++) { req[pos] = b; check_parser(req, sizeof req - 1); }
    req[pos] = old;
  }
  for (int trial = 0; trial < 2000; trial++) {
    char s[256]; size_t n = snprintf(s, sizeof s, "GET /unknown HTTP/1.1\r\nX: ");
    size_t extra = random32() % 129;
    for (size_t i = 0; i < extra; i++) s[n++] = random32();
    memcpy(s + n, "\r\n\r\n", 4); n += 4; check_parser(s, n);
  }
  // Exact limit, oversized headers, and declared bodies exceeding total limit.
  for (int excess = -1; excess <= 1; excess++) {
    size_t n = MAXREQ + excess; char *s = malloc(n + 1);
    size_t head = strlen("GET /unknown HTTP/1.1\r\nX: "); memcpy(s, "GET /unknown HTTP/1.1\r\nX: ", head);
    memset(s + head, 'v', n - head - 4); memcpy(s + n - 4, "\r\n\r\n", 4); check_parser(s, n); free(s);
  }
  char cap[] = "GET /unknown HTTP/1.1\r\nContent-Length: 65536\r\n\r\n"; check_parser(cap, sizeof cap - 1);
  close(t.ep); free(conns); free(arena); free(zbuf);
  printf("ok frozen baseline serve differential: %zu binary/fragment/pipeline/negotiation/limit cases\n", parser_checks);
}
int main(void) { scanners(); parsers(); puts("all HTTP scan and baseline-equivalence checks passed"); }
