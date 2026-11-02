#include "pg.h"
#include "json.h"
#include "net.h" /* net_now_wall: 1s-tick-cached wall clock */
#include <libpq-fe.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POOL_N 32
#define POOL_R 24
#define POOL_W 8
#define POOL_W_BASE 24

static _Thread_local PGconn *t_pool[POOL_N];
static _Thread_local unsigned t_rr = 0;
static _Thread_local unsigned t_rr_r = 0;
static _Thread_local unsigned t_rr_w = 0;
static _Thread_local char t_conninfo[2048];

/* Pass 6 async state: one in-flight flag per slot + start stamp for the
 * >50ms slow histogram. Plain async only, no pipeline mode.
 * Pass 9: per-query clock_gettime is sampled 1-in-64 (the stats path is
 * log-only); the stats report itself stays 1/1024 ops + 60s gated. */
static _Thread_local char t_async_busy[POOL_N];
static _Thread_local char t_need_wr[POOL_N];
static _Thread_local char t_async_sampled[POOL_N];
static _Thread_local struct timespec t_async_t0[POOL_N];
static _Thread_local unsigned long t_busy_peak = 0;

/* Pass 3 acquire-wait histogram (log only; query path behavior unchanged). */
static _Thread_local unsigned long t_qops = 0;
static _Thread_local unsigned long t_qslow = 0;
static _Thread_local unsigned long t_recon = 0;
static _Thread_local time_t t_last_rep = 0;

int pg_busy_count(void) {
    int n = 0;
    for (int i = 0; i < POOL_N; i++) if (t_async_busy[i]) n++;
    return n;
}

/* Pass 17: per-pool busy counts (R = slots 0..23, W = 24..31). */
int pg_busy_count_R(void) {
    int n = 0;
    for (int i = 0; i < POOL_R; i++) if (t_async_busy[i]) n++;
    return n;
}

int pg_busy_count_W(void) {
    int n = 0;
    for (int i = POOL_W_BASE; i < POOL_N; i++) if (t_async_busy[i]) n++;
    return n;
}

int pg_slot_is_write(int slot) {
    return slot >= POOL_W_BASE && slot < POOL_N;
}

/* 1 when libpq still holds unsent bytes for the slot (needs POLLOUT),
 * else 0. Lets net arm EPOLLOUT only while a flush is actually pending;
 * an idle PG socket is always writable, so a standing EPOLLOUT arm would
 * spin the event loop at 100% CPU. */
int pg_need_write(int slot) {
    if (slot < 0 || slot >= POOL_N) return 0;
    if (!t_pool[slot] || !t_async_busy[slot]) return 0;
    return t_need_wr[slot] ? 1 : 0;
}

static void pg_report_maybe(void) {
    if ((t_qops & 0x3FFu) != 0) return;
    time_t now = net_now_wall();
    if (now - t_last_rep < 60) return;
    t_last_rep = now;
    fprintf(stderr, "pg stats: ops=%lu slow_gt50ms=%lu reconnects=%lu busy=%d(R%d/W%d) peak=%lu\n",
            t_qops, t_qslow, t_recon, pg_busy_count(), pg_busy_count_R(), pg_busy_count_W(), t_busy_peak);
}

static const char *SQL_HEALTH =
    "SELECT 1";
static const char *SQL_FEED =
    "SELECT p.id,p.body,p.created_at,u.username,"
    "(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count "
    "FROM posts p JOIN users u ON u.id=p.user_id "
    "ORDER BY p.created_at DESC,p.id DESC LIMIT 20";
static const char *SQL_POST =
    "SELECT p.id,p.body,p.created_at,u.username,"
    "(SELECT count(*) FROM likes l WHERE l.post_id=p.id)::int AS like_count "
    "FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=$1";
static const char *SQL_MKPOST =
    "INSERT INTO posts(user_id,body) VALUES($1,$2) RETURNING id,created_at";
static const char *SQL_LIKE =
    "INSERT INTO likes(user_id,post_id) VALUES($1,$2) "
    "ON CONFLICT(user_id,post_id) DO NOTHING";

static const char *sql_for_stmt(const char *stmt) {
    if (!stmt) return NULL;
    if (strcmp(stmt, "q_health") == 0) return SQL_HEALTH;
    if (strcmp(stmt, "q_feed") == 0) return SQL_FEED;
    if (strcmp(stmt, "q_post") == 0) return SQL_POST;
    if (strcmp(stmt, "q_mkpost") == 0) return SQL_MKPOST;
    if (strcmp(stmt, "q_like") == 0) return SQL_LIKE;
    return NULL;
}

/* Thread-local render buffers (feed worst case ~20 x 3.2KB). */
static _Thread_local char t_feed[131072];
static _Thread_local char t_post[8192];

static int wait_conn(PGconn *c, int timeout_ms) {
    int waited = 0;
    while (1) {
        PostgresPollingStatusType st = PQconnectPoll(c);
        if (st == PGRES_POLLING_OK) return 0;
        if (st == PGRES_POLLING_FAILED) return -1;
        int sock = PQsocket(c);
        if (sock < 0) {
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            waited += 10;
        } else {
            short ev = (st == PGRES_POLLING_READING) ? POLLIN : POLLOUT;
            struct pollfd pfd;
            pfd.fd = sock; pfd.events = ev; pfd.revents = 0;
            if (poll(&pfd, 1, 100) < 0) return -1;
            waited += 100;
        }
        if (waited > timeout_ms) return -1;
    }
}

