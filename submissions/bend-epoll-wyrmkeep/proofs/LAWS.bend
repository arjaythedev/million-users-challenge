# LAWS.bend: the laws of Bend Million. Human-owned. Status of every law is
# in docs/10-proof-coverage-matrix.md; the approved hash of this file is
# docs/laws.sha256 and scripts/verify.sh refuses any other.
#
# ALL 29 LAWS BELOW WERE APPROVED BY THE OWNER AS STATED: the first 25 on
# 2026-11-08 (ADR-0001 laws 1-5, ADR-0003 laws 1-13 in the handle form,
# ADR-0004 laws 1-7; docs/log/2026-11-08.md, 12:45; byte-identical to the
# approved scratch file, sha256 a394da91cc90c35b...), the last four on
# 2026-11-09 02:08 (ADR-0001 laws 6-8 and committed_write_succeeds;
# docs/log/2026-11-09.md). Agents never change a law block; S20 appended
# the four as stated, and the lead corrects only these comments after an
# approval, with the diff in the day's log and the new hash in
# docs/laws.sha256.
#
# Scope: the pure request classifier (src/request.bend). These laws are
# about Bend semantics only. They say nothing about the native host that
# lexes bytes into a Request, about HS256 verification, about PostgreSQL,
# or about the compiled binary (docs/09-trusted-computing-base.md).
import Base
import ../src/request.bend as Req
import ../src/respond.bend as Rsp
import ../src/policy.bend as Pol

# LAW auth_gate (SPEC.md:73-75, README.md rule 9): an unauthenticated
# request to a protected route is rejected with 401, whatever its id or
# body. "Unauthenticated requests cannot produce authenticated commands."
law auth_gate:
  for +route: Req.Route
  for +auth: Req.Auth
  for +body: Req.Body
  for p: {Req.is_protected(route) == True{} : Bool}
  for u: {Req.is_user(auth) == False{} : Bool}
  {Req.status(Req.classify(Req.Request{route, auth, body})) == 401 : U32}

# LAW auth_before_id (SPEC.md:81): on a protected route, a missing token
# wins over an invalid id: POST /posts/abc/like without a token is 401.
law auth_before_id:
  for +body: Req.Body
  {Req.status(Req.classify(Req.Request{Req.LikePost{Req.InvalidId{}}, Req.NoBearer{}, body})) == 401 : U32}

# LAW write_carries_user (section 5.2, "command parameters preserve the
# intended user identifier"): when the kernel decides to run a write, the
# user id in the database command is the authenticated user's id.
law write_carries_user:
  for +req: Req.Request
  for w: {Req.is_write(Req.classify(req)) == True{} : Bool}
  {Req.op_uid(Req.classify(req)) == Req.auth_uid(req) : Maybe<U32>}

# LAW no_write_on_reject (section 5.2, "rejected operations generate no
# unintended write commands"): a decision is a rejection or a run, never
# both; a rejected request carries no operation. Stated as: a request with
# an invalid body on POST /posts never runs a write.
law bad_body_no_write:
  for +auth: Req.Auth
  for +body: Req.Body
  for t: {Req.is_text(body) == False{} : Bool}
  {Req.is_write(Req.classify(Req.Request{Req.CreatePost{}, auth, body})) == False{} : Bool}

# LAW invalid_id_no_query (SPEC.md:71): an invalid id on GET /posts/:id is
# rejected with 400 and runs nothing. "Invalid input never enters an
# operation requiring stronger preconditions."
law invalid_id_no_query:
  for +auth: Req.Auth
  for +body: Req.Body
  {Req.status(Req.classify(Req.Request{Req.PostById{Req.InvalidId{}}, auth, body})) == 400 : U32}

# The response policy (S02; approved 2026-11-08 in the handle form of S09)
#
# Scope: the response policy (src/respond.bend), drafted in
# docs/specs/S02-response-policy.md, "Laws to propose", plus five laws the
# review of S02 asked for. The laws are about Bend semantics only. They
# hold under assumption A5: the host reports Committed and Liked only after
# the ReadyForQuery that commits the write (PostgreSQL also sends
# ReadyForQuery after a rollback, so the arrival of ReadyForQuery is not
# enough), reports Unknown when the connection failed between send and
# ReadyForQuery, and maps SQLSTATE 23503 on InsertLike to NotFound. They
# say nothing about whether the host does that, nor about how it renders a
# Reply as bytes.

