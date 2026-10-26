#include "gz.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_IGZIP
/*
 * igzip, level 0, with a Huffman table of our own. Level 0 is one pass over the input with a
 * fixed table; the stock table is tuned for text in general and costs about 15% of ratio
 * against a table built for this API's JSON. So the table is built from a response the loop
 * has just sent and rebuilt about once a second: the content of a feed changes with what the
 * users post, and the table keeps up without anybody having to know in advance what they
 * will write. The histogram is statistics, not data; nothing of a response is kept.
 */
#include <igzip_lib.h>

#define RETRAIN_MS 1000

struct gz_ctx {
    struct isal_zstream stream;
    struct isal_hufftables table;
    struct isal_huff_histogram histogram;
    uint64_t trained_at;
    int trained;
};

gz_ctx *gz_create(void)
{
    gz_ctx *ctx = calloc(1, sizeof *ctx);

    return ctx;
}

size_t gz_compress(gz_ctx *ctx, const void *in, size_t len, void *out, size_t cap, uint64_t now_ms)
{
    struct isal_zstream *z = &ctx->stream;

    if (!ctx->trained || now_ms - ctx->trained_at >= RETRAIN_MS) {
        memset(&ctx->histogram, 0, sizeof ctx->histogram);
        isal_update_histogram((uint8_t *)in, (int)len, &ctx->histogram);
        isal_create_hufftables(&ctx->table, &ctx->histogram);
        ctx->trained = 1;
        ctx->trained_at = now_ms;
    }
    isal_deflate_stateless_init(z);
    z->level = 0;
    z->gzip_flag = IGZIP_GZIP;
    z->hufftables = &ctx->table;
    z->next_in = (uint8_t *)in;
    z->avail_in = (uint32_t)len;
    z->next_out = out;
    z->avail_out = (uint32_t)cap;
    z->end_of_stream = 1;
    z->flush = NO_FLUSH;
    if (isal_deflate_stateless(z) != COMP_OK)
        return 0;
    return cap - z->avail_out;
}

const char *gz_backend(void)
{
    return "igzip (ISA-L), level 0, table retrained from the responses";
}

#else
/* libdeflate, level 1. One compressor per loop, kept: it holds its tables, so a response
 * costs the deflate and nothing else. */
#include <libdeflate.h>

struct gz_ctx {
    struct libdeflate_compressor *compressor;
};

gz_ctx *gz_create(void)
{
    gz_ctx *ctx = calloc(1, sizeof *ctx);

    if (ctx == NULL)
        return NULL;
    if ((ctx->compressor = libdeflate_alloc_compressor(1)) == NULL) {
        free(ctx);
        return NULL;
    }
    return ctx;
}

size_t gz_compress(gz_ctx *ctx, const void *in, size_t len, void *out, size_t cap, uint64_t now_ms)
{
    (void)now_ms;
    return libdeflate_gzip_compress(ctx->compressor, in, len, out, cap);
}

const char *gz_backend(void)
{
    return "libdeflate, level 1";
}
#endif
