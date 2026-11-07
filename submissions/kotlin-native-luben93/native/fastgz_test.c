/*
 * Round-trip tests and a speed comparison for fastgz.c. Needs only zlib. The test includes the encoder source (so it can
 * fill the hash table with arbitrary hints), which means fastgz.c is not compiled separately:
 *
 *   cc -O2 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined fastgz_test.c -lz -o /tmp/fastgz_test
 *   ASAN_OPTIONS=detect_leaks=0 /tmp/fastgz_test            # correctness (add "heavy" for a much longer run)
 *   cc -O3 -march=native fastgz_test.c -lz -o /tmp/fastgz_bench && /tmp/fastgz_bench   # speed vs zlib level 1
 *
 * Every output is decoded with zlib's inflate (gzip mode, which also checks the CRC-32 and the size) and compared with the
 * input. ONE compressor serves all the tests, so stale hash-table entries from earlier calls are exercised, and a block of
 * tests overwrites the table with zeros, 0xFFFF and random values first: the table is only ever a hint, so no content of
 * it may change what the output decodes to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include "fastgz.c"

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static long tests, fails;

/* overwrite the hint table: all zeros, all 0xFFFF, or random positions */
static void poison(fastgz_t *c, uint64_t kind) {
    for (size_t i = 0; i < HSIZE; i++) c->tab[i] = kind == 0 ? 0 : kind == 1 ? 0xFFFF : (uint16_t)rnd();
}

static int roundtrip(fastgz_t *c, const uint8_t *in, size_t n, const char *what) {
    tests++;
    size_t cap = fastgz_bound(n);
    uint8_t *out = malloc(cap), *dec = malloc(n + 16);
    size_t m = fastgz_compress(c, in, n, out, cap);
    int bad = 0;
    if (m == 0) { printf("FAIL %s n=%zu: returned 0\n", what, n); bad = 1; }
    else {
        z_stream s; memset(&s, 0, sizeof s); inflateInit2(&s, 31);
        s.next_in = out; s.avail_in = (uInt)m; s.next_out = dec; s.avail_out = (uInt)(n + 16);
        int r = inflate(&s, Z_FINISH);
        if (r != Z_STREAM_END || s.total_out != n || memcmp(dec, in, n) != 0) { printf("FAIL %s n=%zu: inflate rc=%d\n", what, n, r); bad = 1; }
        inflateEnd(&s);
        if (n > 0 && fastgz_compress(c, in, n, out, cap - 1) != 0) { printf("FAIL %s n=%zu: accepted a too-small buffer\n", what, n); bad = 1; }
    }
    free(out); free(dec);
    fails += bad;
    return bad;
}

/* a feed-shaped document: 20 posts with ids, timestamps, author names and short bodies */
static size_t make_feed(uint8_t *b, size_t cap) {
    size_t n = (size_t)snprintf((char *)b, cap, "{\"posts\":[");
    for (int i = 0; i < 20; i++)
        n += (size_t)snprintf((char *)b + n, cap - n, "%s{\"id\":%llu,\"body\":\"user_%llu says hi at 2026-10-30T13:%02d:%02d.%03dZ (VU %llu, iter %d)\",\"created_at\":\"2026-10-30T13:%02d:%02d.%03dZ\",\"author\":\"salty_river_%llu\",\"like_count\":%d}",
                              i ? "," : "", (unsigned long long)(1032872 - i), (unsigned long long)(rnd() % 50000), (int)(rnd() % 60), (int)(rnd() % 60), (int)(rnd() % 1000), (unsigned long long)(rnd() % 50000), (int)(rnd() % 40),
                              (int)(rnd() % 60), (int)(rnd() % 60), (int)(rnd() % 1000), (unsigned long long)(rnd() % 50000), (int)(rnd() % 9));
    return n + (size_t)snprintf((char *)b + n, cap - n, "]}");
}

