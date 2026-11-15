// What host/epoll.c and host/pg.c share. The compiler splices both into one
// translation unit after the runtime (vendor/bend/guide/EFFECTS.md), in an
// order this header does not rely on: every name here is declared before
// either file uses it. Outside every proof.
#ifndef BM_HOST_H
#define BM_HOST_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "json.h"
#include "latemap.h"

// ---- the flags (host/epoll.c reads them in Epoll.open) ----

enum { DB_PG, DB_CANNED };
// --pick: which of its kind's ready connections a flushed batch takes (host/pg.c, pg_pick).
// A named type, so that a switch over it must name every pick.
typedef enum {
  PICK_SPREAD,      // the fewest batches in flight, ties to a scan start that moves one slot each pick
  PICK_FILL,        // the kind's first slot with fewer than --fill-depth in flight (S39)
  PICK_FILL_READS,  // a read batch as PICK_FILL, a write batch as PICK_SPREAD (S39)
} HostPick;

typedef struct {
  uint32_t    port, extra_port;
  uint32_t    db;             // DB_PG or DB_CANNED
  uint32_t    pool;           // connections, 1 .. 511; (pool + 1 cancel) x workers <= 512 (exit 64)
  uint32_t    batch_max;      // statements per batch, 1 .. 1024
  uint32_t    read_batch_max; // --read-batch-max: the read batch's own cap, 1 .. batch_max (default batch_max)
  uint32_t    sync_max;       // --sync-max: Synced events per turn, at most; 0 is no bound
  uint32_t    batch_wait_us;  // a read batch flushes this long after its first Queue
  uint32_t    write_slots;    // the pool's last N slots take write batches, the rest read batches;
                              // 0: any slot takes either (the pick before S15)
  HostPick    pick;           // --pick spread|fill|fill-reads
  uint32_t    fill_depth;     // --fill-depth, 1 .. 8: the batches in flight from which the fill rule goes on
                              // to the kind's next slot (pg_pick_fill); PICK_SPREAD does not read it
  uint32_t    depth_max;      // --depth-max D, 0 .. 64 (S63): a read batch is not sent to a connection that carries D
                              // batches in flight; 0, the default, is no bound (host/pg.c, pg_pick)
  uint32_t    busy_max;       // --busy-max N, 0 .. 1000000 (S63): a turn lexes no request while N requests of this
                              // worker have no reply yet; 0, the default, is no bound (host/epoll.c, ep_room)
  uint32_t    write_wait_us;  // a write batch flushes this long after its first Queue
  uint32_t    db_timeout_ms;  // a batch without its ReadyForQuery this long dies
  uint32_t    max;            // client descriptors read per round, 1 .. 1024
  uint32_t    window_us;      // --window-us: a turn after a light wake sleeps to that wake + this; 0 is off
  uint32_t    fuel;           // rounds before the server stops
  uint32_t    max_head;       // --max-head: request line and headers, 512 .. 65536 bytes; over it 431
  uint32_t    max_body;       // --max-body: a Content-Length over it is answered 413, 0 .. 999999
  uint32_t    idle_timeout_s; // --idle-timeout: a connection silent this long is closed; 0 is off
  uint32_t    worker;         // this process's number, 0 .. workers - 1 (--worker)
  uint32_t    workers;        // processes sharing the ports (--workers), 1 without the flag
  bool        sock_opts;      // --sock-opts on: the listener's TCP options and the late-reply TCP_QUICKACK
  // The two quick-ACK options of that bundle, each true when it is in force: its own flag asked
  // for it, or --sock-opts on did (S64). ep_flags writes them; sock_opts implies both.
  bool        ack_first;      // TCP_QUICKACK 0 on the listeners after listen() (--ack-first on; ep_listen)
  bool        ack_late;       // TCP_QUICKACK 0 on a client's socket before a late reply's write (--ack-late on; ep_reply)
  bool        rows_block;     // --rows block: a feed's rows cross to Bend as one Row cell (S33)
  bool        result_mixed;   // --result-format mixed: a feed row's id and like_count come from PostgreSQL as text (S50)
  bool        ring_lifo;      // --ring lifo: newest first off the again ring while its oldest entry is --ring-age old (S35)
  bool        late_map;       // --late-map: each reply's time split into four waits and counted (S48)
  bool        window_hold;    // --window-hold on: a turn takes the window after a full wake and with requests on the ring too (S57)
  uint32_t    ring_age_ms;    // --ring-age: that age, ms
  uint32_t    ring_old;       // --ring-old N: in newest-first turns every N-th arrival comes from the old end
  const char* trace;          // the trace file, or NULL (with --worker K: the name given, .K)
  const char* stats;          // the stats file, or NULL (with --worker K: the name given, .K)
  const char* golden;         // --db canned: the golden directory
} HostConf;