static void pg_prepare_all(PGconn *c) {
    struct { const char *name; const char *sql; int n; } stmts[] = {
        { "q_health", SQL_HEALTH, 0 },
        { "q_feed", SQL_FEED, 0 },
        { "q_post", SQL_POST, 1 },
        { "q_mkpost", SQL_MKPOST, 2 },
        { "q_like", SQL_LIKE, 2 },
    };
    for (size_t i = 0; i < sizeof stmts / sizeof stmts[0]; i++) {
        PGresult *r = PQprepare(c, stmts[i].name, stmts[i].sql, stmts[i].n, NULL);
        if (r) PQclear(r);
    }
}

/* Pass 16: per-session planner SETs (session scope only, no SQL/schema
 * change). Single multi-statement PQexec keeps setup at one round trip.
 * Covers all setup paths: async connectStart, blocking fallback, and every
 * reconnect/probe (all funnel through pg_connect_one). */
static void pg_apply_session_sets(PGconn *c) {
    PGresult *r = PQexec(c,
        "SET statement_timeout=5000;"
        "SET application_name='c-iouring';"
        "SET plan_cache_mode=force_generic_plan;"
        "SET jit=off;"
        "SET max_parallel_workers_per_gather=0;"
        "SET enable_memoize=off");
    if (r) PQclear(r);
}

static PGconn *pg_connect_finish(PGconn *c) {
    /* Drive an async-started connection to completion, SET timeout, prepare.
     * Returns c on success, NULL on failure (already finished). */
    if (!c) return NULL;
    if (PQstatus(c) == CONNECTION_BAD) { PQfinish(c); return NULL; }
    if (PQsetnonblocking(c, 1) != 0) { PQfinish(c); return NULL; }
    if (wait_conn(c, 5000) != 0) { PQfinish(c); return NULL; }
    if (PQstatus(c) != CONNECTION_OK) { PQfinish(c); return NULL; }
    pg_apply_session_sets(c);
    pg_prepare_all(c);
    return c;
}

static PGconn *pg_connect_one(void) {
    if (t_conninfo[0] == '\0') return NULL;
    /* 1) URI-aware async start: URI + extra keywords in one conninfo string. */
    {
        char ci[2560];
        snprintf(ci, sizeof ci,
            "%s connect_timeout=5 options='-c statement_timeout=5000' application_name=c-iouring",
            t_conninfo);
        PGconn *c = pg_connect_finish(PQconnectStart(ci));
        if (c) return c;
    }
    /* 2) Plain blocking connectdb fallback (handles keyword edge cases). */
    {
        PGconn *c = PQconnectdb(t_conninfo);
        if (c && PQstatus(c) == CONNECTION_OK) {
            if (PQsetnonblocking(c, 1) != 0) { PQfinish(c); return NULL; }
            pg_apply_session_sets(c);
            pg_prepare_all(c);
            return c;
        }
        if (c) PQfinish(c);
        return NULL;
    }
}

void pg_init_thread(const char *conninfo) {
    if (conninfo && *conninfo) {
        strncpy(t_conninfo, conninfo, sizeof t_conninfo - 1);
        t_conninfo[sizeof t_conninfo - 1] = '\0';
    }
    if (t_conninfo[0] == '\0') return;
    int ok = 0;
    for (int i = 0; i < POOL_N; i++) {
        t_pool[i] = pg_connect_one();
        if (t_pool[i]) ok++;
        t_async_busy[i] = 0;
        t_need_wr[i] = 0;
        t_async_sampled[i] = 0;
    }
    t_busy_peak = 0;
    fprintf(stderr, "pg pool: %d/%d conns ok (24R+8W split)\n", ok, POOL_N);
}

int pg_claim_slot(void) {
    int s = (int)(t_rr % POOL_N);
    t_rr++;
    return s;
}

/* Pass 17: pool-aware claims. R and W have independent round-robins so a
 * read burst cannot starve writes and vice versa.
 * 404 fix: prefer an idle slot (bounded scan from the cursor) so waiters
 * share a slot only when the whole pool is genuinely in flight; sharing is
 * what forced queued/BUSY retries and stole results pre-fix. When every
 * slot is busy, fall back to round-robin (the waiter queues, sent==0). */
int pg_claim_slot_R(void) {
    for (int i = 0; i < POOL_R; i++) {
        int s = (int)((t_rr_r + (unsigned)i) % POOL_R);
        if (!t_async_busy[s]) { t_rr_r = (unsigned)s + 1; return s; }
    }
    int s = (int)(t_rr_r % POOL_R);
    t_rr_r++;
    return s;
}

int pg_claim_slot_W(void) {
    for (int i = 0; i < POOL_W; i++) {
        int s = POOL_W_BASE + (int)((t_rr_w + (unsigned)i) % POOL_W);
        if (!t_async_busy[s]) { t_rr_w = (unsigned)(s - POOL_W_BASE) + 1; return s; }
    }
    int s = POOL_W_BASE + (int)(t_rr_w % POOL_W);
    t_rr_w++;
    return s;
}