int main(int argc, char **argv) {
    int heavy = argc > 1 && strcmp(argv[1], "heavy") == 0;
    static uint8_t feed[1 << 16], big[65535], b[65535], out[1 << 17];
    size_t fn = make_feed(feed, sizeof feed);
    fastgz_t *c = fastgz_new();
    for (size_t n = 0; n <= fn; n++) roundtrip(c, feed, n, "feed prefix");
    size_t sizes[] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 100, 255, 256, 257, 258, 259, 1000, 1024, 1025, 4096, 5000, 32767, 32768, 32769, 65534, 65535};
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        size_t n = sizes[k]; uint8_t *x = malloc(n + 1);
        for (size_t i = 0; i < n; i++) x[i] = (uint8_t)rnd(); roundtrip(c, x, n, "random");
        memset(x, 0, n); roundtrip(c, x, n, "zeros");
        for (size_t i = 0; i < n; i++) x[i] = (uint8_t)"abc"[i % 3]; roundtrip(c, x, n, "abcabc");
        for (size_t i = 0; i < n; i++) x[i] = (uint8_t)("0123456789"[rnd() % 10]); roundtrip(c, x, n, "digits");
        for (size_t i = 0; i < n; i++) x[i] = feed[i % fn]; roundtrip(c, x, n, "feed repeated");
        free(x);
    }
    for (int k = 0; k < 4000; k++) { /* other ids, timestamps and names */
        memcpy(b, feed, fn);
        int muts = 1 + (int)(rnd() % (fn / 8));
        for (int m = 0; m < muts; m++) { size_t p = rnd() % fn; b[p] = (uint8_t)(rnd() % 3 == 0 ? rnd() : ('0' + rnd() % 10)); }
        roundtrip(c, b, fn - (rnd() % 64), "mutated feed");
    }
    { /* matches at distances up to and past the 32768 window */
        size_t periods[] = {200, 5000, 20000, 24577, 32767, 32768, 32769, 40000};
        static uint8_t blk[65535];
        for (size_t i = 0; i < sizeof blk; i++) blk[i] = (uint8_t)rnd();
        for (size_t k = 0; k < sizeof periods / sizeof *periods; k++) {
            for (size_t i = 0; i < sizeof b; i++) b[i] = blk[i % periods[k]];
            roundtrip(c, b, sizeof b, "period");
            for (size_t i = 0; i < sizeof b; i += 97) b[i] ^= 1;
            roundtrip(c, b, sizeof b, "near period");
        }
        for (int k = 0; k < 400; k++) { /* random overlapping copies, lengths up to 300 */
            size_t n = 1000 + rnd() % 60000; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(rnd() % 4);
            for (int m = 0; m < 30; m++) { size_t len = 1 + rnd() % 300, d = 1 + rnd() % 33000, to = rnd() % n; if (to >= d) for (size_t i = 0; i < len && to + i < n; i++) b[to + i] = b[to + i - d]; }
            roundtrip(c, b, n, "random copies");
        }
    }
    for (int k = 0; k < 3000; k++) { /* a table full of arbitrary hints must never matter */
        size_t n = 1 + rnd() % 8000; int kind = (int)(rnd() % 3);
        for (size_t i = 0; i < n; i++) b[i] = kind == 0 ? feed[i % fn] : kind == 1 ? (uint8_t)"ab"[rnd() % 2] : (uint8_t)rnd();
        poison(c, rnd() % 3);
        roundtrip(c, b, n, "poisoned table");
    }
    if (heavy) { /* a long run of full-size inputs that keep changing, on one compressor */
        for (size_t i = 0; i < sizeof big; i++) big[i] = feed[i % fn] ^ (uint8_t)(i / 997);
        for (long k = 0; k < 66500; k++) {
            big[rnd() % sizeof big] ^= (uint8_t)(1 + rnd() % 255);
            if (k % 7 == 0) poison(c, rnd() % 3);
            if (roundtrip(c, big, sizeof big, "long run")) break;
        }
    }
    printf("tests=%ld failures=%ld\n", tests, fails);
    if (fails) return 1;
    int N = 200000; size_t o = 0; double t0 = now();
    for (int i = 0; i < N; i++) o = fastgz_compress(c, feed, fn, out, sizeof out);
    double t1 = now(); printf("fastgz : %6.2f us per %zu-byte feed, out %zu B (%.0f%%)\n", (t1 - t0) / N / 1e3, fn, o, 100.0 * o / fn);
    z_stream s; memset(&s, 0, sizeof s); deflateInit2(&s, 1, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    t0 = now(); for (int i = 0; i < N; i++) { deflateReset(&s); s.next_in = feed; s.avail_in = (uInt)fn; s.next_out = out; s.avail_out = sizeof out; deflate(&s, Z_FINISH); o = sizeof out - s.avail_out; }
    t1 = now(); printf("zlib 1 : %6.2f us per %zu-byte feed, out %zu B (%.0f%%)\n", (t1 - t0) / N / 1e3, fn, o, 100.0 * o / fn);
    return 0;
}
