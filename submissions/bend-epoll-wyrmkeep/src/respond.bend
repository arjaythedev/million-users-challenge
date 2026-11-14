# The response policy: the second pure transition of the application
# kernel. classify (src/request.bend) decided which database operation
# runs. The database host runs it and reports what happened as an Outcome
# (the boundary: see docs/09-trusted-computing-base.md, assumption A5).
# respond decides, with no effect, the Reply the host renders. Types and
# rows are fixed by docs/specs/S02-response-policy.md and its behaviour
# table; the status codes and messages come from
# vendor/million-users-challenge/SPEC.md (endpoints 42-48, queries 54-62,
# errors 64-81).
import Base
import ./request.bend as Req

# One post as the database returned it: its id and like count, and a
# handle the host gave for the row's three strings (body, created_at,
# author), which the host keeps and resolves only until the reply that
# carries the handle is written (docs/specs/S09-rows-as-handles.md). The
# kernel never inspects a row; it passes the row through so that the host
# renders it.
type PostRow is Data:
  PostRow{id: U32, like_count: U32, row: U32}

# What the database host reports after running one Op. The host reports
# Committed and Liked only when the ReadyForQuery that closes the batch
# that carried the write closes a batch in which no statement failed:
# PostgreSQL also sends ReadyForQuery after a rollback, so the arrival of
# ReadyForQuery alone is not a commit. It reports Unknown when the
# connection died with the commit state unknown (assumption A5). Posts
# holds at most 20 rows; the host guarantees that and no proof depends on
# it.
type Outcome is Data:
  Committed{post_id: U32, created_at: String}
  Liked{already: Bool}
  Post{post: PostRow}
  Posts{posts: List<&2, PostRow>}
  Ping{}
  NotFound{}
  Unknown{}
  Failed{}

# What the host renders: a status and a JSON body, one constructor per
# body shape. Error carries the spec's exact message.
type Reply is Data:
  Error{status: U32, msg: String}
  Health{uptime_s: U32}
  Degraded{}
  Feed{posts: List<&2, PostRow>}
  One{post: PostRow}
  Created{id: U32, text: String, ts: String, name: String, like_count: U32}
  LikedReply{status: U32, already: Bool, post_id: U32}

# The HTTP status of a reply. Degraded is the 503 health answer.
def reply_status(r: Reply) -> U32:
  match r:
    case Error{status, msg}:
      status
    case Health{uptime_s}:
      200
    case Degraded{}:
      503
    case Feed{posts}:
      200
    case One{post}:
      200
    case Created{id, text, ts, name, like_count}:
      201
    case LikedReply{status, already, post_id}:
      status

# A reply reports success when its status is below 400.
def is_success(r: Reply) -> Bool:
  U32.is_lt(reply_status(r), 400)

# The Outcome predicates the laws use as hypotheses. Each names the one
# constructor that authorises a success: a write succeeds on Committed or
# Liked, a health check on Ping, and only SelectOne treats Unknown as 503.
def is_committed(o: Outcome) -> Bool:
  match o:
    case Committed{post_id, created_at}:
      True{}
    case Liked{already}:
      False{}
    case Post{post}:
      False{}
    case Posts{posts}:
      False{}
    case Ping{}:
      False{}
    case NotFound{}:
      False{}
    case Unknown{}:
      False{}
    case Failed{}:
      False{}

def is_liked(o: Outcome) -> Bool:
  match o:
    case Committed{post_id, created_at}:
      False{}
    case Liked{already}:
      True{}
    case Post{post}:
      False{}
    case Posts{posts}:
      False{}
    case Ping{}:
      False{}
    case NotFound{}:
      False{}
    case Unknown{}:
      False{}
    case Failed{}:
      False{}

def is_ping(o: Outcome) -> Bool:
  match o:
    case Committed{post_id, created_at}:
      False{}
    case Liked{already}:
      False{}
    case Post{post}:
      False{}
    case Posts{posts}:
      False{}
    case Ping{}:
      True{}
    case NotFound{}:
      False{}
    case Unknown{}:
      False{}
    case Failed{}:
      False{}

