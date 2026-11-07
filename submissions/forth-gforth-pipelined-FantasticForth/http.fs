\ ============================================================================
\ http.fs - HTTP/1.1 request handling: parsing, routing for the five endpoints,
\ the response builder, and the exact error contract. Integrates pg.fs, jwt.fs,
\ json.fs. The connection/accept loop that drives these lives in epoll.fs.
\ Load order: ffi.fs, pg.fs, jwt.fs, json.fs, then this file.
\ ============================================================================

\ ---------------------------------------------------------------------------
\ Config from environment
\ ---------------------------------------------------------------------------
create host-buf 64 allot   variable host-len
variable the-port
variable the-extra-port
variable the-workers
variable start-time

: getenv-or ( name-a name-u dflt-a dflt-u -- a u )
  2>r getenv dup 0= if 2drop 2r> else 2rdrop then ;

: now-s ( -- n ) utime 1000000 um/mod nip ;   \ unix seconds

\ parse an unsigned decimal env value to a number ( a u -- n )
: env>n ( a u -- n ) 0. 2swap >number 2drop drop ;

: config-load ( -- )
  \ SPEC runtime contract: HOST, PORT, EXTRA_PORT from the environment. We serve
  \ the internet directly on both ports (no Nginx). Defaults match the test
  \ harness (127.0.0.1:3000 / 3001) when the env is unset.
  s" HOST" s" 0.0.0.0" getenv-or  dup host-len ! host-buf swap move
  s" PORT" s" 80" getenv-or  env>n the-port !
  s" EXTRA_PORT" getenv dup if env>n else 2drop 0 then  the-extra-port !
  \ WORKERS: number of worker processes (each binds both ports via SO_REUSEPORT
  \ and holds its own Postgres connection). Default 1 (single process). On the
  \ 16-core box this is set to the core count (see README / start.sh).
  s" WORKERS" getenv dup if env>n else 2drop 1 then
    dup 1 < if drop 1 then  the-workers !
  now-s start-time ! ;

create hostz 80 allot
: host>z ( -- zaddr )
  host-buf host-len @ hostz swap move  0 hostz host-len @ + c!  hostz ;

\ ---------------------------------------------------------------------------
\ Prepared statements live in pg.fs (prepared once per connection at startup by
\ pg-prepare-all, called from server.fs). http.fs just names them at exec time
\ via nm-feed / nm-post / nm-insert / nm-like / nm-health. No per-statement
\ cursor state here: libpq returns a whole PGresult per execution (see pg.fs).
\ ---------------------------------------------------------------------------

\ ---------------------------------------------------------------------------
\ Buffers
\ ---------------------------------------------------------------------------
8192 constant REQ-MAX
create req-buf REQ-MAX allot   variable req-len

16384 65536 + constant RESP-MAX
create resp-buf RESP-MAX allot variable resp-len
: resp-reset ( -- ) 0 resp-len ! ;
: resp+ ( a u -- )
  dup resp-len @ + RESP-MAX > abort" resp overflow"
  resp-buf resp-len @ + swap dup >r move r> resp-len +! ;
: resp-c ( ch -- ) resp-buf resp-len @ + c! 1 resp-len +! ;

\ ---------------------------------------------------------------------------
\ Parsed request fields
\ ---------------------------------------------------------------------------
2variable r-method     \ addr/u into req-buf
2variable r-path       \ addr/u into req-buf
2variable r-auth       \ addr/u Authorization value (0 0 if none)
2variable r-body       \ addr/u request body
variable  r-clen
variable  r-close      \ 1 if Connection: close

\ ---------------------------------------------------------------------------
\ Small string helpers
\ ---------------------------------------------------------------------------
: s= ( a1 u1 a2 u2 -- f ) compare 0= ;

