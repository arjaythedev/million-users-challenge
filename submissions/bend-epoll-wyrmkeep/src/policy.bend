# The command policy: the server's decisions as one pure transition.
# step(state, event) returns the next state and the commands the host must
# execute; run folds step over a list of events. classify
# (src/request.bend) and respond (src/respond.bend) are called from here,
# so the laws about a whole trace of events (no success reply before its
# commit, one reply per request, a reply goes to the generation that
# asked) are laws about this file. The host only interprets the commands;
# what it must do for the laws to mean anything is assumption A6 in
# docs/09-trusted-computing-base.md. Shapes and behaviour follow
# docs/specs/S07-command-policy.md and docs/specs/S10-two-batches.md, with
# three names changed because Base already defines them: the spec's Event
# is HostEvent, its Done is Synced and its Lost is Died.
import Base
import ./request.bend as Req
import ./respond.bend as Rsp

# What the host reports. req is unique in a trace; gen is the connection's
# generation, bumped on every accept. Synced is reported on the
# ReadyForQuery that answers the Sync closing batch: ok is False{} when a
# statement of the batch failed (PostgreSQL sends ReadyForQuery after a
# rollback too), outs pairs each request id with its Outcome, and uptime_s
# is the uptime that a health reply reports. Died is reported when the
# database connection carrying batch died before that ReadyForQuery.
type HostEvent is Data:
  Arrived{req: U32, conn: U32, gen: U32, r: Req.Request}
  Flushed{batch: U32}
  Synced{batch: U32, ok: Bool, outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, uptime_s: U32}
  Died{batch: U32}
  Gone{conn: U32, gen: U32}

# A batch's kind: reads (SelectOne, FeedQuery, PostQuery) and writes
# (InsertPost, InsertLike) go to separate batches, so a read never waits
# for a write's commit.
type BatchKind is Data:
  Read{}
  Write{}

# What the host does. Send writes reply to conn only if conn's generation
# is still gen. Queue appends op to batch. Open says batch is now the open
# batch of its kind.
type Command is Data:
  Send{req: U32, conn: U32, gen: U32, reply: Rsp.Reply}
  Queue{batch: U32, req: U32, op: Req.Op}
  Open{batch: U32, kind: BatchKind}

# A queued request waiting for its batch's Synced or Died.
type Pending is Data:
  Pending{req: U32, conn: U32, gen: U32, op: Req.Op}

# A batch and the requests queued in it, newest first.
type Batch is Data:
  Batch{id: U32, kind: BatchKind, entries: List<&2, Pending>}

# reads and writes are the two open batches; inflight holds the flushed
# batches not yet answered, the last flushed first; next is the id the
# next batch to open gets. The spec's replied list is not kept: no decision
# reads it, and it would grow by one id per answered request.
type State is Data:
  State{next: U32, reads: Batch, writes: Batch, inflight: List<&2, Batch>}

def state0() -> State:
  State{2, Batch{0, Read{}, Nil{}}, Batch{1, Write{}, Nil{}}, Nil{}}

def commands(r: State & List<&2, Command>) -> List<&2, Command>:
  (s, cs) = r
  cs

# Every decision on a computed Bool, Decision or BatchKind goes through a
# helper that takes it as its first parameter (find.pick, answer_in.put,
# keep.put, arrived.put, ...), because a proof cannot match on a computed
# value, only on a parameter.

def kind_of(op: Req.Op) -> BatchKind:
  match op:
    case Req.SelectOne{}:
      Read{}
    case Req.FeedQuery{}:
      Read{}
    case Req.PostQuery{pid}:
      Read{}
    case Req.InsertPost{uid, name, text}:
      Write{}
    case Req.InsertLike{uid, pid}:
      Write{}

def id_of(bt: Batch) -> U32:
  match bt:
    case Batch{id, kind, entries}:
      id

# The outcome outs reports for req: the first pair with that id, Unknown{}
# if there is none.
def find.pick(hit: Bool, o: Rsp.Outcome, rest: Rsp.Outcome) -> Rsp.Outcome:
  match hit:
    case True{}:
      o
    case False{}:
      rest