# LAW created_needs_commit (README.md rule 7, SPEC.md:47): the reply to an
# InsertPost reports success only when the host reported Committed. Every
# other Outcome, including Liked, gives an error. This is the first half of
# write_ack; it is stated per operation so that the success constructor
# of one write can never authorise the other.
law created_needs_commit:
  for u: U32
  for uid: U32
  for name: String
  for text: String
  for +out: Rsp.Outcome
  for h: {Rsp.is_committed(out) == False{} : Bool}
  {Rsp.is_success(Rsp.respond(Req.InsertPost{uid, name, text}, out, u)) == False{} : Bool}

# LAW liked_needs_commit (README.md rule 7, SPEC.md:48): the reply to an
# InsertLike reports success only when the host reported Liked. The
# second half of write_ack.
law liked_needs_commit:
  for u: U32
  for uid: U32
  for pid: U32
  for +out: Rsp.Outcome
  for h: {Rsp.is_liked(out) == False{} : Bool}
  {Rsp.is_success(Rsp.respond(Req.InsertLike{uid, pid}, out, u)) == False{} : Bool}

# LAW no_success_on_unknown (SPEC.md:44, 79; docs/01 section 5.3): when the
# commit state is unknown no operation reports success. The two laws below
# state the exact status: 503 for a health check, 500 for every other op.
law no_success_on_unknown:
  for op: Req.Op
  for u: U32
  {Rsp.is_success(Rsp.respond(op, Rsp.Unknown{}, u)) == False{} : Bool}

# LAW health_unknown_is_503 (SPEC.md:44): a health check that is not
# answered by Ping, Unknown included, gives 503.
law health_unknown_is_503:
  for u: U32
  for +out: Rsp.Outcome
  for h: {Rsp.is_ping(out) == False{} : Bool}
  {Rsp.reply_status(Rsp.respond(Req.SelectOne{}, out, u)) == 503 : U32}

# LAW unknown_is_500 (SPEC.md:79): every operation but the health check
# answers 500 when the commit state is unknown.
law unknown_is_500:
  for +op: Req.Op
  for u: U32
  for h: {Rsp.is_select_one(op) == False{} : Bool}
  {Rsp.reply_status(Rsp.respond(op, Rsp.Unknown{}, u)) == 500 : U32}

# LAW not_found_is_404 (SPEC.md:61, 72): a post query and a like that name
# a post the database does not have answer 404. Both operations carry the
# same id here.
law not_found_is_404:
  for +id: U32
  for uid: U32
  for +u: U32
  {Rsp.reply_status(Rsp.respond(Req.PostQuery{id}, Rsp.NotFound{}, u)) == 404 : U32} & {Rsp.reply_status(Rsp.respond(Req.InsertLike{uid, id}, Rsp.NotFound{}, u)) == 404 : U32}

# LAW created_echoes_request (SPEC.md:47): the 201 body is the post that
# the request made: the author and the text of the InsertPost op, the id
# and the created_at the host reported, and like_count 0. (S09 moved
# like_count out of the reply into the C renderer; S10 puts it back, so
# this law fixes it again.)
law created_echoes_request:
  for u: U32
  for uid: U32
  for +name: String
  for +text: String
  for id: U32
  for ts: String
  {Rsp.respond(Req.InsertPost{uid, name, text}, Rsp.Committed{id, ts}, u) == Rsp.Created{id, text, ts, name, 0} : Rsp.Reply}

# LAW like_status (SPEC.md:48): a committed like answers 201 the first
# time (already is False) and 200 on a repeat (already is True), and the
# reply carries the post id of the op.
law like_status:
  for +uid: U32
  for +pid: U32
  for +u: U32
  {Rsp.respond(Req.InsertLike{uid, pid}, Rsp.Liked{False{}}, u) == Rsp.LikedReply{201, False{}, pid} : Rsp.Reply} & {Rsp.respond(Req.InsertLike{uid, pid}, Rsp.Liked{True{}}, u) == Rsp.LikedReply{200, True{}, pid} : Rsp.Reply}