extern HostConf host_conf;

// epoll data: a client is its descriptor; a database slot and a listener
// carry a tag bit and their index.
#define HOST_TAG_PG  (1ull << 62)
#define HOST_TAG_LSN (1ull << 61)

// ---- events (host/epoll.c owns the Wire constructors) ----
// An event is recorded item by item, front to back, then handed over:
// host_ev_end writes it to the trace and appends its Wire to this round's
// list. Strings are copied, so a pointer need live only for the call.
void host_ev_begin(void);
void host_ev_word(uint32_t w);
void host_ev_str(const char* p, size_t n);
void host_ev_open(void);   // a Nest begins
void host_ev_close(void);  // that Nest ends
// One post row as one Row node: the post's id, its like count and the handle
// pg_hold issued for its strings (host/wire.bend reads it as a Rsp.PostRow).
void host_ev_row(uint32_t id, uint32_t likes, uint32_t h);
void host_ev_end(Env e);
// The handles this event issued (host/pg.c), first .. first + n - 1: the
// trace writes their rows beside the event so that replay.sh can render
// what the host rendered. Call before host_ev_end.
void host_ev_rows(uint32_t first, uint32_t n);

// ---- the reactor's descriptor sets ----
int  host_watch(int fd, uint32_t events, uint64_t data);    // add to both sets
int  host_rewatch(int fd, uint32_t events, uint64_t data);  // change in both sets
void host_unwatch(int fd);
int  host_db_set(void);  // the set that holds only database descriptors

uint32_t host_uptime(uint64_t now);

// ---- the trace (--trace): one JSON object per line ----
// host_tl_begin starts a line in host_tl with {"t":<µs since start>, and
// host_tl_end closes it and writes it. Only while host_trace is not NULL.
extern FILE* host_trace;
extern JBuf  host_tl;
void host_tl_begin(void);
void host_tl_end(void);

// A String argument of an effect, consumed (its cells freed) and appended
// to b as UTF-8; escaped as a JSON string body when json is true.
void host_take_str(Env e, Term s, JBuf* b, bool json);

// ---- the database side (host/pg.c) ----
void     pg_tick(Env e, uint64_t now);                          // flush when due, time out, reconnect
void     pg_flush_reads(Env e, uint64_t now);                   // --window-us: the read batch goes now, whatever its clock
uint64_t pg_next(uint64_t now);                                 // the next deadline, or UINT64_MAX
void     pg_ready(Env e, uint64_t data, uint32_t events, uint64_t now);
uint32_t pg_room(void);                                         // arrivals this round may still queue
bool     pg_pending(void);                                      // answers wait in a receive buffer (--sync-max): no sleep
void     pg_close(void);
void     pg_report(void);                                        // batch counts and db_in_bytes to stderr
uint64_t pg_in_bytes(void);                                      // bytes read from the database sockets so far (host/pgwire.h, pgw_in_bytes)
// --depth-max (S63), over the run so far: the times the bound held a read batch back, and the most batches a
// connection carried when a read batch joined it (at most D). For the stats file and the stop line.
void     pg_bound(uint64_t* stalls, uint32_t* most);
// The Queue and Open commands, from host/epoll.c's walk of a round's commands
// (ep_cmd). f: Queue's fields in host/epoll.bend's order (batch, req, kind,
// uid, pid, name, text); its strings are consumed. A Queue for a batch that is
// not the open one of its kind, or an Open out of turn, stops the process
// (exit 70).
void     pg_queue(Env e, const Term* f);
void     pg_open_batch(uint32_t batch, uint32_t kind);