/* MKPOST(4)/LIKE(5) -> W pool, everything else -> R pool. Constants mirror
 * PG_ROUTE_MKPOST/PG_ROUTE_LIKE in net.h without a hard dependency. */
int pg_claim_slot_for_route(int route) {
    if (route == 4 || route == 5) return pg_claim_slot_W();
    return pg_claim_slot_R();
}

/* Sync-path slot: skip async-busy slots to preserve single in-flight per
 * conn. Returns -1 when every slot is async-busy (caller fails over). */
static int pg_slot_sync(void) {
    for (int i = 0; i < POOL_N; i++) {
        int s = pg_claim_slot();
        if (!t_async_busy[s]) return s;
    }
    return -1;
}

/* Pass 17: pool-scoped sync slots. Blocking fallbacks stay in their route
 * pool so a busy R pool never borrows (and blocks) a W conn. */
static int pg_slot_sync_R(void) {
    for (int i = 0; i < POOL_R; i++) {
        int s = pg_claim_slot_R();
        if (!t_async_busy[s]) return s;
    }
    return -1;
}

static int pg_slot_sync_W(void) {
    for (int i = 0; i < POOL_W; i++) {
        int s = pg_claim_slot_W();
        if (!t_async_busy[s]) return s;
    }
    return -1;
}

/* Exec a prepared statement with reconnect-once + inline fallback.
 * Returns result (caller checks status + PQclear) or NULL on transport failure. */
/* Timed exec wrapper for the histogram: the two clock reads are sampled
 * 1-in-64 (vdso or not, this removes them from the hot path); per-query
 * behavior is unchanged, only histogram coverage is sampled. */
static PGresult *pg_exec_timed(PGconn *c, const char *stmt, const char *sql,
                               int npar, const char *const *par, int prepared) {
    int sample = ((t_qops & 63) == 0);
    struct timespec q0 = { 0, 0 }, q1 = { 0, 0 };
    if (sample) clock_gettime(CLOCK_MONOTONIC, &q0);
    /* Pass 8: binary results (resultFormat=1). Params stay text (NULL
     * paramFormats); TEXT columns decode identically, ints/timestamps
     * arrive as big-endian binary. */
    PGresult *r = prepared
        ? PQexecPrepared(c, stmt, npar, par, NULL, NULL, 1)
        : PQexecParams(c, sql, npar, NULL, par, NULL, NULL, 1);
    t_qops++;
    if (sample) {
        clock_gettime(CLOCK_MONOTONIC, &q1);
        long ms = (q1.tv_sec - q0.tv_sec) * 1000L + (q1.tv_nsec - q0.tv_nsec) / 1000000L;
        if (ms > 50) t_qslow++;
    }
    pg_report_maybe();
    return r;
}

static PGresult *pg_run(int slot, const char *stmt, const char *sql, int npar, const char *const *par) {
    if (slot < 0 || slot >= POOL_N) return NULL;
    if (t_async_busy[slot]) return NULL; /* single in-flight: never steal an async slot */
    PGconn *c = t_pool[slot];
    if (!c || PQstatus(c) == CONNECTION_BAD) {
        if (c) PQfinish(c);
        c = pg_connect_one();
        t_pool[slot] = c;
        t_recon++;
        pg_report_maybe();
        if (!c) return NULL;
    }
    /* Blocking API consumes inline; defensively drain stale input on a reused
     * conn (PQisBusy is buffer-state only: no syscall when idle). */
    { int guard = 0; while (PQisBusy(c) && guard++ < 8) { if (!PQconsumeInput(c)) break; } }
    PGresult *r = pg_exec_timed(c, stmt, sql, npar, par, 1);
    if (r) {
        ExecStatusType st = PQresultStatus(r);
        if (st == PGRES_TUPLES_OK || st == PGRES_COMMAND_OK) return r;
        /* Real query errors (e.g. FK 23503) keep the connection healthy:
         * return as-is so the caller can inspect SQLSTATE. */
        if (PQstatus(c) == CONNECTION_OK) {
            const char *m = PQresultErrorMessage(r);
            if (!m || !strstr(m, "does not exist")) return r;
        }
        PQclear(r);
        r = NULL;
    }
    if (!t_pool[slot] || PQstatus(t_pool[slot]) == CONNECTION_BAD) {
        if (t_pool[slot]) PQfinish(t_pool[slot]);
        c = pg_connect_one();
        t_pool[slot] = c;
        t_recon++;
        pg_report_maybe();
        if (!c) return NULL;
    } else {
        c = t_pool[slot];
    }
    return pg_exec_timed(c, stmt, sql, npar, par, 0);
}

static void set_err(char *eb, size_t ecap, PGconn *c, PGresult *r) {
    if (!eb || ecap == 0) return;
    const char *m = NULL;
    if (r) m = PQresultErrorMessage(r);
    if ((!m || !*m) && c) m = PQerrorMessage(c);
    if (!m || !*m) m = "db unreachable";
    snprintf(eb, ecap, "%s", m);
    size_t l = strlen(eb);
    while (l && (eb[l - 1] == '\n' || eb[l - 1] == '\r')) eb[--l] = '\0';
}

