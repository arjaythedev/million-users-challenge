/* gzip for one response, on the loop thread that sends it. Two implementations behind this
 * header, chosen at build time (build.zig): ISA-L's igzip, or libdeflate. */
#ifndef GZ_H
#define GZ_H

#include <stddef.h>
#include <stdint.h>

typedef struct gz_ctx gz_ctx;

/** One per loop thread; never shared. NULL on failure. */
gz_ctx *gz_create(void);

/**
 * Compresses @p len bytes at @p in into @p out, as a complete gzip stream. Returns the number
 * of bytes written, or 0 when @p cap was too small -- the caller then sends the body as it is.
 * @p now_ms is the loop's clock; igzip uses it to retrain its Huffman table about once a
 * second from the response it is given, so the table follows whatever the content is.
 */
size_t gz_compress(gz_ctx *ctx, const void *in, size_t len, void *out, size_t cap, uint64_t now_ms);

/** The implementation underneath, for the startup line. */
const char *gz_backend(void);

#endif
