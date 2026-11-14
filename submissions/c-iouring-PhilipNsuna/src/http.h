#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_HEADER_BYTES (16 * 1024)
#define MAX_BODY_BYTES (16 * 1024)

enum { M_OTHER, M_GET, M_POST };
enum { P_OK, P_INCOMPLETE, P_BAD, P_TOO_LARGE };

typedef struct {
    int method;
    const uint8_t *path; size_t path_len;
    const uint8_t *auth; size_t auth_len; // NULL if absent
    const uint8_t *body; size_t body_len;
    bool gzip, close;
} request_t;

// Parses one request at the start of b; on P_OK, *used is its length.
int parse_request(const uint8_t *b, size_t n, request_t *r, size_t *used);
// Positive integer id ("0", "-1", "abc", "1.5" are invalid).
bool parse_id(const uint8_t *s, size_t n, int64_t *out);
