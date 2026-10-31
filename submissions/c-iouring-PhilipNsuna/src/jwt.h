#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sha256.h"

enum { AUTH_OK, AUTH_MISSING, AUTH_INVALID, AUTH_BAD_PAYLOAD };

typedef struct {
    hmac_key_t key;
    uint8_t scratch[2048];
} verifier_t;

void verifier_init(verifier_t *v, const char *secret, size_t len);
// Checks an Authorization header value. On AUTH_OK: *uid and the username (points into v->scratch,
// valid until the next call).
int jwt_verify(verifier_t *v, const uint8_t *auth, size_t n, int64_t now, int64_t *uid, const uint8_t **name, size_t *name_len);
// Unpadded base64url decode; returns length or -1.
long b64url_decode(const uint8_t *s, size_t n, uint8_t *dst, size_t cap);