# LAW created_only_for_insert_post (README.md rule 7, SPEC.md:47; the
# "and op is InsertPost" half of the write_ack row of the S02 spec): a
# Created reply is given exactly when the op is InsertPost and the host
# reported Committed. No other op can answer Created, whatever the host
# reports: a FeedQuery with Committed is not a created post.
law created_only_for_insert_post:
  for +op: Req.Op
  for +out: Rsp.Outcome
  for u: U32
  {Rsp.is_created(Rsp.respond(op, out, u)) == Bool.and(Rsp.is_insert_post(op), Rsp.is_committed(out)) : Bool}

# LAW liked_only_for_insert_like (README.md rule 7, SPEC.md:48): a
# LikedReply is given exactly when the op is InsertLike and the host
# reported Liked. No other op can answer LikedReply.
law liked_only_for_insert_like:
  for +op: Req.Op
  for +out: Rsp.Outcome
  for u: U32
  {Rsp.is_liked_reply(Rsp.respond(op, out, u)) == Bool.and(Rsp.is_insert_like(op), Rsp.is_liked(out)) : Bool}

# LAW health_ok_reports_uptime (SPEC.md:44): a health check that the
# database answered reports the uptime the host gave, and nothing else.
law health_ok_reports_uptime:
  for u: U32
  {Rsp.respond(Req.SelectOne{}, Rsp.Ping{}, u) == Rsp.Health{u} : Rsp.Reply}

# LAW feed_ok_passes_rows (SPEC.md:45): a feed query that found rows
# answers with exactly those rows. (S09: a row is its id, its like count
# and the host's handle for its body, created_at and author; which bytes
# a handle names is the host's, outside this law.)
law feed_ok_passes_rows:
  for u: U32
  for ps: List<&2, Rsp.PostRow>
  {Rsp.respond(Req.FeedQuery{}, Rsp.Posts{ps}, u) == Rsp.Feed{ps} : Rsp.Reply}

# LAW post_ok_passes_row (SPEC.md:46): a post query that found its row
# answers with exactly that row (S09: id, like count and handle, as above).
law post_ok_passes_row:
  for pid: U32
  for u: U32
  for p: Rsp.PostRow
  {Rsp.respond(Req.PostQuery{pid}, Rsp.Post{p}, u) == Rsp.One{p} : Rsp.Reply}

# The command policy (S07, restated over two open batches in S10, law 7
# added in S10; approved 2026-11-08)
#
# Scope: the command policy (src/policy.bend, imported as Pol), drafted in
# docs/specs/S07-command-policy.md, "Laws". step(state, event) is the
# server's whole decision; run folds it over a list of events from
# state0(). The laws below are about that fold. They hold under assumption
# A6 (docs/09-trusted-computing-base.md): the host reports Synced only on
# the ReadyForQuery that answers the batch's Sync, with ok False{} when any
# statement failed and outs the statements' outcomes; Died when the
# database connection died first; unique request ids; a fresh generation
# per accepted connection; and it executes Send only to the generation it
# names. They say nothing about whether the host does that.
#
# The spec's Done and Lost are Synced and Died here, and its Event is
# HostEvent: Base already defines those three names.
#
# S10 (docs/specs/S10-two-batches.md) changes the State these laws run
# over: two open batches (reads and writes), each holding its own entries,
# and a list of flushed batches, instead of one open batch id and one list
# of entries tagged with their batch. Open now names the kind of the batch
# it opens. The six statements are the S07 ones; only the Open patterns of
# the helpers below gained the kind.
#
# The functions below give the laws their meaning, so they sit in this
# file and its hash fixes them. Each search over a list is its own
# recursive function: a List.any template may not capture a variable.

# A saturating count: no, one, or more than one.
type Count is Data:
  Never{}
  Once{}
  Repeated{}

def count.add(a: Count, b: Count) -> Count:
  match a:
    case Never{}:
      b
    case Once{}:
      match b:
        case Never{}:
          Once{}
        case Once{}:
          Repeated{}
        case Repeated{}:
          Repeated{}
    case Repeated{}:
      Repeated{}

def count.of(hit: Bool) -> Count:
  match hit:
    case True{}:
      Once{}
    case False{}:
      Never{}

def at_most_once(c: Count) -> Bool:
  match c:
    case Never{}:
      True{}
    case Once{}:
      True{}
    case Repeated{}:
      False{}

