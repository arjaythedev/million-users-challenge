# Reads one host event, as host/epoll.c and host/pg.c hand it over in a
# Wire (host/epoll.bend), into a Policy.HostEvent. Every constructor of src/
# is named here, in Bend, so the C side never writes an arm number
# (docs/09-trusted-computing-base.md, the S06 row). A code this table does
# not define fails closed: an unknown event or a malformed one is no event,
# an unknown route is Unknown (404), an unknown auth is BadToken, an unknown
# body is NotJson, an unknown outcome is Failed. Synced's ok and Liked's
# already are 0 or 1: an ok word of any other value fails the batch (and
# host/main.bend logs it), an already word of any other value is Failed.
#
#   event       tag 0 Arrived  [req, conn, gen, route, id, auth, uid, Str name, body, Str text]
#               tag 1 Flushed  [batch]
#               tag 2 Synced   [batch, ok, uptime_s, Nest outs]
#               tag 3 Died     [batch]
#               tag 4 Gone     [conn, gen]
#   route, id   src/decode.bend (route_of); body src/decode.bend (body_of)
#   auth        0 NoBearer, 1 BadToken, 2 BadPayload, 3 User{uid, name}
#   outs        one Nest per statement: [req, kind, fields]
#               kind 0 Committed [post_id, Str created_at]
#                    1 Liked     [already]
#                    2 Post      [Row]
#                    3 Posts     [Nest rows]: one Row per row, in the reply's order
#                    4 Ping, 5 NotFound, 6 Unknown, 7 Failed
#   Row         Row{id, likes, h}: one node per post row; h is the host's handle
#               for the row's body, created_at and author (host/pg.c, pg_row_put).
#               The trace writes it as [id, h, likes] (host/epoll.c, ev_trace), the
#               bytes a Nest of three words would give: bench/servers/bend-epoll/replay.mjs
#               reads a three-number array in a row's place as a Row
#
# Each level is a run of Nest cells, or of Row cells for rows, so every function
# here recurses on a field of its argument. This file imports host/epoll.bend for
# the Wire type, so no proof may import it.
import Base
import ../src/request.bend as Req
import ../src/respond.bend as Rsp
import ../src/policy.bend as Pol
import ../src/decode.bend as D
import ./epoll.bend as E

# 0 is False and 1 is True; host/pg.c writes no other word. Any other word is
# None, and each caller fails closed on it.
def flag(w: U32) -> Maybe<Bool>:
  match w:
    case 0:
      Some{False{}}
    case 1:
      Some{True{}}
    case _:
      None{}

# Synced's ok word: 1 is True and 0 is False. Any other word is False too, so
# its batch fails closed, and the second Bool notes it (host/main.bend logs it).
def ok_seen(m: Maybe<Bool>) -> Bool & Bool:
  match m:
    case Some{b}:
      (b, False{})
    case None{}:
      (False{}, True{})

def auth_with(code: U32, uid: U32, name: String) -> Req.Auth:
  match code:
    case 0:
      Req.NoBearer{}
    case 1:
      Req.BadToken{}
    case 2:
      Req.BadPayload{}
    case 3:
      Req.User{uid, name}
    case _:
      Req.BadToken{}

# ---- rows and outcomes ----

def row_on(r: Rsp.PostRow, x: List<&2, Rsp.PostRow> & Bool) -> List<&2, Rsp.PostRow> & Bool:
  (rs, bad) = x
  (r <> rs, bad)

# The rows of a Posts outcome in order, and True when the run holds a node that
# is neither Row nor End: posts then answers Failed for the whole outcome.
def rows_of(w: E.Wire) -> List<&2, Rsp.PostRow> & Bool:
  match w:
    case E.Row{id, likes, h, rest}:
      row_on(Rsp.PostRow{id, likes, h}, rows_of(rest))
    case E.End{}:
      (Nil{}, False{})
    case _:
      (Nil{}, True{})

def posts_or_failed(x: List<&2, Rsp.PostRow> & Bool) -> Rsp.Outcome:
  (rs, bad) = x
  match bad:
    case False{}:
      Rsp.Posts{rs}
    case True{}:
      Rsp.Failed{}

def committed(w: E.Wire) -> Rsp.Outcome:
  match w:
    case E.Word{id, E.Str{ts, E.End{}}}:
      Rsp.Committed{id, ts}
    case _:
      Rsp.Failed{}

