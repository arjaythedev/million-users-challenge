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
    /* Pass 25: zero-malloc waiter stash. ids (post/like id, jwt sub) are
     * ≤15-19 digits + NUL, so p0/p1 inline [24] always fit (no bound check
     * needed, but http_defer guards fail-closed anyway). username arrives
     * NUL-terminated in a 256 B buffer (json_scan_string cap), so
     * author[256] always fits. mkpost body (validated ≤500 code points ⇒
     * ≤2000 B) is copied once into body[] (fail-closed past 2048). The
     * PG retry / blocking fallback / render paths read these arrays, so no
     * per-request malloc/free remains on defer/resume. */
    char p0[24];               /* param 0 inline (id / sub) */
    char p1[24];               /* param 1 inline (post/like id; mkpost uses body[]) */
    char author[256];          /* mkpost render only */
    char body[2048];           /* mkpost trimmed body (+NUL), copied once */
    size_t body_len;           /* mkpost trimmed body length */
    unsigned long long idv;    /* like render */
    int ver_major, ver_minor;
    /* Pass 25: Connection view as a 1 B kind (was a malloc'd dup):
     * 0 = no header, 1 = holds a "close" token, 2 = other header.
     * http_resume rebuilds tmp.connection from literals ("close" /
     * "keep-alive" / NULL) so wants_close verdicts are bit-identical. */
    unsigned char conn_kind;
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

/* Pass 24: publish the worker count (call once from main before spawning
 * workers). Derives the adaptive per-worker conn cap and the narrow-box
 * slab caps. Safe to call once; workers default to the 128k floor. */
void net_set_nworkers(int n);
extern int g_nworkers;

/* Re-arm a conn for EPOLLOUT (unsent response pending). 0 ok, -1 on error. */
int net_arm_out(struct conn_hot *c);
/* Pass 25: transport-stash pool (EAGAIN tail buffering only, freed at
 * resume/drain; never a response/query cache). Single WSTASH_CAP class,
 * small per-worker freelist; malloc-backed so a stray free() stays legal.
 * Defined in http.c (owns send_via_conn); net.c drain/free paths release
 * through wstash_release so stashes recycle instead of churning. */
char *wstash_take(void);
void wstash_release(char *p);
/* Live PG_WAIT waiter count on this worker's PG slot (O(1)). The HTTP
 * defer path rejects to the blocking fallback (identical per-route codes)
 * past PG_WAIT_MAX_PER_SLOT / PG_WAIT_MAX on a genuinely busy slot, so the
 * summed intrusive-list length stays bounded (worst case ~PG_WAIT_MAX
 * waiters, all params inline in pend); steady state never
 * approaches it. A bound hit on an idle slot still sends immediately. */
int net_waiter_count(int slot);
/* Pass 21: total live PG_WAIT waiters on this worker (O(1)). Bounds the
 * summed intrusive lists to PG_WAIT_MAX (plus in-flight); see http_defer. */
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