def implies(a: Bool, b: Bool) -> Bool:
  match a:
    case True{}:
      b
    case False{}:
      True{}

# The outcome o is the commit of the write op: Committed for InsertPost,
# Liked for InsertLike. A read has no commit.
def commits(op: Req.Op, o: Rsp.Outcome) -> Bool:
  match op:
    case Req.SelectOne{}:
      False{}
    case Req.FeedQuery{}:
      False{}
    case Req.PostQuery{pid}:
      False{}
    case Req.InsertPost{uid, name, text}:
      Rsp.is_committed(o)
    case Req.InsertLike{uid, pid}:
      Rsp.is_liked(o)

# outs holds a pair (req, o) with o the commit of op.
def reports_commit(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op) -> Bool:
  match outs:
    case Nil{}:
      False{}
    case (r, o) <> rest:
      Bool.or(Bool.and(U32.is_eq(r, req), commits(op, o)), reports_commit(rest, req, op))

# e is a Synced of batch b, not rolled back, that reports the commit of op
# for req.
def commits_at(+b: U32, +req: U32, +op: Req.Op, e: Pol.HostEvent) -> Bool:
  match e:
    case Pol.Arrived{r, conn, gen, rq}:
      False{}
    case Pol.Flushed{batch}:
      False{}
    case Pol.Synced{batch, ok, outs, u}:
      Bool.and(U32.is_eq(b, batch), Bool.and(ok, reports_commit(outs, req, op)))
    case Pol.Died{batch}:
      False{}
    case Pol.Gone{conn, gen}:
      False{}

def committed_in(+b: U32, +req: U32, +op: Req.Op, es: List<&2, Pol.HostEvent>) -> Bool:
  match es:
    case Nil{}:
      False{}
    case e <> rest:
      Bool.or(commits_at(b, req, op, e), committed_in(b, req, op, rest))

# c queued req in some batch b, and if its op is a write, es commits that
# write in b.
def backs(+req: U32, +es: List<&2, Pol.HostEvent>, c: Pol.Command) -> Bool:
  match c:
    case Pol.Send{r, conn, gen, reply}:
      False{}
    case Pol.Queue{b, r, +op}:
      Bool.and(U32.is_eq(r, req), implies(Req.is_write(Req.Run{op}), committed_in(b, req, op, es)))
    case Pol.Open{b, k}:
      False{}

def backed(+req: U32, +es: List<&2, Pol.HostEvent>, cs: List<&2, Pol.Command>) -> Bool:
  match cs:
    case Nil{}:
      False{}
    case c <> rest:
      Bool.or(backs(req, es, c), backed(req, es, rest))

# Every Send in xs that reports success is backed by a Queue in cs and,
# for a write, by its commit in es.
def success_backed(+es: List<&2, Pol.HostEvent>, +cs: List<&2, Pol.Command>, xs: List<&2, Pol.Command>) -> Bool:
  match xs:
    case Nil{}:
      True{}
    case x <> rest:
      match x:
        case Pol.Send{req, conn, gen, reply}:
          Bool.and(implies(Rsp.is_success(reply), backed(req, es, cs)), success_backed(es, cs, rest))
        case Pol.Queue{b, req, op}:
          success_backed(es, cs, rest)
        case Pol.Open{b, k}:
          success_backed(es, cs, rest)

# LAW success_after_commit (spec row 1; the round-1 reviewers' "no success
# reply before the matching commit"): in the commands of any trace, a Send
# that reports success answers a request that was queued in some batch b,
# and if the request is a write, the trace holds a Synced of b, not rolled
# back, whose outs report that write's commit for that request. "Earlier"
# follows from the trace being arbitrary: a Send made at event k is in the
# commands of the first k events, and the law for that prefix puts its
# Synced among those k events. Stronger than the spec's row in one way:
# a success reply to a read also needs its Queue.
law success_after_commit:
  for +es: List<&2, Pol.HostEvent>
  {success_backed(es, Pol.commands(Pol.run(Pol.state0(), es)), Pol.commands(Pol.run(Pol.state0(), es))) == True{} : Bool}

def reply_to(+u: U32, c: Pol.Command) -> Count:
  match c:
    case Pol.Send{req, conn, gen, reply}:
      count.of(U32.is_eq(req, u))
    case Pol.Queue{b, req, op}:
      Never{}
    case Pol.Open{b, k}:
      Never{}