// What the database side has in flight, for the stats file (--stats, host/epoll.c):
// read once a second, so a walk over the slots is cheap enough.
typedef struct {
  uint32_t read_batches, write_batches;  // flushed, not yet Synced or Died
  uint32_t busy_slots;                   // pool slots carrying at least one batch
  uint32_t cancels;                      // cancel requests in flight
  uint32_t cancels_waiting;              // slots waiting to send their cancel
  uint32_t handles;                      // row handles live until the next Epoll.turn
} PgStats;
void pg_stats(PgStats* out);

// ---- segments by class (S23, docs/specs/S23-packets-by-class.md) ----
// One class's segment counts, summed over its sockets: every segment in and out, and those
// that carried data (the kernel's tcpi_segs_in, tcpi_segs_out, tcpi_data_segs_in and
// tcpi_data_segs_out). Each class's sums only grow.
typedef struct {
  uint64_t in, out, data_in, data_out;
} HostSegs;
// Adds fd's four counters to s (host/epoll.c); nothing when TCP_INFO fails or is too short.
void host_segs_add(HostSegs* s, int fd);
// The database class (host/pg.c): the sockets closed so far and every socket open now, the
// pool's and the cancel connection's. A walk over the slots: once a stats second, and at stop.
void pg_segs(HostSegs* out);

// ---- the late map (--late-map, docs/specs/S48-the-host-says-where-a-late-reply-waited.md) ----
// host/epoll.c keeps the table; host/pg.c tells it each batch's two instants. Both calls are
// made only with the flag.
// The batch that carried request req ended: it was first written to a database socket at out
// and the read that brought its ReadyForQuery returned at in (io_tick() times; equal when the
// batch was never sent, or with --db canned), and repair is what that socket's TCP saw between
// the two (LM_DB_RESENT, LM_DB_OOO or LM_DB_NEITHER, host/latemap.h).
void host_late_db(uint32_t req, uint64_t out, uint64_t in, uint8_t repair);
// fd's tcpi_total_retrans and tcpi_rcv_ooopack into *retrans and *ooo, and true; false, and
// both untouched, when TCP_INFO fails or is too short to hold them.
bool host_tcp_repair(int fd, uint32_t* retrans, uint32_t* ooo);

// ---- rows behind handles (docs/specs/S09-rows-as-handles.md) ----
// A row a Synced event names crosses to Bend as its id, a handle (a U32) and
// its like count; its body, created_at and author stay here, in the batch's
// PGresult (--db pg) or the golden rows (--db canned), until pg_release in
// the next Epoll.turn, after that turn ran this round's commands
// (host/epoll.c, ep_cmds): no Send is left to name a row, and no Policy State
// can hold one. The
// host's obligation: the handle names the row whose id Bend holds beside it.
typedef struct {
  uint32_t    req;       // the request whose statement's result holds the row
  uint32_t    id;        // the post's id, the word the event put beside the handle
  uint32_t    likes;     // its like count (--rows block renders it from here)
  uint32_t    bn, an, tn;
  const char* body;      // bn bytes, not NUL-terminated
  const char* author;    // an bytes
  char        ts[40];    // created_at as SPEC.md:35 writes it, tn bytes
} HostRow;
// The row behind handle h for request req and post id; NULL with *why set
// when h is not one of this round's handles, is another request's, or names
// another post.
const HostRow* pg_row(uint32_t h, uint32_t req, uint32_t id, const char** why);
const HostRow* pg_row_at(uint32_t h);  // the same without the request (the trace)
// A run (--rows block, S33): the n rows behind handles h .. h + n - 1, which must be the whole
// of request req's answer: n at least 1, each a row of this round issued for req, and no row
// of req on either side of the run. Returns the first (a round's rows are neighbours in the
// table, in the order the database gave them), each row's id and like count its own; NULL with
// *why and *at, the handle the check failed on, set.
const HostRow* pg_run(uint32_t h, uint32_t n, uint32_t req, const char** why, uint32_t* at);
void           pg_release(void);

#endif
