#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>

/* Deferred marker: the request is stashed in PG_WAIT; the worker must keep
 * the conn open and resume on PG readiness instead of closing. */
#define HTTP_DEFERRED 2

/* Parses one request in buf[0..len). On success returns header+body total
 * consumed bytes (>0) and fills outputs. Returns 0 if more data needed,
 * -1 on malformed (caller should close or 500). */
typedef struct {
    int method;              /* 0=GET 1=POST other=-1 */
    const char *target;      /* pointer into buf */
    size_t target_len;
    int ver_major, ver_minor;
    long content_length;     /* -1 if absent */
    const char *auth;        /* NULL if absent */
    size_t auth_len;
    const char *connection;  /* NULL if absent */
    size_t connection_len;
    const char *body;
    size_t body_len;
    size_t header_len;       /* bytes incl \r\n\r\n */
    int accept_gzip;         /* 1 when Accept-Encoding contains a gzip token */
} http_req_t;

int http_parse(const char *buf, size_t len, http_req_t *out);

/* Handles one request; writes response via send_all callback context.
 * Returns 1 if connection should stay open (keep-alive), 0 to close,
 * HTTP_DEFERRED (2) when stashed in PG_WAIT (caller must not close and must
 * arm the PG socket; resume happens via http_resume). */
struct conn_hot;
struct pg_pending;
int http_respond(struct conn_hot *c, const http_req_t *rq, long uptime_s);

/* Resume a PG_WAIT conn. Returns 1 keep-alive, 0 close, HTTP_DEFERRED when
 * the query is still pending (caller keeps waiting). */
int http_resume(struct conn_hot *c);

/* Free malloc'd copies inside a pending slot (idempotent). */
void http_pending_free(struct pg_pending *p);

int http_send(int fd, const char *hdr, size_t hlen, const char *body, size_t blen);

#endif