def replies(+u: U32, cs: List<&2, Pol.Command>) -> Count:
  match cs:
    case Nil{}:
      Never{}
    case c <> rest:
      count.add(reply_to(u, c), replies(u, rest))

def arrival_of(+u: U32, e: Pol.HostEvent) -> Count:
  match e:
    case Pol.Arrived{req, conn, gen, r}:
      count.of(U32.is_eq(req, u))
    case Pol.Flushed{batch}:
      Never{}
    case Pol.Synced{batch, ok, outs, uptime_s}:
      Never{}
    case Pol.Died{batch}:
      Never{}
    case Pol.Gone{conn, gen}:
      Never{}

def arrivals(+u: U32, es: List<&2, Pol.HostEvent>) -> Count:
  match es:
    case Nil{}:
      Never{}
    case e <> rest:
      count.add(arrival_of(u, e), arrivals(u, rest))

# LAW one_reply_per_request (spec row 2): for a request id u that arrives
# at most once in the trace (A6: request ids are unique), the trace's
# commands hold at most one Send for u. A duplicate Synced or Died for a
# batch answers nothing twice.
law one_reply_per_request:
  for +es: List<&2, Pol.HostEvent>
  for +u: U32
  for h: {at_most_once(arrivals(u, es)) == True{} : Bool}
  {at_most_once(replies(u, Pol.commands(Pol.run(Pol.state0(), es)))) == True{} : Bool}

def arrival_matches(+req: U32, +conn: U32, +gen: U32, e: Pol.HostEvent) -> Bool:
  match e:
    case Pol.Arrived{r, c, g, rq}:
      Bool.and(U32.is_eq(r, req), Bool.and(U32.is_eq(c, conn), U32.is_eq(g, gen)))
    case Pol.Flushed{batch}:
      False{}
    case Pol.Synced{batch, ok, outs, uptime_s}:
      False{}
    case Pol.Died{batch}:
      False{}
    case Pol.Gone{c, g}:
      False{}

def arrived(+req: U32, +conn: U32, +gen: U32, es: List<&2, Pol.HostEvent>) -> Bool:
  match es:
    case Nil{}:
      False{}
    case e <> rest:
      Bool.or(arrival_matches(req, conn, gen, e), arrived(req, conn, gen, rest))

def sends_match_arrivals(+es: List<&2, Pol.HostEvent>, cs: List<&2, Pol.Command>) -> Bool:
  match cs:
    case Nil{}:
      True{}
    case c <> rest:
      match c:
        case Pol.Send{req, conn, gen, reply}:
          Bool.and(arrived(req, conn, gen, es), sends_match_arrivals(es, rest))
        case Pol.Queue{b, req, op}:
          sends_match_arrivals(es, rest)
        case Pol.Open{b, k}:
          sends_match_arrivals(es, rest)

# LAW reply_to_the_same_generation (spec row 3): every Send{req, conn, gen}
# in the trace's commands has an Arrived{req, conn, gen} in the trace. With
# unique request ids (A6) that Arrived is the one that introduced req, so a
# reply never goes to a later generation of the same connection id.
law reply_to_the_same_generation:
  for +es: List<&2, Pol.HostEvent>
  {sends_match_arrivals(es, Pol.commands(Pol.run(Pol.state0(), es))) == True{} : Bool}

def has_outcome(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32) -> Bool:
  match outs:
    case Nil{}:
      False{}
    case (r, o) <> rest:
      Bool.or(U32.is_eq(r, req), has_outcome(rest, req))

# Every Send in cs for a request id that outs has no outcome for is not a
# success.
def missing_fails(+outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, cs: List<&2, Pol.Command>) -> Bool:
  match cs:
    case Nil{}:
      True{}
    case c <> rest:
      match c:
        case Pol.Send{req, conn, gen, reply}:
          Bool.and(implies(Bool.not(has_outcome(outs, req)), Bool.not(Rsp.is_success(reply))), missing_fails(outs, rest))
        case Pol.Queue{b, req, op}:
          missing_fails(outs, rest)
        case Pol.Open{b, k}:
          missing_fails(outs, rest)

