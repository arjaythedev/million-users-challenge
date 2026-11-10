# The database side of route A′'s host
# (docs/specs/S08-pg-effect-and-interpreter.md, docs/specs/S10-two-batches.md,
# docs/specs/S24-the-pure-round.md). Its C side, host/pg.c, holds a pool of
# the host's wire client's connections (host/pgwire.c) in pipeline mode, one socket each in the reactor's epoll
# set, and the two open batches, one of reads and one of writes. The Queue
# and Open commands cross as E.Cmd in the list Epoll.turn takes
# (host/epoll.bend); host/epoll.c's walk hands them to host/pg.c. Batch
# results come back to Bend as Synced and Died events through Epoll.turn.
# host/pg.js is a stub: Pg.open runs on the native lane only. This file has
# foreign imports, so no proof may import it.
import Base
import ./epoll.bend as E

# Starts the pool with the flags Epoll.open read, so it runs after
# Epoll.open (it fails with EBADF before): --db pg connects --pool
# connections to DATABASE_URL and prepares the five statements of
# host/sql.h on each; --db canned loads the golden files of --golden and
# answers every batch itself (bench/servers/bend-epoll/README.md). Fails
# when no connection is ready within --db-timeout.
def Pg.open() -> IO(Result<&1, &1, U32 & String, Unit>):
  import "./pg.c"
  import "./pg.js"

# The Queue command: appends one statement for request req to the open
# batch of its kind, which must be batch. kind: 0 SelectOne, 1 FeedQuery,
# 2 PostQuery{pid} (the read batch), 3 InsertPost{uid, name, text},
# 4 InsertLike{uid, pid} (the write batch); the fields a kind does not use
# are 0 and "".
def Pg.queue(batch: U32, req: U32, kind: U32, uid: U32, pid: U32, name: String, text: String) -> E.Cmd:
  E.Queue{batch, req, kind, uid, pid, name, text}

# The Open command: batch is the open batch of kind (0 reads, 1 writes) from
# now on. That kind must have no open batch (its last one was flushed), and
# batch must be the id after the last one opened.
def Pg.open_batch(batch: U32, kind: U32) -> E.Cmd:
  E.Open{batch, kind}
