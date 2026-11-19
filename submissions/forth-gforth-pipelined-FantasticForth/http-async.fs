\ ============================================================================
\ http-async.fs - async (pipelined) request handling.
\
\ Each DB route is split:
\   *-submit ( slot -- submitted? )  parse/auth/bind, pgp-submit, park slot in
\       ST-DBWAIT. true = owned by DB engine; false = already replied into
\       resp-buf (error/validation) -> caller finishes the slot now.
\   *-complete ( slot res status -- )  format the result into resp-buf, then
\       finish-slot-response (copies to wbuf, arms write).
\
\ keep-alive decision is made at submit time (as in the sync path) and stashed
\ in slot-keep; completion restores it into keep-alive? before finishing, so an
\ interleaved later submit cannot clobber this request's keep-alive.
\
\ Reuses http.fs (parse-request, require-auth, seg>id, trim-ws, json-*,
\ emit-post-cols/-wrapped, ca-reformat, reply-*, s4xx), pg.fs (pg-set-int/text,
\ pg-col-*), pg-pipe.fs (pgp-submit, PGP-OK/ERR/FK), epoll.fs (slot fields,
\ finish-slot-response, keep-alive?, pend-* scratch).
\ Load order: ffi pg pg-pipe jwt json http epoll http-async  (http-async LAST so
\ it sees slot fields + RT-* from epoll.fs; epoll calls back via deferred words).

\ statement ids (match pg-pipe stmt-name / stmt-nparams)
0 constant STMT-HEALTH
1 constant STMT-FEED
2 constant STMT-POST
3 constant STMT-INSERT
4 constant STMT-LIKE

\ stash the current keep-alive decision for this slot, then park in ST-DBWAIT.
: park-slot ( slot route-id -- )
  over slot-route swap scell !
  keep-alive? @ over slot-keep swap scell !
  ST-DBWAIT swap slot-state swap scell ! ;

\ restore this slot's keep-alive into the global before formatting/finishing.
: restore-keep ( slot -- ) slot-keep swap scell @ keep-alive? ! ;

\ emit a 500 for a slot and finish it
: complete-500 ( slot -- )
  dup restore-keep
  s" internal server error" s500 reply-error
  finish-slot-response ;

\ ======================= GET /feed =======================================
: feed-submit ( slot -- submitted? )
  dup STMT-FEED false pgp-submit if RT-FEED park-slot true
    else drop false then ;

: feed-complete ( slot res status -- )
  { slot res status }
  status PGP-OK <> if res 0<> if res pg-done then slot complete-500 exit then
  slot restore-keep
  json-reset json-feed-open
  res pg-ntuples { n }
  n 0 ?do i 0> if json-feed-sep then res i emit-post-cols loop
  json-feed-close
  res pg-done
  s200 reply-json
  slot finish-slot-response ;

\ ======================= GET /posts/:id ==================================
\ submit expects the post id already parsed; caller does auth-free routing.
: getpost-submit ( slot id -- submitted? )
  0 pg-set-int                              \ bind $1 = id
  dup STMT-POST false pgp-submit if RT-GETPOST park-slot true else drop false then ;

: getpost-complete ( slot res status -- )
  { slot res status }
  status PGP-OK <> if res 0<> if res pg-done then slot complete-500 exit then
  slot restore-keep
  res pg-ntuples 0= if res pg-done
    json-reset s" post not found" json-error s404 reply-json
    slot finish-slot-response exit then
  json-reset res 0 emit-post-wrapped
  res pg-done
  s200 reply-json
  slot finish-slot-response ;

\ ======================= POST /posts (create) ============================
\ caller has already authed (uid, name) and validated the trimmed body.
\ We copy body + author into per-slot scratch so they survive to completion.
: create-submit ( slot uid body-a body-u name-a name-u -- submitted? )
  { slot uid ba bu na nu }
  \ stash state for completion
  uid slot slot-arg0 swap scell !
  bu slot pend-bodyl swap scell !   ba slot pend-body@ bu move
  nu slot pend-namel swap scell !   na slot pend-name@ nu move
  \ bind params: $1 = uid, $2 = body
  uid 0 pg-set-int
  ba bu 1 pg-set-text
  slot STMT-INSERT true pgp-submit if slot RT-CREATE park-slot true else false then ;

: create-complete ( slot res status -- )
  { slot res status }
  status PGP-OK <> if res 0<> if res pg-done then slot complete-500 exit then
  slot restore-keep
  res pg-ntuples 0= if res pg-done slot complete-500 exit then
  res 0 0 pg-col-int { new-id }
  res 0 1 pg-col-text ca-reformat        ( ca-a ca-u )
  \ copy created_at out before pg-done (reuse newpost-ca from http.fs)
  dup newpost-ca-len ! newpost-ca swap move
  json-reset
  new-id
  slot pend-body@ slot pend-bodyl swap scell @
  newpost-ca newpost-ca-len @
  slot pend-name@ slot pend-namel swap scell @
  0 json-post-wrapped                    \ like_count = 0 for a new post
  res pg-done
  s201 reply-json
  slot finish-slot-response ;