# LAW unknown_is_not_success (spec row 4): from any state, a Synced whose
# outs carry no outcome for a request answers that request with a reply
# that is not a success, whether ok is True{} or False{}.
law unknown_is_not_success:
  for s: Pol.State
  for b: U32
  for ok: Bool
  for +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>
  for u: U32
  {missing_fails(outs, Pol.commands(Pol.step(s, Pol.Synced{b, ok, outs, u}))) == True{} : Bool}

def is_reject(d: Req.Decision) -> Bool:
  match d:
    case Req.Reject{status, msg}:
      True{}
    case Req.Run{op}:
      False{}

def send_count(cs: List<&2, Pol.Command>) -> Count:
  match cs:
    case Nil{}:
      Never{}
    case c <> rest:
      match c:
        case Pol.Send{req, conn, gen, reply}:
          count.add(Once{}, send_count(rest))
        case Pol.Queue{b, req, op}:
          send_count(rest)
        case Pol.Open{b, k}:
          send_count(rest)

def queue_count(cs: List<&2, Pol.Command>) -> Count:
  match cs:
    case Nil{}:
      Never{}
    case c <> rest:
      match c:
        case Pol.Send{req, conn, gen, reply}:
          queue_count(rest)
        case Pol.Queue{b, req, op}:
          count.add(Once{}, queue_count(rest))
        case Pol.Open{b, k}:
          queue_count(rest)

# LAW reject_commands_nothing (spec row 5): from any state, an Arrived
# whose request classify rejects gives exactly one Send and no Queue.
law reject_commands_nothing:
  for +s: Pol.State
  for +req: U32
  for +conn: U32
  for +gen: U32
  for +r: Req.Request
  for h: {is_reject(Req.classify(r)) == True{} : Bool}
  {send_count(Pol.commands(Pol.step(s, Pol.Arrived{req, conn, gen, r}))) == Once{} : Count} & {queue_count(Pol.commands(Pol.step(s, Pol.Arrived{req, conn, gen, r}))) == Never{} : Count}

# queues(b, u, c): c is a Queue of request u into batch b.
def queues(b: U32, u: U32, c: Pol.Command) -> Bool:
  match c:
    case Pol.Send{req, conn, gen, reply}:
      False{}
    case Pol.Queue{batch, req, op}:
      Bool.and(U32.is_eq(batch, b), U32.is_eq(req, u))
    case Pol.Open{batch, kind}:
      False{}

def queued_in(+b: U32, +u: U32, cs: List<&2, Pol.Command>) -> Bool:
  match cs:
    case Nil{}:
      False{}
    case c <> rest:
      Bool.or(queues(b, u, c), queued_in(b, u, rest))

# No Send to request u in cs is a success.
def no_success_to(+u: U32, cs: List<&2, Pol.Command>) -> Bool:
  match cs:
    case Nil{}:
      True{}
    case c <> rest:
      match c:
        case Pol.Send{req, conn, gen, reply}:
          Bool.and(implies(U32.is_eq(req, u), Bool.not(Rsp.is_success(reply))), no_success_to(u, rest))
        case Pol.Queue{b, req, op}:
          no_success_to(u, rest)
        case Pol.Open{b, k}:
          no_success_to(u, rest)

# The state the kernel reaches after the events es.
def final(r: Pol.State & List<&2, Pol.Command>) -> Pol.State:
  (s, cs) = r
  s

def state_after(es: List<&2, Pol.HostEvent>) -> Pol.State:
  final(Pol.run(Pol.state0(), es))

# LAW failed_batch_no_success (spec row 6): split a trace into pre, a
# failure of batch b, and post. The failure is Synced{b, False{}, outs, up}
# (the batch rolled back, whatever outs carries) or Died{b}. If the
# commands of pre queue request u in b, then no command from the failure
# on (the run from the state pre reaches, over the failure and post) is a
# successful Send to u. Hypothesis (A6: request ids are unique): u arrives
# at most once in pre and post; the failure is not an arrival. The law says
# nothing about a request queued into b after the failure. Since S10 a
# Synced or Died of a batch still open first closes it, so such a request
# needs a later batch with the same id (batch ids are U32 and wrap, A6.11);
# A6 has no Died{b} or Synced{b} before Flushed{b} and none after Died{b},
# so a host that keeps A6 sends no such trace.
law failed_batch_no_success:
  for +pre: List<&2, Pol.HostEvent>
  for +post: List<&2, Pol.HostEvent>
  for +b: U32
  for +u: U32
  for +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>
  for +up: U32
  for h: {at_most_once(arrivals(u, List.append(&2, Pol.HostEvent, pre, post))) == True{} : Bool}
  for q: {queued_in(b, u, Pol.commands(Pol.run(Pol.state0(), pre))) == True{} : Bool}
  {no_success_to(u, Pol.commands(Pol.run(state_after(pre), Pol.Synced{b, False{}, outs, up} <> post))) == True{} : Bool} & {no_success_to(u, Pol.commands(Pol.run(state_after(pre), Pol.Died{b} <> post))) == True{} : Bool}