/* ---- Pass 6 async primitives ---- */
/* Pass 19 (Greptile #13): explicit per-slot owner. Only the owner that
 * claimed the slot (owner pointer + claim generation ogen) may consume its
 * result. A recycled conn struct reuses addresses, so the pointer alone is
 * not enough -- the monotonic per-thread generation disambiguates. Orphan
 * reclaim (pg_slot_heal) runs only when no live waiter exists, so it can
 * never race a live owner in this single-threaded worker. */
static _Thread_local void *t_async_owner[POOL_N];
static _Thread_local unsigned t_async_own_gen[POOL_N];

static inline void slot_clear_owner(int slot) {
    t_async_owner[slot] = NULL;
    t_async_own_gen[slot] = 0;
}

int pg_send_async(int slot, const char *stmt, int npar, const char *const *par,
                  void *owner, unsigned ogen) {
    if (slot < 0 || slot >= POOL_N || !stmt) return PG_ASYNC_ERR;
    /* Ownership gate (404 fix): a slot with a query in flight is owned by
     * its waiter until that waiter polls the result. Never consume/discard
     * here: the bytes may be another waiter's complete-but-unpolled result,
     * and discarding it cross-wires the slot to a foreign result (e.g. a
     * FEED result rendered as a single post -> bogus 404 for a post that
     * exists, growing with load/slot-sharing). A busy slot simply queues
     * the new waiter (BUSY); its retry runs only after the owner polled
     * and freed the slot. True orphans (no live waiter) are reclaimed by
     * pg_slot_heal(), which net calls only when the slot has no waiters. */
    if (t_async_busy[slot]) return PG_ASYNC_BUSY;
    PGconn *c = t_pool[slot];
    if (!c || PQstatus(c) == CONNECTION_BAD) {
        if (c) PQfinish(c);
        c = pg_connect_one();
        t_pool[slot] = c;
        t_recon++;
        pg_report_maybe();
        if (!c) return PG_ASYNC_ERR;
        t_async_busy[slot] = 0;
        t_need_wr[slot] = 0;
    }
    /* Ownerless stale input only (busy==0 above, so no live waiter will
     * poll this input; discarding it cannot steal anyone's result). */
    if (PQisBusy(c)) {
        if (!PQconsumeInput(c)) {
            if (PQstatus(c) == CONNECTION_BAD) {
                PQfinish(c);
                t_pool[slot] = NULL;
                t_async_busy[slot] = 0;
                t_need_wr[slot] = 0;
                slot_clear_owner(slot);
                c = pg_connect_one();
                t_pool[slot] = c;
                t_recon++;
                pg_report_maybe();
                if (!c) return PG_ASYNC_ERR;
            } else {
                return PG_ASYNC_BUSY;
            }
        } else if (PQisBusy(c)) {
            return PG_ASYNC_BUSY;
        } else {
            PGresult *d;
            while ((d = PQgetResult(c)) != NULL) PQclear(d);
            t_async_busy[slot] = 0;
            t_need_wr[slot] = 0;
            slot_clear_owner(slot);
        }
    }
    /* t_async_busy[slot]==0 here (gated at entry; every path above either
     * returns or leaves it clear), so the send below owns the slot. */
    const char *sql = sql_for_stmt(stmt);
    int ok = PQsendQueryPrepared(c, stmt, npar, par, NULL, NULL, 1);
    if (!ok) {
        const char *m = PQerrorMessage(c);
        if (m && strstr(m, "does not exist") && sql)
            ok = PQsendQueryParams(c, sql, npar, NULL, par, NULL, NULL, 1);
    }
    if (!ok) {
        if (PQstatus(c) == CONNECTION_BAD) {
            PQfinish(c);
            t_pool[slot] = NULL;
            t_async_busy[slot] = 0;
            t_need_wr[slot] = 0;
            slot_clear_owner(slot);
            c = pg_connect_one();
            t_pool[slot] = c;
            t_recon++;
            pg_report_maybe();
            if (!c) return PG_ASYNC_ERR;
            ok = PQsendQueryPrepared(c, stmt, npar, par, NULL, NULL, 1);
            if (!ok && sql) {
                const char *m2 = PQerrorMessage(c);
                if (m2 && strstr(m2, "does not exist"))
                    ok = PQsendQueryParams(c, sql, npar, NULL, par, NULL, NULL, 1);
            }
            if (!ok) return PG_ASYNC_ERR;
        } else {
            return PG_ASYNC_ERR;
        }
    }
    t_async_busy[slot] = 1;
    t_async_owner[slot] = owner;
    t_async_own_gen[slot] = ogen;
    /* Pass 9: completion-latency clock sampled 1-in-64 with the query op. */
    if ((t_qops & 63) == 0) {
        clock_gettime(CLOCK_MONOTONIC, &t_async_t0[slot]);
        t_async_sampled[slot] = 1;
    } else {
        t_async_sampled[slot] = 0;
    }
    t_qops++;
    {
        int cur = pg_busy_count();
        if ((unsigned long)cur > t_busy_peak) t_busy_peak = (unsigned long)cur;
    }
    { /* Opportunistic flush; POLLOUT (via pg_need_write) drives the rest. */
        int fl = PQflush(c);
        if (fl == -1) {
            PQfinish(c);
            t_pool[slot] = NULL;
            t_async_busy[slot] = 0;
            t_need_wr[slot] = 0;
            slot_clear_owner(slot);
            t_recon++;
            pg_report_maybe();
            return PG_ASYNC_ERR;
        }
        t_need_wr[slot] = (fl == 1) ? 1 : 0;
    }
    pg_report_maybe();
    return PG_ASYNC_OK;
}

