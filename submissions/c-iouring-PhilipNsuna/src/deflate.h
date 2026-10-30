#pragma once
#include <stddef.h>
#include <stdint.h>
#include "resp.h"

// A gzip encoder for one response at a time: LZ77 with a 4-byte hash and one dynamic-Huffman block
// whose code is fixed (trained once on a sample feed body) instead of rebuilt per block.
#define DF_HASH_BITS 13
#define DF_HASH_SIZE (1 << DF_HASH_BITS)

typedef struct { uint32_t bits; uint8_t n; } dfcode_t;
typedef struct {
    dfcode_t lit[286], dist[30];
    uint8_t header[512]; int hbits;
    int32_t head[DF_HASH_SIZE];
    buf_t out;
} deflater_t;

void deflater_init(deflater_t *d, const uint8_t *sample, size_t n);
// Returns the gzip member in d->out (valid until the next call).
const uint8_t *deflate_gzip(deflater_t *d, const uint8_t *src, size_t n, size_t *outlen);
uint32_t crc32_ieee(const uint8_t *p, size_t n);