# Every Send in cs goes to a connection other than c, or to a generation
# other than g.
def no_send_to(+c: U32, +g: U32, cs: List<&2, Pol.Command>) -> Bool:
  match cs:
    case Nil{}:
      True{}
    case x <> rest:
      match x:
        case Pol.Send{req, conn, gen, reply}:
          Bool.and(Bool.not(Bool.and(U32.is_eq(conn, c), U32.is_eq(gen, g))), no_send_to(c, g, rest))
        case Pol.Queue{b, req, op}:
          no_send_to(c, g, rest)
        case Pol.Open{b, k}:
          no_send_to(c, g, rest)

# es holds an Arrived on connection c at generation g.
def arrives_on(+c: U32, +g: U32, es: List<&2, Pol.HostEvent>) -> Bool:
  match es:
    case Nil{}:
      False{}
    case e <> rest:
      match e:
        case Pol.Arrived{req, conn, gen, r}:
          Bool.or(Bool.and(U32.is_eq(conn, c), U32.is_eq(gen, g)), arrives_on(c, g, rest))
        case Pol.Flushed{batch}:
          arrives_on(c, g, rest)
        case Pol.Synced{batch, ok, outs, uptime_s}:
          arrives_on(c, g, rest)
        case Pol.Died{batch}:
          arrives_on(c, g, rest)
        case Pol.Gone{conn, gen}:
          arrives_on(c, g, rest)

# LAW gone_no_reply (S10): split a trace into pre, Gone{c, g} and post.
# No command from the Gone on (the run from the state pre reaches, over the
# Gone and post) sends to connection c at generation g: a request whose
# connection closed is dropped, never answered on that generation.
# Hypothesis (A6.5: a generation is never reused): post holds no Arrived
# on c at g. Without it the law is false: Gone{c, g} then an Arrived on
# (c, g) that classify rejects is answered at once
# (Witness.gone_no_reply.unconditional in PROOF.bend computes it).
law gone_no_reply:
  for +pre: List<&2, Pol.HostEvent>
  for +post: List<&2, Pol.HostEvent>
  for +c: U32
  for +g: U32
  for h: {arrives_on(c, g, post) == False{} : Bool}
  {no_send_to(c, g, Pol.commands(Pol.run(state_after(pre), Pol.Gone{c, g} <> post))) == True{} : Bool}

# The exact classifier (ADR-0001 laws 6-8; approved 2026-11-09 02:08)
#
# Scope: classify (src/request.bend), as for the first five laws above.
# Each of these says the database operation a request runs is exactly
# the one its route, user and body name.

# LAW post_exact (SPEC.md:47, 59): an authenticated POST /posts with a valid body runs
# exactly InsertPost with the user's id, the user's name and the trimmed text.
law post_exact:
  for uid: U32
  for +name: String
  for +s: String
  {Req.classify(Req.Request{Req.CreatePost{}, Req.User{uid, name}, Req.Trimmed{s}}) == Req.Run{Req.InsertPost{uid, name, s}} : Req.Decision}

# LAW like_exact (SPEC.md:48, 60): an authenticated POST /posts/:id/like with a valid id
# runs exactly InsertLike with the user's id and that post id, whatever the body.
law like_exact:
  for uid: U32
  for name: String
  for id: U32
  for body: Req.Body
  {Req.classify(Req.Request{Req.LikePost{Req.ValidId{id}}, Req.User{uid, name}, body}) == Req.Run{Req.InsertLike{uid, id}} : Req.Decision}

