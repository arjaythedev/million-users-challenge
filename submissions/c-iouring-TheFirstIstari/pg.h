#ifndef PG_H
#define PG_H
#include <stddef.h>
/* Forward-declared libpq result (libpq-fe.h is included by pg.c/http.c). */
typedef struct pg_result PGresult;

/* Per-thread pool of 32 PG connections (16 workers -> 512 max).
 * Single-flight per conn; blocking exec on the owning worker thread.
 * Pass 6 adds a plain-async path (PQsendQuery/PQconsumeInput, no pipeline
 * mode) so one slow query no longer head-of-line blocks the worker. */
void pg_init_thread(const char *conninfo);
int pg_health_check(char *errbuf, unsigned long errcap);

/* Feed / single post / create render compact JSON into thread-local buffers.
 * 0 ok, -1 db error (errbuf set). pg_get_post returns -2 when missing. */
int pg_feed(const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_get_post(const char *idstr, const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_create_post(const char *uid, const char *body, size_t body_n, const char *author,
                   const char **out, size_t *len, char *errbuf, size_t errcap);

/* Like insert. 0 ok (*already 0 first / 1 repeat), -2 post not found (23503), -1 db error. */
int pg_like(const char *uid, const char *pid, int *already, char *errbuf, size_t errcap);

/* ---- Pass 6 async primitives (plain async, NO pipeline mode) ---- */
#define PG_ASYNC_OK 0
#define PG_ASYNC_BUSY 1
#define PG_ASYNC_ERR -1
#define PG_POLL_PENDING 0
#define PG_POLL_READY 1
#define PG_POLL_ERROR -1

/* Pass 17: R/W-split pools. Per-thread 32 conns = 24R (slots 0..23) + 8W
 * (slots 24..31); 16 workers -> 384R+128W = 512 max. Reads (health/feed/post)
 * use R slots, writes (mkpost/like) use W slots with their own
 * Sync/autocommit (each write committed before its response; no shared-txn
 * group commit). Claims prefer an idle slot and queue (BUSY) on a busy one
 * only when the whole pool is in flight, so one slot maps to one in-flight
 * query at a time. */
#define PG_POOL_R 24
#define PG_POOL_W 8
#define PG_POOL_N 32
#define PG_R_END 24
int pg_claim_slot_R(void);
int pg_claim_slot_W(void);
/* Route-aware claim: MKPOST/LIKE -> W, else R. Route values are PG_ROUTE_*
 * from net.h (4=MKPOST, 5=LIKE); takes int to avoid a header cycle. */
int pg_claim_slot_for_route(int route);
int pg_slot_is_write(int slot);
/* Round-robin slot claim shared with the blocking path (legacy: any pool). */
int pg_claim_slot(void);
/* Send one prepared query on slot. OKBUSY/ERR as above. Single in-flight
 * per conn is preserved; BUSY means the slot is still busy.
 * Pass 19 (Greptile #13 hardening): the sender is recorded as the slot's
 * explicit owner (owner pointer + claim generation ogen). Only that owner
 * may poll/consume the result; a foreign waiter gets PENDING without
 * touching the slot, so a recycled/shared slot can never cross-wire a
 * result (the 404 class). Orphan slots (no live waiter) are reclaimed by
 * pg_slot_heal, which the net layer calls only when the waiter count is
 * zero -- never while an owner is live. */
int pg_send_async(int slot, const char *stmt, int npar, const char *const *par,
                  void *owner, unsigned ogen);
/* Poll a busy slot. PENDING = not ready (or not ours: foreign owner+gen --
 * the slot is left untouched); READY = *out holds the PGresult
 * (caller owns it, must PQclear even on query-error results); ERROR =
 * transport failure (slot already reset for reconnect-on-demand). */
int pg_poll(int slot, PGresult **out, void *owner, unsigned ogen);
/* Underlying socket for epoll arming, or -1 when unusable (caller must use
 * the blocking pg_run fallback). */
int pg_socket(int slot);
/* 1 when the slot has an async query in flight (pure flag read, no syscall). */
int pg_slot_busy(int slot);
/* Reclaim an ownerless slot (caller guarantees no live waiter on it).
 * Nonblocking; frees the slot when the orphan query completed or the conn
 * is bad, else leaves it for a later heal. */
void pg_slot_heal(int slot);
/* Number of slots with an async query in flight on this thread. */
int pg_busy_count(void);
int pg_busy_count_R(void);
int pg_busy_count_W(void);
/* 1 when libpq still holds unsent bytes for the slot (arm POLLOUT too),
 * else 0 (EPOLLIN suffices; an idle PG socket is always writable). */
int pg_need_write(int slot);

/* Render helpers for async completion (same wire semantics as the blocking
 * pg_* above). They do NOT PQclear(r); the caller owns r. */
int pg_render_health(PGresult *r, char *errbuf, size_t errcap);
int pg_render_feed(PGresult *r, const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_render_post(PGresult *r, const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_render_mkpost(PGresult *r, const char *body, size_t body_n, const char *author,
                     const char **out, size_t *len, char *errbuf, size_t errcap);
int pg_render_like(PGresult *r, int *already, char *errbuf, size_t errcap);

void pg_fini_thread(void);

#endif
