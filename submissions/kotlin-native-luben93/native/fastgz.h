#ifndef FASTGZ_H
#define FASTGZ_H
#include <stddef.h>
#include <stdint.h>
typedef struct fastgz fastgz_t;
/* One compressor per thread. */
fastgz_t *fastgz_new(void);
void fastgz_free(fastgz_t *c);
/* Worst-case output size for n input bytes (what the caller must provide). */
size_t fastgz_bound(size_t n);
/* gzip stream of in[0..n) into out (capacity cap >= fastgz_bound(n)); returns its size, or 0 if n > 65535 or cap is too small. */
size_t fastgz_compress(fastgz_t *c, const uint8_t *in, size_t n, uint8_t *out, size_t cap);
#endif
