#ifndef JSON_H
#define JSON_H
#include <stddef.h>

/* Exact error bodies (spec). */
extern const char *JERR_NOT_FOUND;        /* 404 {"error":"not found"} */
extern const char *JERR_INVALID_POST_ID;  /* 400 {"error":"invalid post id"} */
extern const char *JERR_POST_NOT_FOUND;   /* 404 {"error":"post not found"} */
extern const char *JERR_MISSING_BEARER;   /* 401 {"error":"missing bearer token"} */
extern const char *JERR_INVALID_TOKEN;    /* 401 {"error":"invalid or expired token"} */
extern const char *JERR_INVALID_PAYLOAD;  /* 401 {"error":"invalid token payload"} */
extern const char *JERR_MALFORMED_JSON;   /* 400 {"error":"malformed JSON body"} */
extern const char *JERR_BODY_REQUIRED;    /* 400 {"error":"body is required"} */
extern const char *JERR_BODY_TOO_LONG;    /* 400 {"error":"body must be at most 500 characters"} */
extern const char *JERR_INTERNAL;         /* 500 {"error":"internal server error"} */

/* Escape src (NUL-terminated) into dst (cap bytes, incl NUL). Returns escaped length (excl NUL). Truncates safely. */
size_t json_escape(const char *src, char *dst, size_t cap);
/* Length-bounded variant (src may contain UTF-8, must not contain NUL). */
size_t json_escape_n(const char *src, size_t n, char *dst, size_t cap);

/* Render {"status":"ok","db":"ok","uptime_s":N} into out. Returns length. */
size_t json_render_health(char *out, size_t cap, long uptime_s);

/* Render {"status":"degraded","db":"unreachable","error":"<esc>"} . Returns length. */
size_t json_render_degraded(char *out, size_t cap, const char *errmsg);

/* Decode a JSON string inner payload (text between the quotes, escapes
 * included) into out (cap incl NUL). Returns decoded length or (size_t)-1
 * on bad escape / truncation. Handles " \ \/ \b \f \n \r \t \uXXXX
 * incl surrogate pairs (UTF-8 output). */
size_t json_decode_string(const char *s, size_t n, char *out, size_t cap);

/* Flat mini-scan for "key":<string> (ws-tolerant). 0 found (decoded into
 * out), -1 missing / not-a-string / unterminated. */
int json_scan_string(const char *js, size_t len, const char *key, char *out, size_t cap);

/* Flat mini-scan for "key":<number>. 0 found, -1 missing / not-a-number. */
int json_scan_number(const char *js, size_t len, const char *key, long long *out);

/* 1 if the JSON object contains "alg"\s*:\s*"HS256", else 0. */
int json_header_is_hs256(const char *js, size_t len);

/* Trim " \t\n\r\v\f" both ends, in place; returns new length. */
size_t json_trim_inplace(char *s, size_t n);

/* Count UTF-8 code points (lead bytes) in s[0..n). */
size_t json_charlen(const char *s, size_t n);

/* Format PG timestamptz text (YYYY-MM-DD HH:MI:SS.ffffff+TZ) to
 * YYYY-MM-DDTHH:MI:SS.mmmZ (truncate us -> ms, no rounding). 0 ok, -1 fail. */
int json_fmt_ts(const char *s, size_t n, char *out, size_t cap);

#endif