\ ======================= POST /posts/:id/like ============================
: like-submit ( slot uid id -- submitted? )
  { slot uid id }
  id slot slot-arg0 swap scell !            \ json-like needs the post id at completion
  uid 0 pg-set-int
  id  1 pg-set-int
  slot STMT-LIKE true pgp-submit if slot RT-LIKE park-slot true else false then ;

: like-complete ( slot res status -- )
  { slot res status }
  status PGP-FK = if res 0<> if res pg-done then
    slot restore-keep
    json-reset s" post not found" json-error s404 reply-json
    slot finish-slot-response exit then
  status PGP-OK <> if res 0<> if res pg-done then slot complete-500 exit then
  slot restore-keep
  res pg-cmd-tuples 1 = { newly }        \ 1 row inserted = newly liked
  res pg-done
  json-reset  newly 0=  slot slot-arg0 swap scell @  json-like
  newly if s201 else s200 then reply-json
  slot finish-slot-response ;

\ ======================= async router ====================================
\ Mirrors http.fs route, but DB routes call *-submit and may park the slot.
\ Returns: true  = slot parked in ST-DBWAIT (DB engine owns it now)
\          false = a full response is already in resp-buf (error / health /
\                  validation / 404 / 400) -> caller finishes the slot now.
\ Requires the current slot# (the request is already parsed into req-buf and
\ r-* globals by process-slot before calling this).
: route-async-impl ( slot -- parked? )
  { slot }
  r-path 2@ { pa pu }
  \ GET /health  -- synchronous (rare); reply now, not parked
  s" GET" method-is  pa pu s" /health" s= and if do-health false exit then
  \ GET /feed
  s" GET" method-is  pa pu s" /feed" s= and if slot feed-submit exit then
  \ /posts (exact) : POST create
  pa pu s" /posts" s= if
    s" POST" method-is 0= if s" not found" s404 reply-error false exit then
    require-auth 0= if false exit then        ( uid name-a name-u )
    { uid na nu }                             \ uid(deep) name-a name-u(top)
    r-body 2@ dup 0= if 2drop
      s" malformed JSON body" s400 reply-error false exit then
    json-parse-body                           ( body-a body-u status )
    dup JSON-MALFORMED = if drop 2drop
      s" malformed JSON body" s400 reply-error false exit then
    dup JSON-NO-BODY = if drop 2drop
      s" body is required" s400 reply-error false exit then
    drop trim-ws                              ( tb-a tb-u )
    dup 0= if 2drop s" body is required" s400 reply-error false exit then
    dup 500 > if 2drop
      s" body must be at most 500 characters" s400 reply-error false exit then
    { ba bu }
    slot uid ba bu na nu create-submit exit
  then
  \ /posts/... : GET /posts/:id  or POST /posts/:id/like
  pa pu s" /posts/" starts-with if
    pa 7 + pu 7 - { ra ru }
    ra ru s" /like" findsub dup if            ( idx true )
      drop                                    ( idx )
      dup 5 + ru = if                         \ "/like" proper suffix
        { seglen }                            \ id segment length = idx
        s" POST" method-is 0= if
          s" not found" s404 reply-error false exit then
        require-auth 0= if false exit then    ( uid name-a name-u )
        2drop { uid }                         \ keep uid
        ra seglen seg>id 0= if drop
          s" invalid post id" s400 reply-error false exit then  ( id )
        { id }
        slot uid id like-submit exit
      else
        drop s" invalid post id" s400 reply-error false exit
      then
    else drop then
    \ GET /posts/:id
    s" GET" method-is 0= if s" not found" s404 reply-error false exit then
    ra ru seg>id 0= if s" invalid post id" s400 reply-error false exit then ( id )
    { id } slot id getpost-submit exit
  then
  s" not found" s404 reply-error false ;

\ ======================= completion dispatch =============================
\ Called by the DB engine (pgp-complete) when a parked slot's result arrives.
: route-complete ( slot res status -- )
  { slot res status }
  slot slot-route swap scell @ { rt }
  rt RT-FEED    = if slot res status feed-complete    exit then
  rt RT-GETPOST = if slot res status getpost-complete exit then
  rt RT-CREATE  = if slot res status create-complete  exit then
  rt RT-LIKE    = if slot res status like-complete    exit then
  \ unknown route: clear result, 500 the slot
  res 0<> if res pg-done then slot complete-500 ;

\ --- bind the deferred words declared in epoll.fs ---
' route-async-impl is route-async
