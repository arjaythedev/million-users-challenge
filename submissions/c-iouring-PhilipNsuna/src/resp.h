#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BODY_NOT_FOUND "{\"error\":\"not found\"}"
#define BODY_POST_NOT_FOUND "{\"error\":\"post not found\"}"
#define BODY_INVALID_ID "{\"error\":\"invalid post id\"}"
#define BODY_MISSING_TOKEN "{\"error\":\"missing bearer token\"}"
#define BODY_INVALID_TOKEN "{\"error\":\"invalid or expired token\"}"
#define BODY_BAD_PAYLOAD "{\"error\":\"invalid token payload\"}"
#define BODY_MALFORMED_JSON "{\"error\":\"malformed JSON body\"}"
#define BODY_REQUIRED "{\"error\":\"body is required\"}"
#define BODY_TOO_LONG "{\"error\":\"body must be at most 500 characters\"}"
#define BODY_INTERNAL "{\"error\":\"internal server error\"}"
#define BODY_BAD_REQUEST "{\"error\":\"bad request\"}"
#define BODY_TOO_LARGE "{\"error\":\"request too large\"}"

// A growable byte buffer.
typedef struct { uint8_t *p; size_t len, cap; } buf_t;
void buf_reserve(buf_t *b, size_t need);
static inline void buf_reset(buf_t *b) { b->len = 0; }
void buf_append(buf_t *b, const void *s, size_t n);
static inline void buf_str(buf_t *b, const char *s) { buf_append(b, s, __builtin_strlen(s)); }
void buf_int(buf_t *b, int64_t v);
void buf_json_string(buf_t *b, const uint8_t *s, size_t n);

// Full HTTP response (status line, headers, body) appended to out.
void append_response(buf_t *out, int status, const uint8_t *body, size_t n, bool gzipped, bool close);
// Post object; created_micros = microseconds since 2000-01-01 UTC (binary TIMESTAMPTZ).
void append_post(buf_t *b, int64_t id, const uint8_t *body, size_t blen, int64_t created_micros, const uint8_t *author, size_t alen, int32_t likes);
void append_timestamp(buf_t *b, int64_t micros);
void append_health_ok(buf_t *b, int64_t uptime);
void append_health_degraded(buf_t *b, const char *err);
void append_like(buf_t *b, bool already, int64_t post_id);
// POST /posts body validation: returns 0 and the trimmed body (in scratch) or an HTTP status + message.
int validate_body(const uint8_t *raw, size_t n, uint8_t *scratch, size_t cap, const uint8_t **trimmed, size_t *tlen, const char **msg);
