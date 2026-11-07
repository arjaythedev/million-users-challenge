\ ============================================================================
\ pg-pipe.fs - pooled, pipelined, non-blocking Postgres engine.
\
\ Replaces the single blocking connection in pg.fs. Each worker owns POOL
\ connections (NREAD read + NWRITE write), all in libpq pipeline mode and
\ non-blocking. Queries are SENT without waiting; their PGsocket fds live in the
\ same epoll set as the client sockets, and results are collected when the
\ socket becomes readable. Each connection keeps a FIFO ring of the slot#s whose
\ requests are in flight on it, because pipelined results return in send order.
\
\ Contract with the event loop (epoll.fs):
\   pgp-init                        set up the pool, prepare statements
\   pgp-fd-count / pgp-fd i         enumerate PGsocket fds to register in epoll
\   pgp-is-pg-fd? ( fd -- conn|-1 ) map an epoll fd back to a connection index
\   pgp-submit ( slot stmt# write? -- ok? )  queue a request's statement on a conn
\   pgp-flush-all                   flush any conns with unsent bytes (+ sync)
\   pgp-on-readable ( conn -- )     consume input, complete finished requests
\
\ Completion: when a request's result is ready, pgp calls (via a deferred word)
\   pgp-complete ( slot res status -- )   defined by the event loop; it runs the
\ existing formatter for that slot's pending route and moves the slot to WRITING.
\ The engine PQclears nothing the formatter needs; it PQclears after completion.
\ ============================================================================

\ --- status/result constants (ExecStatusType, libpq 16/18) ---
1  constant PGRES-COMMAND-OK
2  constant PGRES-TUPLES-OK
7  constant PGRES-FATAL-ERROR
10 constant PGRES-PIPELINE-SYNC
11 constant PGRES-PIPELINE-ABORTED
67 constant PG-DIAG-SQLSTATE

\ status handed to the completion callback (mirror pg.fs PG-OK/ERR/FK)
0 constant PGP-OK
1 constant PGP-ERR
2 constant PGP-FK

\ --- pool sizing (overridable via env in pgp-init) ---
512 constant MAX-CONN            \ hard cap on connections per worker
variable n-read    4 n-read !    \ read connections (per worker) - set in pgp-init
variable n-write   2 n-write !   \ write connections (per worker)
: n-conn ( -- n ) n-read @ n-write @ + ;

\ --- per-connection state (parallel arrays indexed by conn#) ---
create conn-ptr   MAX-CONN cells allot    \ PGconn*
create conn-sock  MAX-CONN cells allot    \ PQsocket fd
create conn-inflt MAX-CONN cells allot    \ # requests in flight (ring occupancy)
create conn-unsnt MAX-CONN cells allot    \ statements sent since last sync/flush
create conn-wantw MAX-CONN cells allot    \ 1 if PQflush said "more to write" (need EPOLLOUT)
create conn-iswr  MAX-CONN cells allot    \ 1 if this is a write connection
variable conn-being-drained   0 conn-being-drained !  \ conn# currently in pgp-on-readable

\ FIFO ring of in-flight slot#s per connection.
256 constant RING-CAP                      \ max in-flight requests per connection
create ring-buf   MAX-CONN RING-CAP * cells allot
create ring-head  MAX-CONN cells allot
create ring-tail  MAX-CONN cells allot
: cc ( conn base -- addr ) swap cells + ;  \ base[conn] address; called as `conn ARRAY cc`
: ring@ ( conn i -- addr ) swap RING-CAP * + cells ring-buf + ;

: ring-push ( slot conn -- )        \ enqueue slot on conn's ring
  { conn }
  conn ring-tail cc @  ( slot tailidx )
  conn swap ring@ !                 \ ring[conn][tail] = slot
  conn ring-tail cc @ 1+ RING-CAP mod  conn ring-tail cc !
  1 conn conn-inflt cc +! ;

: ring-pop ( conn -- slot )         \ dequeue oldest slot (FIFO)
  { conn }
  conn ring-head cc @  ( headidx )
  conn over ring@ @    ( headidx slot )
  swap 1+ RING-CAP mod  conn ring-head cc !
  -1 conn conn-inflt cc +!
  ;

: ring-empty? ( conn -- f ) conn-inflt cc @ 0= ;

\ --- completion callback, provided by the event loop ---
defer pgp-complete  ( slot res status -- )
: pgp-complete-default ( slot res status -- ) drop 2drop ;
' pgp-complete-default is pgp-complete

\ --- C-string helpers (separate buffers; the spike proved aliasing breaks prepare) ---
: cstr-len ( z -- u ) dup 0= if drop 0 exit then dup begin dup c@ while 1+ repeat swap - ;
create pgp-sqlbuf 8192 allot
: >sqlz ( a u -- z ) dup 8191 > abort" pgp sql too long" pgp-sqlbuf swap 2dup + >r move 0 r> c! pgp-sqlbuf ;
create pgp-namebuf 64 allot
: >namez ( a u -- z ) dup 63 > abort" pgp name too long" pgp-namebuf swap 2dup + >r move 0 r> c! pgp-namebuf ;

variable pgp-conninfo-a  variable pgp-conninfo-u   \ saved for reconnect

\ SQLSTATE of an error result ( res -- a u )
: res-sqlstate ( res -- a u )
  PG-DIAG-SQLSTATE PQresultErrorField dup 0= if drop s" " exit then dup cstr-len ;

\ map a PGRES status -> PGP-OK / PGP-ERR / PGP-FK (23503)
: status>pgp ( res st -- status )
  { res st }
  st PGRES-TUPLES-OK  = if PGP-OK exit then
  st PGRES-COMMAND-OK = if PGP-OK exit then
  st PGRES-FATAL-ERROR = if
    res res-sqlstate s" 23503" compare 0= if PGP-FK else PGP-ERR then exit then
  PGP-ERR ;

\ --- prepare the five statements on one connection ---
: conn-prepare-one ( conn name-a name-u sql-a sql-u -- )
  >sqlz { zsql } >namez { zn } { conn }
  conn conn-ptr cc @ zn zsql 0 0 PQprepare   ( res )
  dup 0= abort" pgp prepare null"
  dup PQresultStatus PGRES-COMMAND-OK <> if
    cr ." pgp prepare failed: " dup res-sqlstate type cr PQclear true abort" pgp prepare" then
  PQclear ;

: conn-prepare-all ( conn -- )
  { conn }
  conn s" health" s" SELECT 1" conn-prepare-one
  conn s" feed"
    s" SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
    conn-prepare-one
  conn s" post"
    s" SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1"
    conn-prepare-one
  conn s" insert"
    s" INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
    conn-prepare-one
  conn s" like"
    s" INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
    conn-prepare-one ;

\ open one connection, set nonblocking + pipeline mode, prepare, record socket
: conn-open ( conn -- )
  { conn }
  pgp-conninfo-a @ pgp-conninfo-u @ >sqlz PQconnectdb  ( pgconn )
  dup 0= abort" pgp connect null"
  dup PQstatus 0<> if cr ." pgp connect failed: " dup PQerrorMessage dup cstr-len type cr
    true abort" pgp connect" then
  conn conn-ptr cc !
  \ session UTC (synchronous, before entering pipeline mode)
  conn conn-ptr cc @ s" SET TIME ZONE 'UTC'" >sqlz PQexec dup 0= abort" pgp SET TZ null"
  dup PQresultStatus PGRES-COMMAND-OK <> abort" pgp SET TZ" PQclear
  conn conn-prepare-all
  conn conn-ptr cc @ 1 PQsetnonblocking drop
  conn conn-ptr cc @ PQenterPipelineMode 0= abort" pgp enterPipelineMode"
  conn conn-ptr cc @ PQsocket  conn conn-sock cc !
  0 conn conn-inflt cc !  0 conn conn-unsnt cc !  0 conn conn-wantw cc !
  0 conn ring-head cc !   0 conn ring-tail cc ! ;

\ --- init: read env, open the pool ---
: env-n ( name-a name-u dflt -- n )
  { d } getenv dup if  0. 2swap >number 2drop drop  dup 0= if drop d then
  else 2drop d then ;

: pgp-init ( conninfo-a conninfo-u -- )
  pgp-conninfo-u ! pgp-conninfo-a !
  \ DB_POOL = total connections (default 6); writes = DB_WRITE (default: 1/3, min 1)
  s" DB_POOL"  6 env-n { total }
  s" DB_WRITE" total 3 / 1 max env-n { nw }
  nw total 1- min 1 max to nw
  total nw - 1 max  n-read !
  nw n-write !
  n-conn MAX-CONN > if MAX-CONN n-read @ min n-read ! then
  n-conn 0 ?do
    i conn-open
    i n-read @ < if 0 else 1 then  i conn-iswr cc !    \ first n-read are read conns
  loop
  cr ." pgp: " n-read @ . ." read + " n-write @ . ." write connections" cr ;

\ --- fd enumeration for epoll registration ---
: pgp-fd-count ( -- n ) n-conn ;
: pgp-fd ( i -- fd ) conn-sock cc @ ;
\ map an epoll fd back to conn# (linear scan over the small pool), -1 if not ours
: pgp-fd->conn ( fd -- conn|-1 )
  { fd } n-conn 0 ?do fd i conn-sock cc @ = if i unloop exit then loop -1 ;

\ --- connection selection (round-robin within read pool / write pool) ---
variable rr-read   0 rr-read !
variable rr-write  0 rr-write !
: pick-read ( -- conn )    \ least-recently-used read conn (simple RR)
  rr-read @ n-read @ mod   dup 1+ rr-read ! ;
: pick-write ( -- conn )   \ write conns are [n-read .. n-conn)
  rr-write @ n-write @ mod  n-read @ +  1 rr-write +! ;

\ --- submit a request's prepared statement on a connection ---------------
\ Params must already be set by the caller via pg-set-int / pg-set-text
\ (tdsc_param_set), exactly as the synchronous pg-exec expected.
\ stmt# selects the prepared name; write? routes to a write conn + own Sync.
: stmt-name ( stmt# -- a u )
  case
    0 of s" health" endof
    1 of s" feed"   endof
    2 of s" post"   endof
    3 of s" insert" endof
    4 of s" like"   endof
  endcase ;

\ how many params each statement binds
: stmt-nparams ( stmt# -- n )
  case
    0 of 0 endof   \ health
    1 of 0 endof   \ feed
    2 of 1 endof   \ post by id
    3 of 2 endof   \ insert
    4 of 2 endof   \ like
  endcase ;

\ pgp-submit ( slot stmt# write? -- ok? )
: pgp-submit ( slot stmt# write? -- ok? )
  { slot st# write? }
  write? if pick-write else pick-read then { conn }
  \ ring full? (shouldn't happen under backpressure) -> fail, caller 503s
  conn conn-inflt cc @ RING-CAP 1- >= if false exit then
  st# stmt-name >namez { zn }
  st# stmt-nparams { np }
  conn conn-ptr cc @ zn np
  np 0= if 0 else tdsc_params_ptr then   ( conn zn np pvals )
  0 0 0                                  ( conn zn np pvals plens pfmts resultFmt )
  PQsendQueryPrepared                    ( ok )
  0= if false exit then
  slot conn ring-push                    \ park this slot on the conn's ring
  1 conn conn-unsnt cc +!
  write? if conn conn-ptr cc @ PQpipelineSync drop then
  true ;

\ flush one connection's buffered bytes; set wantw if more remains
: conn-flush ( conn -- )
  { conn }
  conn conn-ptr cc @ PQflush             ( r )
  dup 1 = if drop 1 conn conn-wantw cc ! exit then
  0 conn conn-wantw cc ! drop ;

\ end-of-pass: for read conns with unsent statements, add one Sync, then flush all.
: pgp-flush-all ( -- )
  n-conn 0 ?do
    i conn-unsnt cc @ 0> if
      i conn-iswr cc @ 0= if i conn-ptr cc @ PQpipelineSync drop then  \ read batch Sync
      0 i conn-unsnt cc !
    then
  loop
  n-conn 0 ?do i conn-flush loop ;

\ --- connection failure / abort handling ---------------------------------
\ On a dropped connection, fail every in-flight request with PGP-ERR (the event
\ loop will 500 them) so clients never hang; then the connection is reopened.
: pgp-conn-fail ( conn -- )
  { conn }
  begin conn ring-empty? 0= while
    conn ring-pop 0 PGP-ERR pgp-complete   \ slot res=0 status=ERR
  repeat
  \ best-effort reconnect (synchronous connect is fine: rare, off the hot path)
  conn conn-ptr cc @ PQfinish
  conn conn-open ;

\ --- drain results on a readable connection ------------------------------
\ Consume input, then pull every ready result. Each request yields one
\ non-NULL result then a NULL terminator; each batch ends with a PIPELINE_SYNC.
\ Match non-terminal results to ring-pop (FIFO = send order) and complete.
: complete-one ( res -- )    \ res is a real (non-sync) result; match to ring head
  { res }
  res PQresultStatus { st }
  conn-being-drained @ { conn }
  conn ring-empty? if res PQclear exit then   \ defensive: no waiting slot
  conn ring-pop { slot }
  res st status>pgp { pgpst }
  slot res pgpst pgp-complete ;               \ formatter reads res, then PQclears

\ Drain all currently-available results on a connection WITHOUT spinning.
\ Rules (libpq pipeline, non-blocking):
\  - Only call PQgetResult when PQisBusy=0 (a full result is buffered), else we
\    would block / spin. When PQisBusy=1 we've consumed all we can for now and
\    return to epoll to wait for more bytes.
\  - PQgetResult returns a chain of results then a single NULL terminator per
\    command; after the NULL, if PQisBusy becomes 1 we must stop (more pending
\    on the wire); if it's still 0 another result is buffered, keep going.
\  - To avoid the NULL/not-busy spin, we stop as soon as PQgetResult returns
\    NULL while PQisBusy is 0 AND the ring is empty (nothing left to complete),
\    or whenever PQisBusy flips to 1.
: pgp-on-readable ( conn -- )
  dup conn-being-drained !
  { conn }
  conn conn-ptr cc @ PQconsumeInput 0= if conn pgp-conn-fail exit then
  begin
    conn conn-ptr cc @ PQisBusy if false       \ busy: wait for more input
    else
      conn conn-ptr cc @ PQgetResult { res }
      res 0= if
        false
      else
        res PQresultStatus { st }
        st PGRES-PIPELINE-SYNC = if res PQclear
        else res complete-one then
        true
      then
    then
  while repeat ;


\ --- public helper: flush a single connection (called on its EPOLLOUT) ---
: conn-flush-public ( conn -- ) conn-flush ;

\ Register every pool connection's PQsocket for readability via a ctl function
\ the event loop installs (avoids load-order coupling to epoll.fs constants).
defer pgp-epoll-register-fn   ( fd -- )
: pgp-register-all ( -- )
  n-conn 0 ?do i conn-sock cc @ pgp-epoll-register-fn loop ;