: lower ( c -- c' ) dup [char] A [char] Z 1+ within if 32 + then ;

\ case-insensitive equality
: ci= ( a1 u1 a2 u2 -- f )
  { a1 u1 a2 u2 }
  u1 u2 <> if false exit then
  u1 0 ?do
    a1 i + c@ lower  a2 i + c@ lower  <> if false unloop exit then
  loop true ;

\ does (a u) start with literal (p pu) ?
: starts-with ( a u p pu -- f )
  { a u p pu }
  u pu < if false exit then
  a pu p pu compare 0= ;

\ find byte c in (a u); ( idx true | false )
: findc ( a u c -- idx true | false )
  { a u c }
  u 0 ?do a i + c@ c = if i true unloop exit then loop false ;

\ find substring (n nu) in (h hu): ( idx true | false )
: findsub ( h hu n nu -- idx true | false )
  { h hu n nu }
  nu 0= if 0 true exit then
  hu nu < if false exit then
  hu nu - 1+ 0 ?do
    h i + nu  n nu  compare 0= if i true unloop exit then
  loop false ;

\ parse unsigned decimal from (a u): ( value ok? ). ok? false if empty or non-digit.
: parse-uint ( a u -- val ok? )
  { a u }
  u 0= if 0 false exit then
  0   ( acc )
  u 0 ?do
    a i + c@ [char] 0 [char] 9 1+ within 0= if drop 0 false unloop exit then
    10 * a i + c@ [char] 0 - +
  loop true ;

\ ---------------------------------------------------------------------------
\ Request parsing. req-buf holds the full request (headers + body).
\ ---------------------------------------------------------------------------
: trim-lead-sp ( a u -- a' u' )
  begin dup 0> if over c@ bl = else false then while 1 /string repeat ;

\ scan one header line within [a,u); handle a single "name: value" line.
: handle-header ( line-a line-u -- )
  2dup [char] : findc 0= if 2drop exit then   ( line-a line-u cidx )
  { la lu ci }
  la ci                                       ( name-a name-u )
  la ci 1+ +  lu ci 1+ -  trim-lead-sp         ( name-a name-u val-a val-u )
  { va vu }
  2dup s" authorization" ci= if va vu r-auth 2! then
  2dup s" content-length" ci= if va vu parse-uint if r-clen ! else drop then then
  2dup s" connection"     ci= if va vu s" close" ci= if 1 r-close ! then then
  2drop ;

: parse-request ( -- ok? )
  req-buf req-len @   { ra ru }
  0 0 r-auth 2!  0 r-clen !  0 r-close !  0 0 r-body 2!
  0 0 r-method 2!  0 0 r-path 2!
  ra ru s\" \r\n\r\n" findsub 0= if false exit then  { hend }
  ra ru s\" \r\n" findsub 0= if false exit then       { rlend }
  \ method = [0, first space) in request line
  ra rlend 32 findc 0= if false exit then             { sp1 }
  ra sp1 r-method 2!
  \ path = [sp1+1, next space or end-of-request-line)
  ra sp1 1+ +  rlend sp1 1+ -                          ( path-a maxlen )
  2dup 32 findc if nip else then                       ( path-a path-u )
  r-path 2!
  \ headers region = [rlend+2, hend)
  rlend 2 +   { pos }                 \ absolute index of first header line
  begin pos hend < while
    \ find CRLF at or after pos (must exist because hend is CRLFCRLF)
    ra pos +  hend pos -  s\" \r\n" findsub 0= if hend pos - then  ( linelen )
    { llen }
    ra pos +  llen  handle-header
    pos llen + 2 +  to pos
  repeat
  \ body = [hend+4, hend+4+clen) clamped to what we have
  r-clen @ ?dup if                                     ( clen )
    ra hend 4 + +  swap                                ( body-a clen )
    req-len @ hend 4 + - min                           ( body-a avail )
    r-body 2!
  then
  true ;


\ ---------------------------------------------------------------------------
\ Response writing into resp-buf
\ ---------------------------------------------------------------------------
: crlf+ ( -- ) 13 resp-c 10 resp-c ;
: int>resp ( n -- )
  dup 0= if [char] 0 resp-c exit then
  here 64 + { tmp } 0 { cnt }
  begin dup 0> while
    10 /mod swap [char] 0 + tmp cnt + c! cnt 1+ to cnt
  repeat drop
  cnt 0 ?do tmp cnt 1- i - + c@ resp-c loop ;

variable keep-alive?

: send-response ( ct? body-a body-u status-a status-u -- )
  { ct ba bu sa su }
  resp-reset
  s" HTTP/1.1 " resp+ sa su resp+ crlf+
  ct if s" Content-Type: application/json" resp+ crlf+ then
  s" Content-Length: " resp+ bu int>resp crlf+
  keep-alive? @ if
    s" Connection: keep-alive" resp+ crlf+
    \ Advertise a generous idle timeout. SPEC requires holding idle connections
    \ >= 75 s; with shedding off we never proactively close an idle keep-alive,
    \ so this header just reassures clients. 120 s comfortably exceeds the 75 s
    \ floor and mload's ~40 s max idle (think-scale 2.5).
    s" Keep-Alive: timeout=120" resp+ crlf+
  else
    s" Connection: close" resp+ crlf+
  then
  crlf+ ba bu resp+ ;

: reply-json ( status-a status-u -- )
  { sa su } true json-result sa su send-response ;

: reply-error ( msg-a msg-u status-a status-u -- )
  { ma mu sa su } json-reset ma mu json-error sa su reply-json ;

\ ---------------------------------------------------------------------------
\ Status line constants
\ ---------------------------------------------------------------------------
: s200 s" 200 OK" ;
: s201 s" 201 Created" ;
: s400 s" 400 Bad Request" ;
: s401 s" 401 Unauthorized" ;
: s404 s" 404 Not Found" ;
: s500 s" 500 Internal Server Error" ;
: s503 s" 503 Service Unavailable" ;

\ emit a post object (compact) from row <row> of result <res> (cols 0..4).
\ Columns: 0=id(int) 1=body(text) 2=created_at(text) 3=username(text) 4=like_count(int).
\ created_at (col 2) is reformatted from libpq's timestamptz text to the
\ SPEC's YYYY-MM-DDTHH:MM:SS.mmmZ. ca-reformat writes into ca-out, which stays
\ valid through json-post (json-post copies the bytes out immediately).
: emit-post-cols ( res row -- )
  { res row }
  res row 0 pg-col-int                 ( id )
  res row 1 pg-col-text                ( id ba bu )
  res row 2 pg-col-text ca-reformat    ( id ba bu ca cu )
  res row 3 pg-col-text                ( id ba bu ca cu aa au )
  res row 4 pg-col-int                 ( id ba bu ca cu aa au like )
  json-post ;

\ emit {"post":<obj>} from row <row> of result <res>.
: emit-post-wrapped ( res row -- )
  { res row }
  res row 0 pg-col-int
  res row 1 pg-col-text
  res row 2 pg-col-text ca-reformat
  res row 3 pg-col-text
  res row 4 pg-col-int
  json-post-wrapped ;

\ ---- GET /health ----
: do-health ( -- )
  pg-health-ok? if
    json-reset  now-s start-time @ -  json-health-ok
    s200 reply-json
  else
    json-reset
    json-s," {\qstatus\q:\qdegraded\q,\qdb\q:\qunreachable\q,\qerror\q:\qselect failed\q}"
    s503 reply-json
  then ;

\ ---- GET /feed ----
: do-feed ( -- )
  nm-feed 0 pg-exec                    ( res status )
  dup PG-OK <> if drop pg-done
    s" internal server error" s500 reply-error exit then
  drop { res }
  json-reset json-feed-open
  res pg-ntuples { n }
  n 0 ?do
    i 0> if json-feed-sep then
    res i emit-post-cols
  loop
  json-feed-close
  res pg-done
  s200 reply-json ;

\ ---- GET /posts/:id ----  ( id -- )
: do-get-post ( id -- )
  0 pg-set-int                         \ bind $1 = id
  nm-post 1 pg-exec                    ( res status )
  dup PG-OK <> if drop pg-done
    s" internal server error" s500 reply-error exit then
  drop { res }
  res pg-ntuples 0= if
    res pg-done s" post not found" s404 reply-error exit then
  json-reset res 0 emit-post-wrapped
  res pg-done
  s200 reply-json ;

\ ---- POST /posts ----  ( uid name-a name-u -- )
: ws? ( c -- f ) dup bl = swap dup 9 = swap dup 10 = swap dup 12 = swap 13 = or or or or ;
: trim-ws ( a u -- a' u' )
  begin dup 0> if over c@ ws? else false then while 1 /string repeat
  begin dup 0> if 2dup + 1- c@ ws? else false then while 1- repeat ;

create newpost-ca 32 allot  variable newpost-ca-len
2variable saved-name

: do-create-post ( uid name-a name-u -- )
  saved-name 2!  { uid }
  r-body 2@ dup 0= if 2drop s" malformed JSON body" s400 reply-error exit then
  json-parse-body                        ( body-a body-u status )
  dup JSON-MALFORMED = if drop 2drop s" malformed JSON body" s400 reply-error exit then
  dup JSON-NO-BODY   = if drop 2drop s" body is required" s400 reply-error exit then
  drop trim-ws                           ( tbody-a tbody-u )
  dup 0= if 2drop s" body is required" s400 reply-error exit then
  dup 500 > if 2drop s" body must be at most 500 characters" s400 reply-error exit then
  { tb-a tb-u }
  \ INSERT ... RETURNING id, created_at : one round trip, no separate SELECT.
  uid 0 pg-set-int
  tb-a tb-u 1 pg-set-text
  nm-insert 2 pg-exec                    ( res status )
  dup PG-OK <> if drop pg-done
    s" internal server error" s500 reply-error exit then
  drop { res }
  res pg-ntuples 0= if res pg-done
    s" internal server error" s500 reply-error exit then
  res 0 0 pg-col-int { new-id }
  res 0 1 pg-col-text ca-reformat        ( ca-a ca-u )  \ reformatted created_at
  dup newpost-ca-len !  newpost-ca swap move   \ copy out before pg-done
  json-reset
  new-id                                 ( id )
  tb-a tb-u                              ( id ba bu )
  newpost-ca newpost-ca-len @            ( id ba bu ca cu )
  saved-name 2@                          ( id ba bu ca cu aa au )
  0 json-post-wrapped                    \ like_count = 0 for a brand-new post
  res pg-done
  s201 reply-json ;

\ ---- POST /posts/:id/like ----  ( uid id -- )
\ No separate existence check: the likes.post_id foreign key fails with SQLSTATE
\ 23503 when the post doesn't exist, which pg-exec reports as PG-FK -> 404.
\ already_liked comes from PQcmdTuples: 1 row affected = newly inserted (201,
\ already_liked=false); 0 = ON CONFLICT did nothing (200, already_liked=true).
: do-like ( uid id -- )
  { uid id }
  uid 0 pg-set-int
  id  1 pg-set-int
  nm-like 2 pg-exec                      ( res status )
  dup PG-FK = if drop pg-done
    s" post not found" s404 reply-error exit then
  dup PG-OK <> if drop pg-done
    s" internal server error" s500 reply-error exit then
  drop { res }
  res pg-cmd-tuples 1 = { newly }        \ true if a row was actually inserted
  res pg-done
  json-reset  newly 0= id json-like      \ already_liked = not newly
  newly if s201 else s200 then
  reply-json ;

\ ---------------------------------------------------------------------------
\ Auth helper: require a valid Bearer token. On success pushes ( uid name-a name-u true ).
\ On failure sends the proper 401 and pushes ( false ).
\ ---------------------------------------------------------------------------
: require-auth ( -- uid name-a name-u true | false )
  r-auth 2@ dup 0= if 2drop
    s" missing bearer token" s401 reply-error false exit then
  ( auth-a auth-u )
  s" Bearer " starts-with 0= if
    s" missing bearer token" s401 reply-error false exit then
  r-auth 2@ 7 /string                  ( token-a token-u )
  jwt-verify                           ( uid name-a name-u status )
  dup JWT-OK = if drop true exit then
  dup JWT-BAD-PAYLOAD = if drop 2drop drop
    s" invalid token payload" s401 reply-error false exit then
  drop 2drop drop
  s" invalid or expired token" s401 reply-error false ;

\ parse a positive-integer path segment ( a u -- id true | false )
: seg>id ( a u -- id true | false )
  parse-uint dup 0= if exit then       ( id true )
  drop dup 1 < if drop false else true then ;

\ ---------------------------------------------------------------------------
\ Routing. Uses r-method / r-path.
\ Path forms: /health /feed /posts /posts/:id /posts/:id/like
\ ---------------------------------------------------------------------------
: method-is ( a u -- f ) r-method 2@ s= ;

\ handle POST /posts/:id/like  given the id-segment slice ( a u )
: route-like ( idseg-a idseg-u -- )
  { ida idu }
  s" POST" method-is 0= if s" not found" s404 reply-error exit then
  require-auth 0= if exit then          ( uid name-a name-u )
  2drop                                 ( uid )  \ drop name, keep uid
  ida idu seg>id 0= if
    drop s" invalid post id" s400 reply-error exit then  ( uid id )
  do-like ;

\ handle GET /posts/:id  given the id-segment slice ( a u )
: route-get-post ( idseg-a idseg-u -- )
  s" GET" method-is 0= if 2drop s" not found" s404 reply-error exit then
  seg>id 0= if s" invalid post id" s400 reply-error exit then  ( id )
  do-get-post ;

: route ( -- )
  r-path 2@ { pa pu }
  s" GET"  method-is  pa pu s" /health" s= and if do-health exit then
  s" GET"  method-is  pa pu s" /feed"   s= and if do-feed   exit then
  pa pu s" /posts" s= if
    s" POST" method-is if
      require-auth 0= if exit then  do-create-post exit
    then
    s" not found" s404 reply-error exit
  then
  pa pu s" /posts/" starts-with if
    pa 7 + pu 7 - { ra ru }             \ rest after "/posts/"
    ra ru s" /like" findsub dup if      ( idx true )
      drop                              ( idx )
      dup 5 + ru = if                   \ "/like" is a proper suffix
        ra swap route-like exit         ( id-seg: ra idx )
      else
        drop s" invalid post id" s400 reply-error exit
      then
    else drop then
    \ plain GET /posts/:id
    ra ru route-get-post exit
  then
  s" not found" s404 reply-error ;


\ ---------------------------------------------------------------------------
\ The connection layer (accept loop, per-connection read/write, keep-alive) is
\ the single-process epoll event loop in epoll.fs. It reuses the shared words
\ above — parse-request, route, and the resp-buf response builder — so all the
\ request handling lives here and only the I/O driver lives in epoll.fs.
\ server.fs loads this file then epoll.fs and calls epoll-main.
\ ---------------------------------------------------------------------------
