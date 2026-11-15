# The inverse of the word encoding that the C lexer in host/epoll.c writes
# for one request (docs/specs/S05-epoll-effect-spike.md), and the
# decision -> (status, kind) map that host/epoll.c turns into a canned
# reply. Pure: no effect, no foreign import, so a proof may import it.
#
# A request is six words [0, route, id, auth, uid, body]:
#   route 0 Health, 1 Feed, 2 PostById, 3 CreatePost, 4 LikePost, 5 Unknown
#   id    0 for an invalid :id, else the id (1..4294967295)
#   auth  0 NoBearer, 1 BadToken, 2 BadPayload, 3 User (uid in word 4)
#   body  0 NoneSent, 1 NotJson, 2 Missing, 3 TooLong, 4 Trimmed
# Every code the encoding does not define fails closed: a route is Unknown
# (404), an auth is BadToken (never User), a body is NotJson, and a list
# that is not six words starting with 0 is a request for an unknown path.
# The words carry no String: a User's name is "" and a Trimmed body needs
# its text from decode_with.
import Base
import ./request.bend as Req

def path_id(+id: U32) -> Req.PathId:
  match id:
    case 0:
      Req.InvalidId{}
    case _:
      Req.ValidId{id}

def route_of(code: U32, +id: U32) -> Req.Route:
  match code:
    case 0:
      Req.Health{}
    case 1:
      Req.Feed{}
    case 2:
      Req.PostById{path_id(id)}
    case 3:
      Req.CreatePost{}
    case 4:
      Req.LikePost{path_id(id)}
    case _:
      Req.Unknown{}

def auth_of(code: U32, uid: U32) -> Req.Auth:
  match code:
    case 0:
      Req.NoBearer{}
    case 1:
      Req.BadToken{}
    case 2:
      Req.BadPayload{}
    case 3:
      Req.User{uid, ""}
    case _:
      Req.BadToken{}

# A Trimmed code with no text that crossed is a missing body.
def trimmed(text: Maybe<String>) -> Req.Body:
  match text:
    case None{}:
      Req.Missing{}
    case Some{s}:
      Req.Trimmed{s}

def body_of(code: U32, text: Maybe<String>) -> Req.Body:
  match code:
    case 0:
      Req.NoneSent{}
    case 1:
      Req.NotJson{}
    case 2:
      Req.Missing{}
    case 3:
      Req.TooLong{}
    case 4:
      trimmed(text)
    case _:
      Req.NotJson{}

def not_found() -> Req.Request:
  Req.Request{Req.Unknown{}, Req.NoBearer{}, Req.NoneSent{}}

def decode6(tag: U32, route: U32, id: U32, auth: U32, uid: U32, body: U32, text: Maybe<String>) -> Req.Request:
  match tag:
    case 0:
      Req.Request{route_of(route, id), auth_of(auth, uid), body_of(body, text)}
    case _:
      not_found()

# The words after the first. serve matches the first word itself (1 means
# the peer closed), so it hands the rest here without rebuilding the list.
def decode_tail(tag: U32, rest: List<U32>, text: Maybe<String>) -> Req.Request:
  match rest:
    case Con{route, Con{id, Con{auth, Con{uid, Con{body, Nil{}}}}}}:
      decode6(tag, route, id, auth, uid, body, text)
    case _:
      not_found()

# The words of one request, and the body's text if one crossed beside them.
def decode_with(ws: List<U32>, text: Maybe<String>) -> Req.Request:
  match ws:
    case Nil{}:
      not_found()
    case Con{tag, rest}:
      decode_tail(tag, rest, text)

def decode(ws: List<U32>) -> Req.Request:
  decode_with(ws, None{})

def run_reply(op: Req.Op) -> U32 & U32:
  match op:
    case Req.SelectOne{}:
      (200, 1)
    case Req.FeedQuery{}:
      (200, 2)
    case Req.PostQuery{pid}:
      (200, 3)
    case Req.InsertPost{uid, name, text}:
      (201, 4)
    case Req.InsertLike{uid, pid}:
      (201, 5)

# (status, kind): kind 0 Reject, 1 SelectOne, 2 FeedQuery, 3 PostQuery,
# 4 InsertPost, 5 InsertLike. A Reject keeps its status; the C side picks
# the body by status (the message strings do not cross in this spike).
def reply_of(d: Req.Decision) -> U32 & U32:
  match d:
    case Req.Reject{status, msg}:
      (status, 0)
    case Req.Run{op}:
      run_reply(op)
