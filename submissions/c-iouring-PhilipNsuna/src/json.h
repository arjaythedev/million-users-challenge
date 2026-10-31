#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A strict JSON scanner (validation, top-level key lookup), string unescape/escape, JS trim.
enum { JK_NONE, JK_STRING, JK_NUMBER, JK_OBJECT, JK_ARRAY, JK_TRUE, JK_FALSE, JK_NULL };

bool json_valid(const uint8_t *b, size_t n);
// For a valid value: finds a top-level key in an object. For strings, *raw is the (still escaped)
// content between the quotes. Returns the kind (JK_NONE if not an object or absent).
int json_object_get(const uint8_t *b, size_t n, const char *key, const uint8_t **raw, size_t *rawlen);
// Unescapes a validated string body into dst (needs at most rawlen bytes). Returns length or -1.
long json_unescape(const uint8_t *raw, size_t rawlen, uint8_t *dst);
// Appends s as a JSON string literal (quotes included; non-ASCII passes through). Returns new length.
size_t json_append_string(uint8_t *dst, size_t pos, const uint8_t *s, size_t n);
// Trims like JavaScript's String.prototype.trim; returns the start and sets *len.
const uint8_t *trim_js(const uint8_t *s, size_t n, size_t *len);
size_t utf8_count(const uint8_t *s, size_t n);
