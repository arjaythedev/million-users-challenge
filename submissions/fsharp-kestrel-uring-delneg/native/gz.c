// gzip for the responses, one call per body: libdeflate (level 1 by default) or ISA-L's igzip (level 0), both built
// from pinned, checksummed source by build.sh. One compressor per I/O thread.
// igzip with `train`: the level-0 Huffman table is rebuilt about once a second from the symbol histogram of the body
// being compressed (#9, #12). Every literal and length keeps a code (isal_create_hufftables, not _subset), so any
// body still compresses; only the histogram is used, no response is kept (rule 7).
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "libdeflate.h"
#include "igzip_lib.h"

typedef struct {
  int impl; // 0 = libdeflate, 1 = igzip
  struct libdeflate_compressor *ld;
  struct isal_zstream zs;
  uint8_t *lvlbuf;
  uint32_t lvlbuf_size;
  int level;
  int train;
  int have_tables;
  int64_t trained_ms;
  struct isal_hufftables tables;
} gz_t;

static int64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &t);
  return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

__attribute__((visibility("default"))) gz_t *gz_new(int impl, int level, int train) {
  gz_t *g = calloc(1, sizeof(gz_t));
  if (!g) return NULL;
  g->impl = impl;
  g->level = level;
  g->train = impl == 1 && level == 0 && train;
  if (impl == 0) {
    g->ld = libdeflate_alloc_compressor(level);
    if (!g->ld) { free(g); return NULL; }
  } else if (level > 0) {
    g->lvlbuf_size = level == 1 ? ISAL_DEF_LVL1_DEFAULT : level == 2 ? ISAL_DEF_LVL2_DEFAULT : ISAL_DEF_LVL3_DEFAULT;
    g->lvlbuf = malloc(g->lvlbuf_size);
  }
  return g;
}

/// compresses in[0..n) into out[0..cap) as one gzip member; returns its length, or 0 if it didn't fit
__attribute__((visibility("default"))) size_t gz_compress(gz_t *g, const void *in, size_t n, void *out, size_t cap) {
  if (g->impl == 0) return libdeflate_gzip_compress(g->ld, in, n, out, cap);
  struct isal_zstream *s = &g->zs;
  if (g->train && n >= 1024) {
    int64_t t = now_ms();
    if (!g->have_tables || t - g->trained_ms >= 1000) {
      struct isal_huff_histogram h;
      memset(&h, 0, sizeof h);
      isal_update_histogram((uint8_t *)in, (int)n, &h);
      if (isal_create_hufftables(&g->tables, &h) == 0) g->have_tables = 1;
      g->trained_ms = t;
    }
  }
  isal_deflate_stateless_init(s);
  if (g->have_tables) s->hufftables = &g->tables;
  s->next_in = (uint8_t *)in;
  s->avail_in = (uint32_t)n;
  s->next_out = out;
  s->avail_out = (uint32_t)cap;
  s->end_of_stream = 1;
  s->flush = NO_FLUSH;
  s->gzip_flag = IGZIP_GZIP;
  s->level = g->level;
  if (g->level > 0) {
    s->level_buf = g->lvlbuf;
    s->level_buf_size = g->lvlbuf_size;
  }
  if (isal_deflate_stateless(s) != COMP_OK) return 0;
  return s->total_out;
}

/// an upper bound for gz_compress's output
__attribute__((visibility("default"))) size_t gz_bound(gz_t *g, size_t n) {
  (void)g;
  return n + n / 8 + 1024;
}