def find(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32) -> Rsp.Outcome:
  match outs:
    case Nil{}:
      Rsp.Unknown{}
    case (r, o) <> rest:
      find.pick(U32.is_eq(r, req), o, find(rest, req))

# The outs left after the first pair with id req, Nil{} if there is none.
def skip.pick(hit: Bool, rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, later: List<&2, +Sigma<U32, _ => Rsp.Outcome>>) -> List<&2, +Sigma<U32, _ => Rsp.Outcome>>:
  match hit:
    case True{}:
      rest
    case False{}:
      later

def skip(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32) -> List<&2, +Sigma<U32, _ => Rsp.Outcome>>:
  match outs:
    case Nil{}:
      Nil{}
    case (r, o) <> +rest:
      skip.pick(U32.is_eq(r, req), rest, skip(rest, req))

# Lockstep. answer walks the entries and outs together: left is what the
# entries before this one have left of all, the Synced's whole outs. A host
# that emits outs in the entries' order (newest first) puts the entry's
# pair at the head of left: that pair is the outcome, and the walk goes on
# with the rest of left. Otherwise the outcome is find over all, as when
# there was no lockstep, and the walk goes on after req's pair in left, so
# that an entry dropped by a Gone (its pair still in outs) costs one scan,
# not one per entry after it. Either way the outcome is a pair of all with
# the id req, or Unknown{} when there is none, whatever the order of outs.
def head.pick(hit: Bool, o: Rsp.Outcome, all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, req: U32) -> Rsp.Outcome:
  match hit:
    case True{}:
      o
    case False{}:
      find(all, req)

def head(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32) -> Rsp.Outcome:
  match left:
    case Nil{}:
      find(all, req)
    case (r, o) <> rest:
      head.pick(U32.is_eq(r, req), o, all, req)

def after.pick(hit: Bool, rest: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, req: U32) -> List<&2, +Sigma<U32, _ => Rsp.Outcome>>:
  match hit:
    case True{}:
      rest
    case False{}:
      skip(rest, req)

def after(left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32) -> List<&2, +Sigma<U32, _ => Rsp.Outcome>>:
  match left:
    case Nil{}:
      Nil{}
    case (r, o) <> rest:
      after.pick(U32.is_eq(r, req), rest, req)

# A batch that rolled back answers Unknown{}, whatever outs says.
def outcome(ok: Bool, all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, req: U32) -> Rsp.Outcome:
  match ok:
    case True{}:
      head(left, all, req)
    case False{}:
      Rsp.Unknown{}

# One reply per entry of es, in the entries' order, followed by rest. left
# comes after es: the termination check reads the arguments left to right.
def answer(+ok: Bool, +all: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, es: List<&2, Pending>, +left: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, rest: List<&2, Command>) -> List<&2, Command>:
  match es:
    case Nil{}:
      rest
    case p <> tail:
      match p:
        case Pending{+req, conn, gen, op}:
          Send{req, conn, gen, Rsp.respond(op, outcome(ok, all, left, req), u)} <> answer(ok, all, u, tail, after(left, req), rest)

# The replies of every batch of bs whose id is b, the batches in list order.
def answer_in.put(hit: Bool, bt: Batch, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, rest: List<&2, Command>) -> List<&2, Command>:
  match hit:
    case True{}:
      match bt:
        case Batch{id, kind, entries}:
          answer(ok, outs, u, entries, outs, rest)
    case False{}:
      rest

def answer_in(+b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, bs: List<&2, Batch>) -> List<&2, Command>:
  match bs:
    case Nil{}:
      Nil{}
    case +bt <> rest:
      answer_in.put(U32.is_eq(id_of(bt), b), bt, ok, outs, u, answer_in(b, ok, outs, u, rest))

# The batches of bs without those whose id is b.
def keep.put(hit: Bool, bt: Batch, rest: List<&2, Batch>) -> List<&2, Batch>:
  match hit:
    case True{}:
      rest
    case False{}:
      bt <> rest