def liked_or_failed(m: Maybe<Bool>) -> Rsp.Outcome:
  match m:
    case Some{already}:
      Rsp.Liked{already}
    case None{}:
      Rsp.Failed{}

def liked(w: E.Wire) -> Rsp.Outcome:
  match w:
    case E.Word{already, E.End{}}:
      liked_or_failed(flag(already))
    case _:
      Rsp.Failed{}

def post(w: E.Wire) -> Rsp.Outcome:
  match w:
    case E.Row{id, likes, h, E.End{}}:
      Rsp.Post{Rsp.PostRow{id, likes, h}}
    case _:
      Rsp.Failed{}

def posts(w: E.Wire) -> Rsp.Outcome:
  match w:
    case E.Nest{rs, E.End{}}:
      posts_or_failed(rows_of(rs))
    case _:
      Rsp.Failed{}

def outcome_of(kind: U32, w: E.Wire) -> Rsp.Outcome:
  match kind:
    case 0:
      committed(w)
    case 1:
      liked(w)
    case 2:
      post(w)
    case 3:
      posts(w)
    case 4:
      Rsp.Ping{}
    case 5:
      Rsp.NotFound{}
    case 6:
      Rsp.Unknown{}
    case _:
      Rsp.Failed{}

# A record that is not [req, kind, ..] answers for no request: req 0 is
# never issued (host/epoll.c counts requests from 1).
def out_of(w: E.Wire) -> +Sigma<U32, _ => Rsp.Outcome>:
  match w:
    case E.Word{req, E.Word{kind, fs}}:
      (req, outcome_of(kind, fs))
    case _:
      (0, Rsp.Failed{})

def outs_of(w: E.Wire) -> List<&2, +Sigma<U32, _ => Rsp.Outcome>>:
  match w:
    case E.Nest{o, rest}:
      out_of(o) <> outs_of(rest)
    case _:
      Nil{}

# ---- events ----

def arrived(w: E.Wire) -> Maybe<Pol.HostEvent>:
  match w:
    case E.Word{req, E.Word{conn, E.Word{gen, E.Word{route, E.Word{id, E.Word{auth, E.Word{uid, E.Str{name, E.Word{body, E.Str{text, E.End{}}}}}}}}}}}:
      Some{Pol.Arrived{req, conn, gen, Req.Request{D.route_of(route, id), auth_with(auth, uid, name), D.body_of(body, Some{text})}}}
    case _:
      None{}

def flushed(w: E.Wire) -> Maybe<Pol.HostEvent>:
  match w:
    case E.Word{b, E.End{}}:
      Some{Pol.Flushed{b}}
    case _:
      None{}

def synced_with(b: U32, k: Bool & Bool, outs: E.Wire, uptime: U32) -> Maybe<Pol.HostEvent> & Bool:
  (ok, odd) = k
  (Some{Pol.Synced{b, ok, outs_of(outs), uptime}}, odd)

def synced(w: E.Wire) -> Maybe<Pol.HostEvent> & Bool:
  match w:
    case E.Word{b, E.Word{ok, E.Word{uptime, E.Nest{outs, E.End{}}}}}:
      synced_with(b, ok_seen(flag(ok)), outs, uptime)
    case _:
      (None{}, False{})

def died(w: E.Wire) -> Maybe<Pol.HostEvent>:
  match w:
    case E.Word{b, E.End{}}:
      Some{Pol.Died{b}}
    case _:
      None{}

def gone(w: E.Wire) -> Maybe<Pol.HostEvent>:
  match w:
    case E.Word{conn, E.Word{gen, E.End{}}}:
      Some{Pol.Gone{conn, gen}}
    case _:
      None{}

def event_by(tag: U32, w: E.Wire) -> Maybe<Pol.HostEvent> & Bool:
  match tag:
    case 0:
      (arrived(w), False{})
    case 1:
      (flushed(w), False{})
    case 2:
      synced(w)
    case 3:
      (died(w), False{})
    case 4:
      (gone(w), False{})
    case _:
      (None{}, False{})

# The event, and True when w is a Synced whose ok word is neither 0 nor 1.
def event_seen(w: E.Wire) -> Maybe<Pol.HostEvent> & Bool:
  match w:
    case E.Word{tag, rest}:
      event_by(tag, rest)
    case _:
      (None{}, False{})

def first(r: Maybe<Pol.HostEvent> & Bool) -> Maybe<Pol.HostEvent>:
  (m, odd) = r
  m

def event_of(w: E.Wire) -> Maybe<Pol.HostEvent>:
  first(event_seen(w))
