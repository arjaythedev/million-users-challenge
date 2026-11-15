# Route A′'s host (docs/specs/S08-pg-effect-and-interpreter.md,
# docs/specs/S10-two-batches.md, docs/specs/S24-the-pure-round.md): the
# interpreter of the command policy. One Bend computation owns the loop;
# host/epoll.c owns the sockets and host/pg.c the database connections. Each
# round asks the reactor for the round's events (Epoll.turn), reads them into
# Policy.HostEvents (host/wire.bend) and folds Policy.round over them, which
# answers the next state and the round's commands in step order. The next
# Epoll.turn takes those commands and runs them, in order, before it waits:
# Send writes the reply of its Reply shape, Queue appends to an open batch,
# Open names the next open batch. The last round's commands go to
# Epoll.close. PROOF.bend proves that round is Policy.run (Round.is_run) and
# that rounds one after another give the commands of one run over all their
# events (Round.append), so the laws about run speak about these commands.
#
#   bend-epoll [--port N] [--extra-port N] [--db pg|canned] [--pool N]
#              [--batch-max N] [--batch-wait US] [--db-timeout MS] [--max N]
#              [--max-head BYTES] [--max-body BYTES] [--idle-timeout SECONDS]
#              [--trace FILE] [--stats FILE] [--worker K --workers N]
#              [--golden DIR] [--fuel N]
#
# The flags are read by the C side (bench/servers/bend-epoll/README.md).
# One computation per process: nothing here calls IO.spawn or IO.fork.
# Several processes share the ports (--workers, submission/start.sh); each
# runs this loop over its own connections, requests and batches.
# This file imports foreign effects, so no proof may import it.
import Base
import ../src/request.bend as Req
import ../src/respond.bend as Rsp
import ../src/policy.bend as Pol
import ./epoll.bend as E
import ./pg.bend as P
import ./wire.bend as W

def bit(b: Bool) -> U32:
  match b:
    case True{}:
      1
    case False{}:
      0

# ---- the commands ----

# A reply's rows cross to C as a list of Row cells, in order, End after the
# last. With --rows each, and for a single post, a cell is one post: its id,
# like count and handle; host/epoll.c renders the handle's strings and refuses
# (exit 70) a handle that is not one of this round's, is not req's, or names
# another post than id. With --rows block a feed's answer is one cell, in which
# id is the count of its rows and h the first one's handle: host/epoll.c
# renders that run from its own table and refuses a run that is not the whole
# of req's answer. No code here differs between the two.
def row(p: Rsp.PostRow, rest: E.Wire) -> E.Wire:
  match p:
    case Rsp.PostRow{id, likes, h}:
      E.Row{id, likes, h, rest}

def pack_rows(ps: List<&2, Rsp.PostRow>) -> E.Wire:
  match ps:
    case Nil{}:
      E.End{}
    case Con{p, rest}:
      row(p, pack_rows(rest))

def send(req: U32, conn: U32, gen: U32, r: Rsp.Reply) -> E.Cmd:
  match r:
    case Rsp.Error{status, msg}:
      E.Http.error(req, conn, gen, status, msg)
    case Rsp.Health{u}:
      E.Http.health(req, conn, gen, u)
    case Rsp.Degraded{}:
      E.Http.degraded(req, conn, gen)
    case Rsp.Feed{ps}:
      E.Http.posts(req, conn, gen, 0, pack_rows(ps))
    case Rsp.One{p}:
      E.Http.posts(req, conn, gen, 1, row(p, E.End{}))
    case Rsp.Created{id, text, ts, name, likes}:
      E.Http.created(req, conn, gen, id, text, ts, name, likes)
    case Rsp.LikedReply{status, already, pid}:
      E.Http.liked(req, conn, gen, status, bit(already), pid)

def queue(b: U32, req: U32, op: Req.Op) -> E.Cmd:
  match op:
    case Req.SelectOne{}:
      P.Pg.queue(b, req, 0, 0, 0, "", "")
    case Req.FeedQuery{}:
      P.Pg.queue(b, req, 1, 0, 0, "", "")
    case Req.PostQuery{pid}:
      P.Pg.queue(b, req, 2, 0, pid, "", "")
    case Req.InsertPost{uid, name, text}:
      P.Pg.queue(b, req, 3, uid, 0, name, text)
    case Req.InsertLike{uid, pid}:
      P.Pg.queue(b, req, 4, uid, pid, "", "")