def keep(+b: U32, bs: List<&2, Batch>) -> List<&2, Batch>:
  match bs:
    case Nil{}:
      Nil{}
    case +bt <> rest:
      keep.put(U32.is_eq(id_of(bt), b), bt, keep(b, rest))

# The entries without those of connection c at generation g.
def gone_from(p: Pending, c: U32, g: U32) -> Bool:
  match p:
    case Pending{req, conn, gen, op}:
      Bool.and(U32.is_eq(conn, c), U32.is_eq(gen, g))

def drop.put(hit: Bool, p: Pending, rest: List<&2, Pending>) -> List<&2, Pending>:
  match hit:
    case True{}:
      rest
    case False{}:
      p <> rest

def drop(+c: U32, +g: U32, es: List<&2, Pending>) -> List<&2, Pending>:
  match es:
    case Nil{}:
      Nil{}
    case +p <> rest:
      drop.put(gone_from(p, c, g), p, drop(c, g, rest))

def drop_batch(+c: U32, +g: U32, bt: Batch) -> Batch:
  match bt:
    case Batch{id, kind, entries}:
      Batch{id, kind, drop(c, g, entries)}

def drop_all(+c: U32, +g: U32, bs: List<&2, Batch>) -> List<&2, Batch>:
  match bs:
    case Nil{}:
      Nil{}
    case bt <> rest:
      drop_batch(c, g, bt) <> drop_all(c, g, rest)

# Arrived: a Reject is answered at once with its status and message; a
# Run{op} is queued in the open batch of op's kind and answered at that
# batch's Synced or Died.
def push(p: Pending, bt: Batch) -> Batch:
  match bt:
    case Batch{id, kind, entries}:
      Batch{id, kind, p <> entries}

def arrived.put(k: BatchKind, p: Pending, next: U32, reads: Batch, writes: Batch, inflight: List<&2, Batch>) -> State:
  match k:
    case Read{}:
      State{next, push(p, reads), writes, inflight}
    case Write{}:
      State{next, reads, push(p, writes), inflight}

def arrived.state(d: Req.Decision, next: U32, reads: Batch, writes: Batch, inflight: List<&2, Batch>, req: U32, conn: U32, gen: U32) -> State:
  match d:
    case Req.Reject{status, msg}:
      State{next, reads, writes, inflight}
    case Req.Run{+op}:
      arrived.put(kind_of(op), Pending{req, conn, gen, op}, next, reads, writes, inflight)

def open_of(k: BatchKind, reads: Batch, writes: Batch) -> U32:
  match k:
    case Read{}:
      id_of(reads)
    case Write{}:
      id_of(writes)

def arrived.commands(d: Req.Decision, reads: Batch, writes: Batch, req: U32, conn: U32, gen: U32) -> List<&2, Command>:
  match d:
    case Req.Reject{status, msg}:
      [Send{req, conn, gen, Rsp.Error{status, msg}}]
    case Req.Run{+op}:
      [Queue{open_of(kind_of(op), reads, writes), req, op}]

# Flushing: an open batch whose id is the event's moves to the head of
# inflight (park) and a fresh batch of its kind opens in its place with the
# id next (fresh, bump), announced by an Open (opened). Reads are looked at
# first, then writes.
def fresh(hit: Bool, n: U32, k: BatchKind, bt: Batch) -> Batch:
  match hit:
    case True{}:
      Batch{n, k, Nil{}}
    case False{}:
      bt

def park(hit: Bool, bt: Batch, bs: List<&2, Batch>) -> List<&2, Batch>:
  match hit:
    case True{}:
      bt <> bs
    case False{}:
      bs

def bump(hit: Bool, n: U32) -> U32:
  match hit:
    case True{}:
      (n + 1 : U32)
    case False{}:
      n

def opened(hit: Bool, n: U32, k: BatchKind, rest: List<&2, Command>) -> List<&2, Command>:
  match hit:
    case True{}:
      Open{n, k} <> rest
    case False{}:
      rest

