// The late map's arithmetic (--late-map, docs/specs/S48-the-host-says-where-a-late-reply-waited.md):
// which cell of the table one reply belongs to. No I/O and no state. host/epoll.c takes the
// instants and keeps the counts, host/pg.c reads a database socket's two counters, and
// tests/host/latemap_test.mjs compiles this file alone for the exact edges. Outside every proof.
#ifndef BM_LATEMAP_H
#define BM_LATEMAP_H

#include <stdbool.h>
#include <stdint.h>

// The bands of a reply's total time, from the kernel's receive time of its request's last
// segment to the return of its reply's write. Band k is lm_edge_ns[k] or more and under
// lm_edge_ns[k + 1]; the last band has no upper edge. A total under the first edge is not late.
enum { LM_BANDS = 4 };
static const uint64_t lm_edge_ns[LM_BANDS] = { 25000000ull, 190000000ull, 400000000ull, 1000000000ull };

// A round trip can be the largest of a late reply's four waits only when it is a quarter of
// the first edge or more: under that, host/pg.c does not read the socket a second time.
#define LM_REPAIR_NS (25000000ull / 4)

// The columns, in the order the stop line and the stats file print them: the wait that held
// most of a late reply's time. The database's round trip has three, by what its socket's TCP
// saw between the batch's send and its answer (lm_repair).
enum { LM_BEFORE_WORKER, LM_BEFORE_DB, LM_DB_RESENT, LM_DB_OOO, LM_DB_NEITHER, LM_AFTER_DB, LM_COLS };

// One reply's five instants, ns on one clock: the kernel received the request's last segment,
// the host read it, the request's batch was written to a database socket, that batch's
// ReadyForQuery was read, the reply's write returned. A request that needs no database has
// out equal to in.
typedef struct {
  uint64_t arrive, read, out, in, write;
} LmTimes;

// The band of a total time, -1 when it is not late.
static inline int lm_band(uint64_t total_ns) {
  int b = -1;
  while (b + 1 < LM_BANDS && total_ns >= lm_edge_ns[b + 1]) {
    b += 1;
  }
  return b;
}

// A database socket's tcpi_total_retrans and tcpi_rcv_ooopack at a batch's send (0) and at its
// answer (1): the host's own segments were sent again, else the database's segments arrived out
// of order, else neither. Both counters only grow.
static inline uint8_t lm_repair(uint32_t retrans0, uint32_t ooo0, uint32_t retrans1, uint32_t ooo1) {
  return retrans1 != retrans0 ? LM_DB_RESENT : ooo1 != ooo0 ? LM_DB_OOO : LM_DB_NEITHER;
}

// The cell of one reply: *band and *col, and true; false when the reply is not late. The
// instants are first put in order, each no earlier than the one before (two clocks and a round's
// resolution can leave one a little early), so the four waits are never negative and add up to
// the total. The column is the largest wait's, the earliest of equal ones; repair is the round
// trip's column (lm_repair).
static inline bool lm_place(const LmTimes* t, uint8_t repair, int* band, int* col) {
  uint64_t at[5] = { t->arrive, t->read, t->out, t->in, t->write };
  for (int i = 1; i < 5; i += 1) {
    at[i] = at[i] < at[i - 1] ? at[i - 1] : at[i];
  }
  int b = lm_band(at[4] - at[0]);
  if (b < 0) {
    return false;
  }
  int k = 0;
  for (int i = 1; i < 4; i += 1) {
    if (at[i + 1] - at[i] > at[k + 1] - at[k]) {
      k = i;
    }
  }
  *band = b;
  *col  = k == 0 ? LM_BEFORE_WORKER : k == 1 ? LM_BEFORE_DB : k == 2 ? repair : LM_AFTER_DB;
  return true;
}

#endif