def kind(k: Pol.BatchKind) -> U32:
  match k:
    case Pol.Read{}:
      0
    case Pol.Write{}:
      1

def cmd(c: Pol.Command) -> E.Cmd:
  match c:
    case Pol.Send{req, conn, gen, reply}:
      send(req, conn, gen, reply)
    case Pol.Queue{b, req, op}:
      queue(b, req, op)
    case Pol.Open{b, k}:
      P.Pg.open_batch(b, kind(k))

# The round's commands as Epoll.turn takes them, in the round's order.
def cmds(cs: List<&2, Pol.Command>) -> List<E.Cmd>:
  match cs:
    case Nil{}:
      Nil{}
    case Con{c, rest}:
      cmd(c) <> cmds(rest)

# ---- the events ----

# A Wire that is no event (host/wire.bend) is dropped.
def put(m: Maybe<Pol.HostEvent>, es: List<&2, Pol.HostEvent>) -> List<&2, Pol.HostEvent>:
  match m:
    case None{}:
      es
    case Some{ev}:
      ev <> es

# One event in front of the round's later ones. host/wire.bend reads a Synced
# whose ok word is neither 0 nor 1 as ok false and says so in the Bool, which
# is counted here.
def seen(r: Maybe<Pol.HostEvent> & Bool, x: List<&2, Pol.HostEvent> & U32) -> List<&2, Pol.HostEvent> & U32:
  (m, bad) = r
  (es, odd) = x
  (put(m, es), (odd + bit(bad) : U32))

# The round's events in the turn's order, and how many Synced events had an
# ok word that is neither 0 nor 1.
def events(ws: List<E.Wire>) -> List<&2, Pol.HostEvent> & U32:
  match ws:
    case Nil{}:
      (Nil{}, 0)
    case Con{w, rest}:
      seen(W.event_seen(w), events(rest))

# ---- the loop ----

def odd(+n: U32) -> IO(Unit):
  match n:
    case 0:
      IO.pure(Unit, Unit{})
    case _:
      IO.print("bend-epoll: " ++ U32.show(n) ++ " Synced events of one round had an ok word that is neither 0 nor 1; their batches are answered as failed")

def done(r: Pol.State & List<&2, Pol.Command>, n: U32, h: Socket) -> IO(Socket & Pol.State & List<E.Cmd>):
  (s, cs) = r
  do IO<Socket & Pol.State & List<E.Cmd>>:
    odd(n)
    IO.pure(Socket & Pol.State & List<E.Cmd>, (h, s, cmds(cs)))

def round(h: Socket, x: List<&2, Pol.HostEvent> & U32, s: Pol.State) -> IO(Socket & Pol.State & List<E.Cmd>):
  (es, n) = x
  done(Pol.round(s, es), n, h)

def turned(x: Socket & List<E.Wire>, s: Pol.State) -> IO(Socket & Pol.State & List<E.Cmd>):
  (h, ws) = x
  round(h, events(ws), s)

# One round per unit of fuel; a round is one Epoll.turn call, which runs the
# commands of the round before. The first turn gets none; Epoll.close runs
# the last round's.
def serve(fuel: Nat, y: Socket & Pol.State & List<E.Cmd>) -> IO(Unit):
  match fuel:
    case 0n:
      (h, s, cs) = y
      do IO<Unit>:
        E.Epoll.close(h, cs)
        IO.print("bend-epoll: fuel spent, reactor closed")
    case 1n+p:
      (h, s, cs) = y
      do IO<Unit>:
        x : Socket & List<E.Wire> <- E.Epoll.turn(h, cs)
        z : Socket & Pol.State & List<E.Cmd> <- turned(x, s)
        serve(p, z)

def start(o: Socket & U32) -> IO(Unit):
  (h, fuel) = o
  do IO<Unit>:
    IO.try(Unit, P.Pg.open())
    serve(U32.to_nat(fuel), (h, Pol.state0(), Nil{}))

def main() -> IO(Unit):
  do IO<Unit>:
    o : Socket & U32 <- IO.try(Socket & U32, E.Epoll.open())
    start(o)