def flush(+b: U32, +next: U32, +reads: Batch, +writes: Batch, inflight: List<&2, Batch>) -> State & List<&2, Command>:
  +hr = U32.is_eq(id_of(reads), b)
  +hw = U32.is_eq(id_of(writes), b)
  +n1 = bump(hr, next)
  (State{bump(hw, n1), fresh(hr, next, Read{}, reads), fresh(hw, n1, Write{}, writes), park(hw, writes, park(hr, reads, inflight))}, opened(hr, next, Read{}, opened(hw, n1, Write{}, Nil{})))

# Synced and Died: the batch b is flushed if it is still open, then every
# batch in flight with id b is answered (Unknown{} for all when ok is
# False{}) and removed.
def settle(+b: U32, +ok: Bool, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +u: U32, +next: U32, +reads: Batch, +writes: Batch, inflight: List<&2, Batch>) -> State & List<&2, Command>:
  +hr = U32.is_eq(id_of(reads), b)
  +hw = U32.is_eq(id_of(writes), b)
  +n1 = bump(hr, next)
  +bs = park(hw, writes, park(hr, reads, inflight))
  (State{bump(hw, n1), fresh(hr, next, Read{}, reads), fresh(hw, n1, Write{}, writes), keep(b, bs)}, opened(hr, next, Read{}, opened(hw, n1, Write{}, answer_in(b, ok, outs, u, bs))))

# Gone drops the connection's entries without a reply.
def step(s: State, e: HostEvent) -> State & List<&2, Command>:
  match s:
    case State{+next, +reads, +writes, +inflight}:
      match e:
        case Arrived{+req, +conn, +gen, r}:
          +d = Req.classify(r)
          (arrived.state(d, next, reads, writes, inflight, req, conn, gen), arrived.commands(d, reads, writes, req, conn, gen))
        case Flushed{b}:
          flush(b, next, reads, writes, inflight)
        case Synced{b, ok, outs, u}:
          settle(b, ok, outs, u, next, reads, writes, inflight)
        case Died{b}:
          settle(b, False{}, Nil{}, 0, next, reads, writes, inflight)
        case Gone{+c, +g}:
          (State{next, drop_batch(c, g, reads), drop_batch(c, g, writes), drop_all(c, g, inflight)}, Nil{})

# run.go takes the last step's result t whole and opens it in the next
# call: a match cannot open the result of step(s, e) directly. acc holds
# the commands of the steps before t.
def run.go(es: List<&2, HostEvent>, t: State & List<&2, Command>, acc: List<&2, Command>) -> State & List<&2, Command>:
  match es:
    case Nil{}:
      match t:
        case (s, c):
          (s, List.append(&2, Command, acc, c))
    case e <> rest:
      match t:
        case (s, c):
          run.go(rest, step(s, e), List.append(&2, Command, acc, c))

def run(s: State, es: List<&2, HostEvent>) -> State & List<&2, Command>:
  run.go(es, (s, Nil{}), Nil{})

# round is the fold the host runs once per Epoll.turn (host/main.bend). It
# answers what run answers (PROOF.bend, Round.is_run), so every law about
# run is a law about round, but it puts each step's commands in front of the
# commands of the steps after it: a round of n events copies each command
# once, where run.go copies all the commands so far at every event. round.go,
# like run.go, takes the last step's result t whole and opens it in the next
# call. Round.append: a round over es ++ fs is the round over es, then the
# round over fs from its state, the commands one after the other.
def round.join(c: List<&2, Command>, r: State & List<&2, Command>) -> State & List<&2, Command>:
  match r:
    case (s, d):
      (s, List.append(&2, Command, c, d))

def round.go(es: List<&2, HostEvent>, t: State & List<&2, Command>) -> State & List<&2, Command>:
  match es:
    case Nil{}:
      t
    case e <> rest:
      match t:
        case (s, c):
          round.join(c, round.go(rest, step(s, e)))

def round(s: State, es: List<&2, HostEvent>) -> State & List<&2, Command>:
  round.go(es, (s, Nil{}))