int pg_poll(int slot, PGresult **out, void *owner, unsigned ogen) {
    if (out) *out = NULL;
    if (slot < 0 || slot >= POOL_N) return PG_POLL_ERROR;
    PGconn *c = t_pool[slot];
    if (!c) { t_async_busy[slot] = 0; t_need_wr[slot] = 0; slot_clear_owner(slot); return PG_POLL_ERROR; }
    if (!t_async_busy[slot]) return PG_POLL_ERROR;
    /* Owner check (Greptile #13): a foreign waiter must never consume
     * another waiter's completed result. PENDING leaves the slot untouched;
     * the true owner will poll it. Pointer reuse across free/malloc is
     * disambiguated by the claim generation. */
    if (t_async_owner[slot] != owner || t_async_own_gen[slot] != ogen)
        return PG_POLL_PENDING;
    int fl = PQflush(c);
    if (fl == -1) {
        PQfinish(c);
        t_pool[slot] = NULL;
        t_async_busy[slot] = 0;
        t_need_wr[slot] = 0;
        slot_clear_owner(slot);
        t_recon++;
        pg_report_maybe();
        return PG_POLL_ERROR;
    }
    if (fl == 1) {
        t_need_wr[slot] = 1;
        return PG_POLL_PENDING; /* need POLLOUT */
    }
    t_need_wr[slot] = 0;
    if (!PQconsumeInput(c)) {
        if (PQstatus(c) == CONNECTION_BAD) {
            PQfinish(c);
            t_pool[slot] = NULL;
            t_async_busy[slot] = 0;
            t_need_wr[slot] = 0;
            slot_clear_owner(slot);
            t_recon++;
            pg_report_maybe();
            return PG_POLL_ERROR;
        }
        return PG_POLL_PENDING;
    }
    if (PQisBusy(c)) return PG_POLL_PENDING;
    PGresult *last = NULL, *r;
    while ((r = PQgetResult(c)) != NULL) {
        if (last) PQclear(last);
        last = r;
    }
    t_async_busy[slot] = 0;
    t_need_wr[slot] = 0;
    slot_clear_owner(slot);
    if (t_async_sampled[slot]) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t_async_t0[slot].tv_sec) * 1000L +
                  (now.tv_nsec - t_async_t0[slot].tv_nsec) / 1000000L;
        if (ms > 50) t_qslow++;
        pg_report_maybe();
    }
    if (!last) {
        if (PQstatus(c) == CONNECTION_BAD) {
            PQfinish(c);
            t_pool[slot] = NULL;
            t_recon++;
            pg_report_maybe();
        }
        return PG_POLL_ERROR;
    }
    {
        ExecStatusType st = PQresultStatus(last);
        if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
            const char *m = PQresultErrorMessage(last);
            if (m && strstr(m, "does not exist")) {
                /* Prepared-statement race: let the caller retry via the
                 * blocking inline fallback, mirroring pg_run. */
                PQclear(last);
                return PG_POLL_ERROR;
            }
        }
    }
    if (out) *out = last;
    else PQclear(last);
    return PG_POLL_READY;
}

int pg_socket(int slot) {
    if (slot < 0 || slot >= POOL_N) return -1;
    PGconn *c = t_pool[slot];
    if (!c) return -1;
    return PQsocket(c);
}

int pg_slot_busy(int slot) {
    if (slot < 0 || slot >= POOL_N) return 0;
    return t_async_busy[slot] ? 1 : 0;
}

/* Reclaim a slot with no live waiter (the caller in net guarantees the
 * waiter count is zero AND the slot's owner generation has no live waiter,
 * so no owner can be polled concurrently: this is single-threaded and the
 * owner always holds its waiter count). Covers waiters freed mid-flight
 * (client gone, sweep) and transport-error races where the waiter already
 * failed over to the blocking path: if the orphan query has completed (or
 * the conn is bad), discard the stale results and free the slot; if it is
 * still in flight, leave it for a later heal.
 * Never touches a slot that has a live waiter, so it cannot steal results. */
void pg_slot_heal(int slot) {
    if (slot < 0 || slot >= POOL_N) return;
    PGconn *c = t_pool[slot];
    if (!c) { t_async_busy[slot] = 0; t_need_wr[slot] = 0; slot_clear_owner(slot); return; }
    if (PQstatus(c) == CONNECTION_BAD) {
        PQfinish(c);
        t_pool[slot] = NULL;
        t_async_busy[slot] = 0;
        t_need_wr[slot] = 0;
        slot_clear_owner(slot);
        t_recon++;
        pg_report_maybe();
        return;
    }
    (void)PQflush(c);
    if (!PQconsumeInput(c)) {
        if (PQstatus(c) == CONNECTION_BAD) {
            PQfinish(c);
            t_pool[slot] = NULL;
            t_async_busy[slot] = 0;
            t_need_wr[slot] = 0;
            slot_clear_owner(slot);
            t_recon++;
            pg_report_maybe();
        }
        return;
    }
    if (PQisBusy(c)) return; /* still in flight: a later heal frees it */
    {
        PGresult *d;
        while ((d = PQgetResult(c)) != NULL) PQclear(d);
    }
    t_async_busy[slot] = 0;
    t_need_wr[slot] = 0;
    slot_clear_owner(slot);
}

