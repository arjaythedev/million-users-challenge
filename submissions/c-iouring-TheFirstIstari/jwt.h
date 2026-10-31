#ifndef JWT_H
#define JWT_H
#include <stddef.h>

#define JWT_OK 0       /* signature + claims valid */
#define JWT_BAD 1      /* malformed / bad alg / bad signature / expired -> 401 invalid or expired token */
#define JWT_PAYLOAD 2  /* good signature, bad sub/username -> 401 invalid token payload */

/* Verify an HS256 JWT (tok is not NUL-terminated, tlen bytes, no Bearer  prefix).
 * On JWT_OK, sub_out holds the sub string and *uid_out its value, user_out the
 * username (unescaped, NUL-terminated). Verifies every call, no cache. */
int jwt_verify(const char *tok, size_t tlen, const char *secret,
               char *sub_out, size_t sub_cap, long long *uid_out,
               char *user_out, size_t user_cap);

/* JWT secret from server config (server.c). */
const char *jwt_secret(void);

#endif
