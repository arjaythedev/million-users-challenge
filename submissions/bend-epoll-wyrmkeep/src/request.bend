# The request classifier: the first pure transition of the application
# kernel. The native host lexes bytes into a Request (the boundary: see
# docs/09-trusted-computing-base.md); classify decides, with no effect,
# whether the request is rejected now or which database operation runs.
# Contract: vendor/million-users-challenge/SPEC.md (endpoints 42-48,
# errors 64-81, auth 83-88). Types are fixed by docs/specs/S01-request-classifier.md;
# their constructor order is fixed by docs/specs/S06-fail-closed-order.md.
import Base

# The :id path segment as the host lexer saw it. ValidId promises digits
# only, no leading sign or dot, value 1..4294967295 (ids past U32 are a
# documented limitation; the seed's largest post id is 500000). InvalidId
# is last on purpose; see Auth.
type PathId is Data:
  ValidId{id: U32}
  InvalidId{}

# The matched route. Unknown is every path not in the spec, and it is last
# on purpose; see Auth.
type Route is Data:
  Health{}
  Feed{}
  PostById{pid: PathId}
  CreatePost{}
  LikePost{pid: PathId}
  Unknown{}

# The outcome of the host's bearer-token check. The host verifies HS256
# and exp (boundary assumption A2); the kernel only trusts its verdict.
#
# The order of the constructors is load-bearing, here and in Route, PathId
# and Body: the rejecting one is last. The emitter compiles the last arm of
# a match as an unchecked `else`, and a value that a native host builds as
# a boxed term reaches the kernel through a chain whose `else` is the last
# constructor of the type. A tag the host should never send therefore lands
# on NoBearer (401), never on User. Every match on these types keeps the
# same order (the reason: docs/specs/S06-fail-closed-order.md).
type Auth is Data:
  User{uid: U32, name: String}
  BadPayload{}
  BadToken{}
  NoBearer{}

# The POST /posts body after the host's JSON parse and trim. The last
# constructor rejects, as for Auth: a bogus tag lands on NotJson (400).
type Body is Data:
  Trimmed{s: String}
  TooLong{}
  Missing{}
  NoneSent{}
  NotJson{}

type Request is Data:
  Request{route: Route, auth: Auth, body: Body}

# The database operation a request runs (SPEC.md:54-62), one per request.
type Op is Data:
  SelectOne{}
  FeedQuery{}
  PostQuery{pid: U32}
  InsertPost{uid: U32, name: String, text: String}
  InsertLike{uid: U32, pid: U32}

# What the kernel decides before any effect: reject now with a status and
# the spec's message, or run one operation.
type Decision is Data:
  Reject{status: U32, msg: String}
  Run{op: Op}

# The status of a decision; a Run has none yet (0).
def status(d: Decision) -> U32:
  match d:
    case Reject{status, msg}:
      status
    case Run{op}:
      0

def is_user(a: Auth) -> Bool:
  match a:
    case User{uid, name}:
      True{}
    case BadPayload{}:
      False{}
    case BadToken{}:
      False{}
    case NoBearer{}:
      False{}

def is_protected(r: Route) -> Bool:
  match r:
    case Health{}:
      False{}
    case Feed{}:
      False{}
    case PostById{pid}:
      False{}
    case CreatePost{}:
      True{}
    case LikePost{pid}:
      True{}
    case Unknown{}:
      False{}

def is_text(b: Body) -> Bool:
  match b:
    case Trimmed{s}:
      True{}
    case TooLong{}:
      False{}
    case Missing{}:
      False{}
    case NoneSent{}:
      False{}
    case NotJson{}:
      False{}

# The authenticated user id of a request, if any.
def auth_uid(req: Request) -> Maybe<U32>:
  match req:
    case Request{route, auth, body}:
      match auth:
        case User{uid, name}:
          Some{uid}
        case BadPayload{}:
          None{}
        case BadToken{}:
          None{}
        case NoBearer{}:
          None{}

# The user id a write operation carries, if it is a write.
def op_uid(d: Decision) -> Maybe<U32>:
  match d:
    case Reject{status, msg}:
      None{}
    case Run{op}:
      match op:
        case SelectOne{}:
          None{}
        case FeedQuery{}:
          None{}
        case PostQuery{pid}:
          None{}
        case InsertPost{uid, name, text}:
          Some{uid}
        case InsertLike{uid, pid}:
          Some{uid}

# Whether a decision runs a write (an INSERT).
def is_write(d: Decision) -> Bool:
  match d:
    case Reject{status, msg}:
      False{}
    case Run{op}:
      match op:
        case SelectOne{}:
          False{}
        case FeedQuery{}:
          False{}
        case PostQuery{pid}:
          False{}
        case InsertPost{uid, name, text}:
          True{}
        case InsertLike{uid, pid}:
          True{}

# The request classifier: SPEC.md:42-48 and 64-81, one row per case of the
# behaviour table in docs/specs/S01-request-classifier.md. The route, the
# auth verdict and the body are matched together. The rows for a protected
# route with no User ignore the id and the body, so a missing token wins
# over a bad id (SPEC.md:81). The body decides only on CreatePost with a
# User. Each field is consumed at most once, so no `+`.
#
# Row order is load-bearing. The emitter tests a match's arms in the order
# the rows first name them and compiles the last as an unchecked `else`.
# Within each group the rows that run a write come first and the rejecting
# rows last, so a tag the host should never send lands on a rejection: a
# bogus route on Unknown (404), auth on NoBearer (401), body on NotJson
# (400) and id on InvalidId (400). Keep the order; the reason is in
# docs/specs/S06-fail-closed-order.md.
def classify(req: Request) -> Decision:
  match req:
    case Request{route, auth, body}:
      match route auth body:
        case Health{} _ _:
          Run{SelectOne{}}
        case Feed{} _ _:
          Run{FeedQuery{}}
        case PostById{ValidId{id}} _ _:
          Run{PostQuery{id}}
        case PostById{InvalidId{}} _ _:
          Reject{400, "invalid post id"}
        case CreatePost{} User{uid, name} Trimmed{s}:
          Run{InsertPost{uid, name, s}}
        case CreatePost{} User{uid, name} TooLong{}:
          Reject{400, "body must be at most 500 characters"}
        case CreatePost{} User{uid, name} Missing{}:
          Reject{400, "body is required"}
        case CreatePost{} User{uid, name} NoneSent{}:
          Reject{400, "malformed JSON body"}
        case CreatePost{} User{uid, name} NotJson{}:
          Reject{400, "malformed JSON body"}
        case CreatePost{} BadPayload{} _:
          Reject{401, "invalid token payload"}
        case CreatePost{} BadToken{} _:
          Reject{401, "invalid or expired token"}
        case CreatePost{} NoBearer{} _:
          Reject{401, "missing bearer token"}
        case LikePost{ValidId{id}} User{uid, name} _:
          Run{InsertLike{uid, id}}
        case LikePost{InvalidId{}} User{uid, name} _:
          Reject{400, "invalid post id"}
        case LikePost{_} BadPayload{} _:
          Reject{401, "invalid token payload"}
        case LikePost{_} BadToken{} _:
          Reject{401, "invalid or expired token"}
        case LikePost{_} NoBearer{} _:
          Reject{401, "missing bearer token"}
        case Unknown{} _ _:
          Reject{404, "not found"}