/* ---- render helpers (shared by blocking + async completion) ---- */

int pg_render_health(PGresult *r, char *errbuf, size_t errcap) {
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        set_err(errbuf, errcap, NULL, r);
        return -1;
    }
    return 0;
}

/* ---- Pass 8: binary decoders (resultFormat=1) ----
 * Params stay text; results arrive binary. TEXT columns decode identically
 * (binary TEXT == bytes). int2/int4/int8 arrive big-endian; timestamptz
 * arrives int64 microseconds since 2000-01-01 00:00:00 UTC. Any NULL
 * (should not happen) or short value fails the render -> caller maps to
 * 500, preserving the invariant that 404s only come from SQLSTATE/ntuples.
 * A text-format fallback (PQfformat==0) is kept for safety so a missed
 * negotiation can never change golden bytes. */

/* Postgres -> unix epoch offset in microseconds (2000-01-01 UTC). */
#define PG_EPOCH_UNIX_MICROS 946684800000000LL

static uint64_t rd_be64(const unsigned char *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

static uint32_t rd_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Binary int (len 8/4/2, big-endian signed) -> long long. 0 ok, -1 fail. */
static int pg_bin_int(const char *v, int len, long long *out) {
    if (!v || len < 0 || !out) return -1;
    const unsigned char *p = (const unsigned char *)v;
    if (len == 8) {
        *out = (long long)(int64_t)rd_be64(p);
        return 0;
    }
    if (len == 4) {
        *out = (long long)(int32_t)rd_be32(p);
        return 0;
    }
    if (len == 2) {
        int16_t s = (int16_t)((p[0] << 8) | p[1]);
        *out = (long long)s;
        return 0;
    }
    return -1;
}

/* Binary timestamptz micros -> "YYYY-MM-DDTHH:MI:SS.mmmZ" (truncate, zero-pad).
 * 0 ok, -1 fail. Pure integer civil-date (Hinnant), no string parsing. */
static int fmt_ts_micros(int64_t pg_micros, char *out, size_t cap) {
    if (!out || cap < 25) return -1;
    int64_t um = pg_micros + PG_EPOCH_UNIX_MICROS;
    int64_t secs = (um >= 0) ? um / 1000000LL : (um - 999999LL) / 1000000LL;
    int64_t rem = um - secs * 1000000LL; /* 0..999999 */
    int ms = (int)(rem / 1000LL);        /* truncate micros -> ms */
    int64_t days = (secs >= 0) ? secs / 86400LL : (secs - 86399LL) / 86400LL;
    int64_t sod = secs - days * 86400LL; /* 0..86399 */
    int64_t z = days + 719468LL;
    int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
    uint64_t doe = (uint64_t)(z - era * 146097LL);
    uint64_t yoe = (doe - doe / 1460ULL + doe / 36524ULL - doe / 146096ULL) / 365ULL;
    long long y = (long long)yoe + era * 400LL;
    uint64_t doy = doe - (365ULL * yoe + yoe / 4ULL - yoe / 100ULL);
    uint64_t mp = (5ULL * doy + 2ULL) / 153ULL;
    unsigned d = (unsigned)(doy - (153ULL * mp + 2ULL) / 5ULL + 1ULL);
    unsigned m = (unsigned)(mp + (mp < 10 ? 3 : (uint64_t)-9));
    y += (m <= 2);
    unsigned hh = (unsigned)(sod / 3600LL);
    unsigned mm = (unsigned)((sod % 3600LL) / 60LL);
    unsigned ss = (unsigned)(sod % 60LL);
    int n = snprintf(out, cap, "%04lld-%02u-%02uT%02u:%02u:%02u.%03dZ",
                     y, m, d, hh, mm, ss, ms);
    if (n != 24) return -1;
    return 0;
}

/* Decode one integer column (binary fast path, text fallback). */
static int pg_col_int(PGresult *r, int row, int col, long long *out) {
    if (PQgetisnull(r, row, col)) return -1;
    if (PQfformat(r, col) == 0) {
        const char *v = PQgetvalue(r, row, col);
        if (!v || !*v) return -1;
        char *e = NULL;
        long long x = strtoll(v, &e, 10);
        if (e == v) return -1;
        *out = x;
        return 0;
    }
    return pg_bin_int(PQgetvalue(r, row, col), PQgetlength(r, row, col), out);
}

/* Decode one timestamptz column into the golden "....mmmZ" shape. */
static int pg_col_ts(PGresult *r, int row, int col, char *ts, size_t cap) {
    if (PQgetisnull(r, row, col)) return -1;
    if (PQfformat(r, col) == 0) {
        const char *v = PQgetvalue(r, row, col);
        if (!v) return -1;
        if (json_fmt_ts(v, strlen(v), ts, cap) != 0) return -1;
        return 0;
    }
    if (PQgetlength(r, row, col) != 8) return -1;
    const unsigned char *p = (const unsigned char *)PQgetvalue(r, row, col);
    int64_t micros = (int64_t)rd_be64(p);
    if (fmt_ts_micros(micros, ts, cap) != 0) return -1;
    return 0;
}

/* Render one post row from a binary PGresult into o/cap.
 * Returns length, or (size_t)-1 on NULL/decode/truncation error. */
static size_t render_row_bin(char *o, size_t cap, PGresult *r, int row) {
    long long idv, likesv;
    char ts[32];
    char eb[3100];
    char ea[512];
    char idbuf[32], likebuf[32];
    const char *body, *author;
    int bodylen, authorlen;
    if (PQgetisnull(r, row, 0) || PQgetisnull(r, row, 1) ||
        PQgetisnull(r, row, 2) || PQgetisnull(r, row, 3) ||
        PQgetisnull(r, row, 4))
        return (size_t)-1;
    if (pg_col_int(r, row, 0, &idv) != 0) return (size_t)-1;
    if (pg_col_int(r, row, 4, &likesv) != 0) return (size_t)-1;
    if (pg_col_ts(r, row, 2, ts, sizeof ts) != 0) {
        snprintf(ts, sizeof ts, "1970-01-01T00:00:00.000Z");
    }
    body = PQgetvalue(r, row, 1);
    bodylen = PQgetlength(r, row, 1);
    author = PQgetvalue(r, row, 3);
    authorlen = PQgetlength(r, row, 3);
    /* TEXT arrives identical in binary mode; body/author stay byte-based. */
    json_escape_n(body, bodylen < 0 ? 0 : (size_t)bodylen, eb, sizeof eb);
    json_escape_n(author, authorlen < 0 ? 0 : (size_t)authorlen, ea, sizeof ea);
    snprintf(idbuf, sizeof idbuf, "%lld", idv);
    snprintf(likebuf, sizeof likebuf, "%lld", likesv);
    int n = snprintf(o, cap,
        "{\"id\":%s,\"body\":\"%s\",\"created_at\":\"%s\",\"author\":\"%s\",\"like_count\":%s}",
        idbuf, eb, ts, ea, likebuf);
    if (n < 0) { if (cap) o[0] = '\0'; return (size_t)-1; }
    if ((size_t)n >= cap) return (size_t)-1;
    return (size_t)n;
}

int pg_render_feed(PGresult *r, const char **out, size_t *len, char *errbuf, size_t errcap) {
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        set_err(errbuf, errcap, NULL, r);
        return -1;
    }
    int rows = PQntuples(r);
    size_t o = 0, cap = sizeof t_feed;
    t_feed[0] = '\0';
    {
        const char *pre = "{\"posts\":[";
        size_t pl = strlen(pre);
        if (pl + 2 > cap) return -1;
        memcpy(t_feed, pre, pl);
        o = pl;
    }
    for (int i = 0; i < rows; i++) {
        char row[4200];
        size_t rl = render_row_bin(row, sizeof row, r, i);
        if (rl == (size_t)-1) {
            if (errbuf && errcap) snprintf(errbuf, errcap, "binary decode failed");
            return -1;
        }
        if (o + rl + 2 > cap) return -1;
        if (i) t_feed[o++] = ',';
        memcpy(t_feed + o, row, rl);
        o += rl;
    }
    if (o + 3 > cap) return -1;
    t_feed[o++] = ']';
    t_feed[o++] = '}';
    t_feed[o] = '\0';
    *out = t_feed;
    *len = o;
    return 0;
}

