#ifndef NET_H
#define NET_H
#include <time.h>
#include <stddef.h>
#include <stdalign.h>

/* Pass 6: PG_WAIT conn state. While a request waits for its async PG query,
 * the worker keeps the client fd open, arms the PQsocket in the same epoll,
 * and resumes on POLLIN|POLLOUT. One in-flight per PG conn is preserved. */
enum { CONN_ACTIVE = 0, CONN_PG_WAIT = 1 };
enum {
    PG_ROUTE_NONE = 0,
    PG_ROUTE_HEALTH = 1,
    PG_ROUTE_FEED = 2,
    PG_ROUTE_POST = 3,
    PG_ROUTE_MKPOST = 4,
    PG_ROUTE_LIKE = 5
};

struct conn_hot; /* waiter back-pointer below */

struct pg_pending {
    int route;                 /* one of PG_ROUTE_* */
    int slot;                  /* claimed PG slot */
    int sent;                  /* 0 = not yet sent (BUSY), 1 = awaiting result */
    int npar;
    char *p0;                  /* malloc'd param copies (or NULL) */
    char *p1;
    char *author;              /* mkpost render only */
    size_t body_len;           /* mkpost trimmed body length */
    unsigned long long idv;    /* like render */
    int ver_major, ver_minor;
    char *conn_hdr;            /* malloc'd Connection value or NULL */
    size_t conn_hdr_len;
    long uptime_s;
    int accept_gzip;           /* client Accept-Encoding: gzip (preserved across PG_WAIT) */
    /* Pass 19: slot ownership + waiter-list linkage (Greptile #12/#13).
     * pg_gen pairs with pg.c's per-slot (owner, gen): only the owner that
     * claimed the slot may poll/consume its result, so a recycled slot can
     * never cross-wire a foreign result (the 404 class). wq_* intrusively
     * links this waiter into its slot's DB waiter list for O(waiters)
     * completion instead of the O(128 slots) wheel scan. */
    unsigned pg_gen;           /* claim generation (never 0 while linked) */
    struct conn_hot *wq_owner; /* owning conn, NULL when unlinked */
    struct pg_pending *wq_next;
    struct pg_pending *wq_prev;
};

/* Pass 20: no-multishot-slot sentinel (cold->mslot when no recv is armed). */
#define MSHOT_NO_SLOT ((unsigned)-1)

/* Pass 10: hot/cold conn split. The per-request hot path touches ONLY
 * struct conn_hot (one 64B cache line). Everything rare -- creation time,
 * EAGAIN stash, PG_WAIT stash -- lives in struct conn_cold, allocated
 * adjacently in the same malloc block (hot at offset 0, cold at +64) so
 * cold access is one predictable pointer chase, never a hot-line polluter.
 * The 1s wheel tick touches cold only for the created_at check (expiry
 * visits, not per-req). */
struct conn_cold {
    time_t created_at;         /* cold: header-timeout + accounting only */
    char *wbuf;                /* cold: stashed unsent response bytes (EAGAIN) */
    size_t wlen;               /* cold: total stashed */
    size_t woff;               /* cold: already sent */
    int close_after_drain;     /* cold: Connection:close stashed EAGAIN: close only after last byte drains */
    /* Pass 20: multishot recv slot + generation. mgen pairs with the
     * user_data high word so a recycled slot never cross-wires a stale CQE;
     * mshot_eof latches a FIN CQE so the resume path closes promptly. */
    unsigned mslot;              /* recv-table slot, or MSHOT_NO_SLOT */
    unsigned mgen;               /* generation armed in this slot */
    int mshot_eof;               /* FIN CQE seen (multishot ended) */
    struct pg_pending pend;    /* cold: valid only when state == CONN_PG_WAIT */
};

struct __attribute__((aligned(64))) conn_hot {
    int fd;                    /* hot: client socket */
    unsigned int req_count;    /* hot: requests served on this conn */
    time_t last_active;        /* hot: 1s-sweep idle timeout */
    char *rbuf;                /* hot: read slab (pooled 2K..32K, lazy) */
    unsigned int rlen;         /* hot: buffered bytes */
    unsigned int rcap;         /* hot: slab class size */
    struct conn_hot *next;     /* hot: timing-wheel slot link (intrusive, Pass 11) */
    struct conn_hot *wprev;    /* hot: wheel doubly-linked prev (Pass 19: O(1) unlink, Greptile #6) */
    int keep_alive;            /* hot: response verdict cache */
    int state;                 /* hot: CONN_ACTIVE or CONN_PG_WAIT */
    struct conn_cold *cold;    /* hot: one chase to rare state (cold-only) */
};
_Static_assert(sizeof(struct conn_hot) <= 64, "conn hot must be <=64B");
_Static_assert(alignof(struct conn_hot) == 64, "conn hot must be 64B-aligned");

void *worker_main(void *arg);

/* Re-arm a conn for EPOLLOUT (unsent response pending). 0 ok, -1 on error. */
int net_arm_out(struct conn_hot *c);
/* Live PG_WAIT waiter count on this worker's PG slot (O(1)). Lets the HTTP
 * defer path fail fast instead of queueing without bound when the whole
 * pool is saturated (overload fail-safe; steady state never approaches it). */
int net_waiter_count(int slot);
/* Pass 21: total live PG_WAIT waiters on this worker (O(1)). Bounds the
 * summed intrusive lists to PG_WAIT_MAX; see http_defer. */
int net_waiter_total(void);
/* 1s-tick-cached wall clock for this worker (no time() per request). */
time_t net_now_wall(void);

extern char g_host[256];
extern int g_port;
extern int g_extra_port;
extern char g_db_url[2048];
extern struct timespec g_start_mono;
long current_uptime_s(void);

#endif
