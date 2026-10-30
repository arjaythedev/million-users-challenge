#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha256_t;
void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

// HMAC-SHA256 with a precomputed key schedule (inner/outer states), so verifying a token costs two
// block compressions of state copies, no key processing.
typedef struct { sha256_t inner, outer; } hmac_key_t;
void hmac_key_init(hmac_key_t *k, const void *key, size_t len);
void hmac_sha256(const hmac_key_t *k, const void *data, size_t len, uint8_t out[32]);
void hmac_sha256_raw(const void *key, size_t klen, const void *data, size_t len, uint8_t out[32]);
void pbkdf2_hmac_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen, int iters, uint8_t out[32]);