int pg_render_post(PGresult *r, const char **out, size_t *len, char *errbuf, size_t errcap) {
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        if (sqlstate && (strcmp(sqlstate, "23503") == 0 || strcmp(sqlstate, "22003") == 0 || strcmp(sqlstate, "22P02") == 0)) {
            return -2;
        }
        set_err(errbuf, errcap, NULL, r);
        return -1;
    }
    if (PQntuples(r) != 1) {
        return -2;
    }
    char row[4200];
    size_t rl = render_row_bin(row, sizeof row, r, 0);
    if (rl == (size_t)-1) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "binary decode failed");
        return -1;
    }
    {
        const char *pre = "{\"post\":";
        size_t pl = strlen(pre);
        if (pl + rl + 2 > sizeof t_post) return -1;
        memcpy(t_post, pre, pl);
        memcpy(t_post + pl, row, rl);
        t_post[pl + rl] = '}';
        t_post[pl + rl + 1] = '\0';
        *out = t_post;
        *len = pl + rl + 1;
    }
    return 0;
}

int pg_render_mkpost(PGresult *r, const char *body, size_t body_n, const char *author,
                     const char **out, size_t *len, char *errbuf, size_t errcap) {
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        set_err(errbuf, errcap, NULL, r);
        return -1;
    }
    if (PQgetisnull(r, 0, 0) || PQgetisnull(r, 0, 1)) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "binary decode failed");
        return -1;
    }
    long long new_idv = 0;
    char ts[32], eb[3100], ea[512], idbuf[32];
    if (PQfformat(r, 0) == 0) {
        const char *v = PQgetvalue(r, 0, 0);
        char *e = NULL;
        new_idv = strtoll(v ? v : "", &e, 10);
        if (!v || e == v) {
            if (errbuf && errcap) snprintf(errbuf, errcap, "binary decode failed");
            return -1;
        }
    } else {
        if (pg_bin_int(PQgetvalue(r, 0, 0), PQgetlength(r, 0, 0), &new_idv) != 0) {
            if (errbuf && errcap) snprintf(errbuf, errcap, "binary decode failed");
            return -1;
        }
    }
    if (pg_col_ts(r, 0, 1, ts, sizeof ts) != 0)
        snprintf(ts, sizeof ts, "1970-01-01T00:00:00.000Z");
    snprintf(idbuf, sizeof idbuf, "%lld", new_idv);
    json_escape_n(body ? body : "", body_n, eb, sizeof eb);
    json_escape_n(author ? author : "", author ? strlen(author) : 0, ea, sizeof ea);
    int n = snprintf(t_post, sizeof t_post,
        "{\"post\":{\"id\":%s,\"body\":\"%s\",\"created_at\":\"%s\",\"author\":\"%s\",\"like_count\":0}}",
        idbuf, eb, ts, ea);
    if (n < 0 || (size_t)n >= sizeof t_post) return -1;
    *out = t_post;
    *len = (size_t)n;
    return 0;
}

