#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <isa-l/igzip_lib.h>

#define HUFF_TRAIN_TARGET_BYTES 4096U

typedef struct {
    struct isal_zstream stream;
    struct isal_hufftables tables;
    struct isal_huff_histogram train_hist;
    uint8_t *level_buf;
    uint32_t level_buf_size;
    uint32_t trained_bytes;
    int level;
    int train_enabled;
    int have_tables;
} igzip_ctx_t;

igzip_ctx_t *igzip_ctx_new(int level, int train_huff) {
    igzip_ctx_t *ctx = (igzip_ctx_t *)calloc(1, sizeof(igzip_ctx_t));
    if (!ctx) {
        return NULL;
    }
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    ctx->level = level;
    if (level == 0 && train_huff != 0) {
        ctx->train_enabled = 1;
    } else if (level > 0) {
        ctx->level_buf_size = (level == 1) ? ISAL_DEF_LVL1_MIN
                            : (level == 2) ? ISAL_DEF_LVL2_MIN
                                           : ISAL_DEF_LVL3_MIN;
        ctx->level_buf = (uint8_t *)malloc(ctx->level_buf_size);
        if (!ctx->level_buf) {
            free(ctx);
            return NULL;
        }
    }
    return ctx;
}

void igzip_ctx_free(igzip_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx->level_buf);
    free(ctx);
}

size_t igzip_compress_gzip(
    igzip_ctx_t *ctx,
    const uint8_t *src,
    size_t src_len,
    uint8_t *dst,
    size_t dst_cap,
    uint64_t now_ms
) {
    (void)now_ms;
    if (!ctx || !src || !dst || dst_cap < 32) {
        return 0;
    }
    if (__builtin_expect(ctx->train_enabled != 0, 0)) {
        uint32_t take = (src_len > 65536U) ? 65536U : (uint32_t)src_len;
        isal_update_histogram((uint8_t *)src, take, &ctx->train_hist);
        ctx->trained_bytes += take;
        if (ctx->trained_bytes >= HUFF_TRAIN_TARGET_BYTES) {
            if (isal_create_hufftables(&ctx->tables, &ctx->train_hist) == 0) {
                ctx->have_tables = 1;
            }
            ctx->train_enabled = 0;
        }
    }

    struct isal_zstream *s = &ctx->stream;
    isal_deflate_stateless_init(s);
    if (ctx->level == 0 && ctx->have_tables) {
        s->hufftables = &ctx->tables;
    }
    s->next_in = (uint8_t *)src;
    s->avail_in = (uint32_t)src_len;
    s->next_out = dst;
    s->avail_out = (uint32_t)dst_cap;
    s->end_of_stream = 1;
    s->flush = NO_FLUSH;
    s->gzip_flag = IGZIP_GZIP;
    s->level = (uint32_t)ctx->level;
    if (ctx->level > 0) {
        s->level_buf = ctx->level_buf;
        s->level_buf_size = ctx->level_buf_size;
    }
    if (isal_deflate_stateless(s) != COMP_OK) {
        return 0;
    }
    return (size_t)s->total_out;
}
