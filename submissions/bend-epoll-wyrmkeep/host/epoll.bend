# The reactor and HTTP side of route A′'s host
# (docs/specs/S08-pg-effect-and-interpreter.md, docs/specs/S24-the-pure-round.md).
# Its C side, host/epoll.c, owns the listeners and every client socket; Bend
# holds the epoll descriptor as a Socket handle and names a client by
# (conn, gen), which the C side checks against its table on every Send.
# Epoll.turn takes the last round's commands as a list of Cmd and runs them
# before it waits. host/epoll.js is a stub: these effects run on the native
# lane only. This file has foreign imports, so no proof may import it or any
# file that imports it (docs/02-bend-runtime-audit.md §5.2).
import Base

# One host event as the C side hands it over: words and strings in order,
# Str for a string, Nest for a nested record and Row for one post row. host/wire.bend
# reads it into a Policy.HostEvent by name, so the C side never writes a constructor of
# src/. The codes are listed in host/wire.bend. Row also carries a reply's rows from
# Bend to C (Http.posts).
type Wire is Data:
  End{}
  Word{w: U32, rest: Wire}
  Str{s: String, rest: Wire}
  Nest{inner: Wire, rest: Wire}
  Row{id: U32, likes: U32, h: U32, rest: Wire}

# Reads the command line (bench/servers/bend-epoll/README.md lists the
# flags), opens the process's one reactor: an epoll descriptor, the trace
# file when --trace names one, the stats file when --stats names one, and a
# non-blocking listener on 0.0.0.0 for --port and for --extra-port when it is
# not 0 (SO_REUSEPORT with --workers above 1). Exits 64 before opening anything when
# (--pool + 1) x --workers is over 512. Answers the reactor handle and the
# number of rounds to serve (--fuel). A second open fails with EBUSY.
def Epoll.open() -> IO(Result<&1, &1, U32 & String, Socket & U32>):
  import "./epoll.c"
  import "./epoll.js"

# One command of a round, as host/main.bend hands it to Epoll.turn: the Send
# command, one constructor per Reply shape, then Queue and Open. Each carries
# the words and strings of its command in the order below, and host/epoll.c
# reads it by these names (CID), so the C side never names a constructor of
# src/. Pol.Command maps to Cmd in Bend (host/main.bend, cmd).
type Cmd is Data:
  SendError{req: U32, conn: U32, gen: U32, status: U32, msg: String}
  SendHealth{req: U32, conn: U32, gen: U32, uptime_s: U32}
  SendDegraded{req: U32, conn: U32, gen: U32}
  SendLiked{req: U32, conn: U32, gen: U32, status: U32, already: U32, post_id: U32}
  SendPosts{req: U32, conn: U32, gen: U32, shape: U32, rows: Wire}
  SendCreated{req: U32, conn: U32, gen: U32, id: U32, text: String, ts: String, name: String, like_count: U32}
  Queue{batch: U32, req: U32, kind: U32, uid: U32, pid: U32, name: String, text: String}
  Open{batch: U32, kind: U32}

# One round: first runs cmds, the commands of the round before, in list
# order (a Send writes the reply, a Queue appends to an open batch, an Open
# names the next open batch: host/pg.bend); then flushes the open batch when
# it is due, waits in epoll_pwait2 until something is ready or the next
# database deadline, accepts, reads, and lexes. Answers the round's events in
# the order step must see them: Flushed first, then Synced and Died, then
# Arrived and Gone. Never parks. The first turn gets Nil.
def Epoll.turn(h: Socket, cmds: List<Cmd>) -> IO(Socket & List<Wire>):
  import "./epoll.c"
  import "./epoll.js"

# Runs cmds, the last round's commands, as Epoll.turn does, then closes every
# client, the listeners, the database connections, the trace file and the
# epoll descriptor.
def Epoll.close(h: Socket, cmds: List<Cmd>) -> IO(Unit):
  import "./epoll.c"
  import "./epoll.js"

# The Send command, one Cmd per Reply shape. host/epoll.c writes the rendered
# reply to conn if conn's generation is still gen, and drops it otherwise.
# The reply ends the connection's request in flight: the next request on
# conn is lexed only after this one is written.
def Http.error(req: U32, conn: U32, gen: U32, status: U32, msg: String) -> Cmd:
  SendError{req, conn, gen, status, msg}

def Http.health(req: U32, conn: U32, gen: U32, uptime_s: U32) -> Cmd:
  SendHealth{req, conn, gen, uptime_s}

def Http.degraded(req: U32, conn: U32, gen: U32) -> Cmd:
  SendDegraded{req, conn, gen}

# already: 0 or 1.
def Http.liked(req: U32, conn: U32, gen: U32, status: U32, already: U32, post_id: U32) -> Cmd:
  SendLiked{req, conn, gen, status, already, post_id}

# The posts of a Feed or One reply for request req: shape 0 is Feed
# (200, {"posts":[..]}), 1 is One (200, {"post":..}, exactly one row, else 500).
# rows holds one Row per post, in reply order, End after the last
# (host/main.bend, pack_rows): its id and like_count as Bend gives them, and the
# handle of the row whose body, created_at and author the host renders. The
# host keeps a synced batch's rows until the next Epoll.turn has run this
# round's commands (host/pg.c); a handle that is not one of this round's rows,
# that is another request's, or that names another post than the id beside it
# stops the process with exit code 70 (docs/specs/S09-rows-as-handles.md), and
# so does a node in rows that is neither Row nor End.
def Http.posts(req: U32, conn: U32, gen: U32, shape: U32, rows: Wire) -> Cmd:
  SendPosts{req, conn, gen, shape, rows}

# The Created reply (201, {"post":..}): the post the request made, from the
# request's own text and author, the id and created_at its InsertPost
# returned, and the like_count respond gives (0).
def Http.created(req: U32, conn: U32, gen: U32, id: U32, text: String, ts: String, name: String, like_count: U32) -> Cmd:
  SendCreated{req, conn, gen, id, text, ts, name, like_count}
