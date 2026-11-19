// Focused tests for Code A's bounded JSON scans, timestamp digits and adaptive gzip.
#define main feed_server_main
#include "../server.c"
#undef main
#include <assert.h>
#include <sys/mman.h>

static uint32_t rng = 0x187349ac;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static size_t escaped_ref(const unsigned char *s, size_t n, char *out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned c = s[i];
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
    else if (c == '\n' || c == '\r' || c == '\t') {
      out[o++] = '\\'; out[o++] = c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
    } else if (c < 32) { snprintf(out + o, 7, "\\u%04x", c); o += 6; }
    else out[o++] = c;
  }
  return o;
}
static void check_escape(const unsigned char *s, size_t n) {
  char expected[4096]; size_t len = escaped_ref(s, n, expected);
  alen = 7; memset(arena, '#', 7); putjs(s, n);
  assert(alen == len + 7 && !memcmp(arena + 7, expected, len));
  assert(!memcmp(arena, "#######", 7));
}
static void test_json(void) {
  size_t page = sysconf(_SC_PAGESIZE);
  unsigned char *mem = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(mem != MAP_FAILED && !mprotect(mem + page, page, PROT_NONE));
  // Every byte value over varying SIMD lanes; a guard page catches reads beyond the slice.
  for (size_t n = 0; n <= 65; n++) {
    unsigned char *s = mem + page - n;
    memset(s, 'a', n);
    check_escape(s, n);
    for (unsigned c = 0; c < 256 && n; c++) {
      size_t pos = c % n; s[pos] = c; check_escape(s, n); s[pos] = 'a';
    }
  }
  unsigned char raw[512];
  for (int i = 0; i < 256; i++) raw[i] = i;
  check_escape(raw, 256);
  for (int k = 0; k < 256; k++) {
    for (size_t i = 0; i < sizeof raw; i++) raw[i] = random32();
    check_escape(raw, k % sizeof raw);
  }
  // Raw valid UTF-8, escapes, surrogate pair and lone surrogate through an ASCII run.
  const char *input = "\"abcdefghijklmnop café ✓ \\uD83D\\uDE80 \\ud800 \\n\\t\\\\\\\"\"";
  const char *expected = "abcdefghijklmnop café ✓ 🚀 � \n\t\\\"";
  size_t len = strlen(expected); char output[256];
  for (size_t cap = 0; cap <= len + 1; cap++) {
    memset(output, '#', sizeof output);
    js_t j = {input, input + strlen(input)};
    long got = jstr(&j, output, cap);
    assert(got == (cap < len ? -2 : (long)len) && j.p == j.e);
    if (cap >= len) assert(!memcmp(output, expected, len));
    assert(output[cap] == '#');
  }
  const char *bad[] = {"\"abcdefghijklmnop\xc0\xaf\"", "\"abcdefghijklmno\xed\xa0\x80\"",
    "\"abcdefghijklmno\xf4\x90\x80\x80\"", "\"abcdefghijklmno\xe2\x82", "\"abcdefghijklmno\x1f\"",
    "\"abcdefghijklmnop\\uXY00\"", "\"abcdefghijklmnop\\q\""};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    size_t n = strlen(bad[i]); char *s = (char *)mem + page - n; memcpy(s, bad[i], n);
    js_t j = {s, s + n}; assert(jstr(&j, output, sizeof output) == -1);
    j.p = s; assert(jstr(&j, NULL, 0) == -1);
  }
  for (size_t n = 2; n <= 65; n++) {
    char *s = (char *)mem + page - n;
    memset(s, 'x', n); s[0] = s[n - 1] = '"';
    js_t j = {s, s + n}; assert(jstr(&j, output, n - 2) == (long)n - 2 && j.p == j.e);
    s[n - 1] = 'x'; j.p = s; assert(jstr(&j, output, sizeof output) == -1);
  }
  assert(!munmap(mem, 2 * page));
  puts("ok JSON escaping/parser: byte coverage, SIMD/page boundaries, Unicode and capacity");
}
static void check_timestamp(int64_t us) {
  int64_t sec = us / 1000000, rem = us % 1000000;
  if (rem < 0) { rem += 1000000; sec--; }
  time_t unix_sec = sec + 946684800; struct tm tm;
  assert(gmtime_r(&unix_sec, &tm));
  char expected[64]; snprintf(expected, sizeof expected, "%04d", tm.tm_year + 1900);
  size_t n = 4 + strftime(expected + 4, sizeof expected - 4, "-%m-%dT%H:%M:%S", &tm);
  snprintf(expected + n, sizeof expected - n, ".%03uZ", (unsigned)(rem / 1000));
  alen = 3; put_ts(us);
  if (alen != 27 || strlen(expected) != 24 || memcmp(arena + 3, expected, 24)) {
    fprintf(stderr, "timestamp %lld: want %s, got %.*s\n", (long long)us, expected, 24, arena + 3); abort();
  }
}
static void test_timestamps(void) {
  int64_t edges[] = {0, -1, -999, -1000, -999999, -1000000, -1000001, 999, 1000, 999999,
    -946684800000001LL, -946684800000000LL, 5097600000000LL,
    31622400000000LL, 31622399999999LL, 31622400000001LL, 3160857600000000LL,
    -63082281600000000LL, 252455615999999999LL};
  for (size_t i = 0; i < sizeof edges / sizeof *edges; i++) check_timestamp(edges[i]);
  for (int i = 0; i < 2000; i++) check_timestamp(((int64_t)random32() - 2147483648LL) * 1000000LL + random32() % 1000000);
  puts("ok timestamps: UTC oracle, negative epoch, leap/day boundaries, millisecond truncation");
}
static void save(const char *path, const void *data, size_t n) {
  FILE *f = fopen(path, "wb"); assert(f && fwrite(data, 1, n, f) == n && !fclose(f));
}
static void roundtrip(const char *in, size_t n, char *packed, int save_fixture) {
  char decoded[16384]; struct inflate_state state;
  size_t z = gzip(in, n, packed, 16384); assert(z);
  isal_inflate_init(&state); state.crc_flag = ISAL_GZIP;
  state.next_in = (uint8_t *)packed; state.avail_in = z;
  state.next_out = (uint8_t *)decoded; state.avail_out = sizeof decoded;
  assert(isal_inflate_stateless(&state) == ISAL_DECOMP_OK);
  assert(state.total_out == n && !memcmp(in, decoded, n));
  if (save_fixture) { save("bin/hotpaths.raw", in, n); save("bin/hotpaths.gz", packed, z); }
}
static void test_gzip(void) {
  char input[8192], packed[16384];
  memset(&gz, 0, sizeof gz); now_ns = 1000000000;
  memset(input, 'a', sizeof input); roundtrip(input, sizeof input, packed, 0);
  assert(gz.have_tables && gz.train_after == 2000000000);
  // The trained table must handle every byte, including symbols absent during training.
  for (size_t i = 0; i < sizeof input; i++) input[i] = i;
  roundtrip(input, sizeof input, packed, 1); assert(gz.train_after == 2000000000);
  now_ns = 2000000000; roundtrip(input, sizeof input, packed, 0); assert(gz.train_after == 3000000000);
  for (size_t i = 0; i < sizeof input; i++) input[i] = random32();
  roundtrip(input, sizeof input, packed, 0);
  assert(!gzip(input, sizeof input, packed, 8));
  want_gz = 1; begin_resp(200, 0); put(input, sizeof input); size_t start = body_start; finish();
  assert(!zlen && alen == start + sizeof input && !memcmp(arena + start, input, sizeof input));
  assert(!memmem(arena, start, "Content-Encoding", 16));
  assert(strtoull(arena + cl_pos, NULL, 10) == sizeof input);
  memset(input, 'b', sizeof input); begin_resp(200, 0); put(input, sizeof input); finish();
  assert(zlen && memmem(arena, alen, "Content-Encoding: gzip", 22));
  assert(strtoull((char *)memmem(arena, alen, "Content-Length: ", 16) + 16, NULL, 10) == zlen);
  puts("ok gzip: unseen symbols, retraining, CRC round trips, insufficient output, HTTP fallback/framing");
}
int main(void) {
  arena = malloc(OBUF); zbuf = malloc(OBUF); assert(arena && zbuf);
  test_json(); test_timestamps(); test_gzip(); free(arena); free(zbuf);
  return 0;
}