# LAW post_query_exact (SPEC.md:46): GET /posts/:id with a valid id runs exactly
# PostQuery for that id, whatever the auth and body.
law post_query_exact:
  for id: U32
  for auth: Req.Auth
  for body: Req.Body
  {Req.classify(Req.Request{Req.PostById{Req.ValidId{id}}, auth, body}) == Req.Run{Req.PostQuery{id}} : Req.Decision}

# The converse of success_after_commit (S12; approved 2026-11-09 02:08)
#
# Scope and assumptions: those of the command policy above (A6).

# cs holds a successful Send to request req on connection conn at
# generation gen.
def success_sent(+req: U32, +conn: U32, +gen: U32, cs: List<&2, Pol.Command>) -> Bool:
  match cs:
    case Nil{}:
      False{}
    case c <> rest:
      match c:
        case Pol.Send{r, cn, g, reply}:
          Bool.or(Bool.and(Bool.and(U32.is_eq(r, req), U32.is_eq(cn, conn)), Bool.and(U32.is_eq(g, gen), Rsp.is_success(reply))), success_sent(req, conn, gen, rest))
        case Pol.Queue{b, r, op}:
          success_sent(req, conn, gen, rest)
        case Pol.Open{b, k}:
          success_sent(req, conn, gen, rest)

# outs holds exactly one pair with id req, and that pair reports the commit
# of op: the first pair with id req commits op and no pair after it has
# the id req.
def only_commit(outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, +req: U32, +op: Req.Op) -> Bool:
  match outs:
    case Nil{}:
      False{}
    case (+r, o) <> +rest:
      Bool.or(Bool.and(U32.is_eq(r, req), Bool.and(commits(op, o), Bool.not(has_outcome(rest, req)))), Bool.and(Bool.not(U32.is_eq(r, req)), only_commit(rest, req, op)))

# Every entry of es whose request's only pair in outs commits its op has a
# successful Send in cs, on its connection and generation.
def commits_answered(+outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, es: List<&2, Pol.Pending>, +cs: List<&2, Pol.Command>) -> Bool:
  match es:
    case Nil{}:
      True{}
    case p <> rest:
      match p:
        case Pol.Pending{+req, conn, gen, op}:
          Bool.and(implies(only_commit(outs, req, op), success_sent(req, conn, gen, cs)), commits_answered(outs, rest, cs))

# The same for the entries of bt, if bt is a batch with id b.
def batch_answered(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, bt: Pol.Batch, +cs: List<&2, Pol.Command>) -> Bool:
  match bt:
    case Pol.Batch{id, kind, entries}:
      implies(U32.is_eq(id, b), commits_answered(outs, entries, cs))

def batches_answered(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, bs: List<&2, Pol.Batch>, +cs: List<&2, Pol.Command>) -> Bool:
  match bs:
    case Nil{}:
      True{}
    case bt <> rest:
      Bool.and(batch_answered(b, outs, bt, cs), batches_answered(b, outs, rest, cs))

# The same for every batch of s with id b: the open read batch, the open
# write batch and the batches in flight.
def commits_succeed(+b: U32, +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>, s: Pol.State, +cs: List<&2, Pol.Command>) -> Bool:
  match s:
    case Pol.State{next, reads, writes, inflight}:
      batches_answered(b, outs, reads <> writes <> inflight, cs)

# LAW committed_write_succeeds (S12; the converse of
# success_after_commit): from any state, a Synced{b, True{}, outs, u}
# answers each queued request of a batch with id b whose only pair in outs
# commits its op (commits: Committed{..} for a create, Liked{..} for a
# like) with a success, on that request's connection and generation.
# success_after_commit says a success needs a reported commit; this law
# says a reported commit gets its success. Without it, a policy that
# answers every write 500 keeps the other 28 laws. "The only pair": with
# two pairs for one request id a reply may follow either; under A6
# (request ids are unique) a host sends one pair per request.
# One step from any state, as unknown_is_not_success: a trace form needs
# A5 (one Synced or Died per batch), as a second Synced answers nobody.
law committed_write_succeeds:
  for +s: Pol.State
  for +b: U32
  for +outs: List<&2, +Sigma<U32, _ => Rsp.Outcome>>
  for u: U32
  {commits_succeed(b, outs, s, Pol.commands(Pol.step(s, Pol.Synced{b, True{}, outs, u}))) == True{} : Bool}