int pg_render_like(PGresult *r, int *already, char *errbuf, size_t errcap) {
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (PQresultStatus(r) == PGRES_COMMAND_OK) {
        const char *t = PQcmdTuples(r);
        *already = (t && strcmp(t, "0") == 0) ? 1 : 0;
        return 0;
    }
    /* FK/range/invalid-text (post missing or out-of-range id) -> 404. */
    {
        const char *sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        if (sqlstate && (strcmp(sqlstate, "23503") == 0 || strcmp(sqlstate, "22003") == 0 || strcmp(sqlstate, "22P02") == 0)) {
            return -2;
        }
    }
    set_err(errbuf, errcap, NULL, r);
    return -1;
}

int pg_health_check(char *errbuf, unsigned long errcap) {
    int slot = pg_slot_sync_R();
    if (slot < 0) slot = pg_slot_sync();
    if (slot < 0) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    if (!t_pool[slot]) {
        PGconn *tmp = pg_connect_one();
        if (tmp) PQfinish(tmp);
    }
    PGresult *r = pg_run(slot, "q_health", SQL_HEALTH, 0, NULL);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    int rc = pg_render_health(r, errbuf, errcap);
    PQclear(r);
    return rc;
}

int pg_feed(const char **out, size_t *len, char *errbuf, size_t errcap) {
    int slot = pg_slot_sync_R();
    if (slot < 0) slot = pg_slot_sync();
    if (slot < 0) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    PGresult *r = pg_run(slot, "q_feed", SQL_FEED, 0, NULL);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    int rc = pg_render_feed(r, out, len, errbuf, errcap);
    PQclear(r);
    return rc;
}

int pg_get_post(const char *idstr, const char **out, size_t *len, char *errbuf, size_t errcap) {
    const char *par[1] = { idstr };
    int slot = pg_slot_sync_R();
    if (slot < 0) slot = pg_slot_sync();
    if (slot < 0) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    PGresult *r = pg_run(slot, "q_post", SQL_POST, 1, par);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    int rc = pg_render_post(r, out, len, errbuf, errcap);
    PQclear(r);
    return rc;
}

int pg_create_post(const char *uid, const char *body, size_t body_n, const char *author,
                   const char **out, size_t *len, char *errbuf, size_t errcap) {
    /* NUL-terminate the trimmed body for the text param. */
    static _Thread_local char t_body[17408];
    size_t bn = body_n < sizeof t_body - 1 ? body_n : sizeof t_body - 1;
    memcpy(t_body, body, bn);
    t_body[bn] = '\0';
    const char *par[2] = { uid, t_body };
    int slot = pg_slot_sync_W();
    if (slot < 0) slot = pg_slot_sync();
    if (slot < 0) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    PGresult *r = pg_run(slot, "q_mkpost", SQL_MKPOST, 2, par);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    int rc = pg_render_mkpost(r, t_body, bn, author, out, len, errbuf, errcap);
    PQclear(r);
    return rc;
}

int pg_like(const char *uid, const char *pid, int *already, char *errbuf, size_t errcap) {
    const char *par[2] = { uid, pid };
    int slot = pg_slot_sync_W();
    if (slot < 0) slot = pg_slot_sync();
    if (slot < 0) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    PGresult *r = pg_run(slot, "q_like", SQL_LIKE, 2, par);
    if (!r) {
        if (errbuf && errcap) snprintf(errbuf, errcap, "db unreachable");
        return -1;
    }
    int rc = pg_render_like(r, already, errbuf, errcap);
    PQclear(r);
    return rc;
}

void pg_fini_thread(void) {
    for (int i = 0; i < POOL_N; i++) {
        if (t_pool[i]) { PQfinish(t_pool[i]); t_pool[i] = NULL; }
        t_async_busy[i] = 0;
        t_need_wr[i] = 0;
        t_async_sampled[i] = 0;
    }
}