def is_select_one(op: Req.Op) -> Bool:
  match op:
    case Req.SelectOne{}:
      True{}
    case Req.FeedQuery{}:
      False{}
    case Req.PostQuery{pid}:
      False{}
    case Req.InsertPost{uid, name, text}:
      False{}
    case Req.InsertLike{uid, pid}:
      False{}

# The Reply and Op predicates of the laws that say which operation may
# answer with a write reply: a Created only for InsertPost and a
# LikedReply only for InsertLike.
def is_insert_post(op: Req.Op) -> Bool:
  match op:
    case Req.InsertPost{uid, name, text}:
      True{}
    case Req.SelectOne{}:
      False{}
    case Req.FeedQuery{}:
      False{}
    case Req.PostQuery{pid}:
      False{}
    case Req.InsertLike{uid, pid}:
      False{}

def is_insert_like(op: Req.Op) -> Bool:
  match op:
    case Req.InsertLike{uid, pid}:
      True{}
    case Req.SelectOne{}:
      False{}
    case Req.FeedQuery{}:
      False{}
    case Req.PostQuery{pid}:
      False{}
    case Req.InsertPost{uid, name, text}:
      False{}

def is_created(r: Reply) -> Bool:
  match r:
    case Created{id, text, ts, name, like_count}:
      True{}
    case Error{status, msg}:
      False{}
    case Health{uptime_s}:
      False{}
    case Degraded{}:
      False{}
    case Feed{posts}:
      False{}
    case One{post}:
      False{}
    case LikedReply{status, already, post_id}:
      False{}

def is_liked_reply(r: Reply) -> Bool:
  match r:
    case LikedReply{status, already, post_id}:
      True{}
    case Error{status, msg}:
      False{}
    case Health{uptime_s}:
      False{}
    case Degraded{}:
      False{}
    case Feed{posts}:
      False{}
    case One{post}:
      False{}
    case Created{id, text, ts, name, like_count}:
      False{}

# The response policy: one row per row of the behaviour table in
# docs/specs/S02-response-policy.md. The operation and the outcome are
# matched together. Only a success row names the outcome that authorises
# it; every other outcome of an operation falls to the row after it, and
# that is an error: 503 for SelectOne, 500 for the rest. So a new Outcome
# constructor, and an Outcome of the wrong shape for its operation
# (Ping for FeedQuery), cannot produce a success. A write succeeds only on
# the one outcome that reports its commit: Committed for InsertPost,
# Liked for InsertLike. Created echoes the author and the text of the
# InsertPost op itself with the id and created_at the host reported
# (SPEC.md:47, 59), and like_count 0: a post has no likes when it is
# created, and the host renders the field it is given. Each field is
# consumed at most once, so no `+`.
def respond(op: Req.Op, out: Outcome, uptime_s: U32) -> Reply:
  match op out:
    case Req.SelectOne{} Ping{}:
      Health{uptime_s}
    case Req.SelectOne{} _:
      Degraded{}
    case Req.FeedQuery{} Posts{ps}:
      Feed{ps}
    case Req.FeedQuery{} _:
      Error{500, "internal server error"}
    case Req.PostQuery{pid} Post{p}:
      One{p}
    case Req.PostQuery{pid} NotFound{}:
      Error{404, "post not found"}
    case Req.PostQuery{pid} _:
      Error{500, "internal server error"}
    case Req.InsertPost{uid, name, text} Committed{id, ts}:
      Created{id, text, ts, name, 0}
    case Req.InsertPost{uid, name, text} _:
      Error{500, "internal server error"}
    case Req.InsertLike{uid, pid} Liked{False{}}:
      LikedReply{201, False{}, pid}
    case Req.InsertLike{uid, pid} Liked{True{}}:
      LikedReply{200, True{}, pid}
    case Req.InsertLike{uid, pid} NotFound{}:
      Error{404, "post not found"}
    case Req.InsertLike{uid, pid} _:
      Error{500, "internal server error"}
