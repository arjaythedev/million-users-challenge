\ ============================================================================
\ epoll.fs - single-process epoll event loop: the connection/I/O driver.
\ Reuses http.fs's parse-request / route / resp-buf response builder unchanged:
\ those operate on req-buf and build into resp-buf, so the event loop copies a
\ connection's bytes into req-buf before routing and copies resp-buf out into
\ the connection's write buffer. Adds non-blocking sockets, a fixed connection
\ slot pool, accept backpressure, connection-pressure shedding, group commit and
\ off-path WAL checkpointing.
\ Load order: ffi.fs db.fs jwt.fs json.fs http.fs epoll.fs
\ ============================================================================

\ --- epoll op + event constants (match ffi.fs C helper) ---
1 constant EP-ADD
2 constant EP-DEL
3 constant EP-MOD
1 constant EPOLLIN
4 constant EPOLLOUT
$2000 constant EPOLLRDHUP   \ not used directly but documented
$008 constant EPOLLERR
$010 constant EPOLLHUP

11 constant EAGAIN-ERR       \ EAGAIN/EWOULDBLOCK on Linux x86-64

\ --- connection slot pool ---
\ MAX-SLOTS bounds concurrent connections. In DIRECT mode each keep-alive client
\ is one connection, so this is the real concurrency ceiling (the box grants
\ LimitNOFILE=65535). Measured cost is ~8 KB RSS per live connection (buffers
\ are lazily faulted); at ~17k live connections the whole process is ~220 MB
\ --- connection slot pool (per worker process) ---------------------------
\ At a million keep-alive connections split across WORKERS processes, each
\ worker holds ~1,000,000/WORKERS connections. With the default 16 workers that
\ is ~62.5k/worker; MAX-SLOTS is sized to 80k so a worker can hold its share
\ plus headroom (and so 12-16 workers all fit). Each live connection is one slot
\ + one fd.
\
\ Memory: the six parallel slot arrays are MAX-SLOTS cells each = 6*80000*8 =
\ ~3.84 MB/worker of dictionary space (always committed), plus the free-stack
\ (~0.64 MB) and the fd->slot map (below). The big cost is the per-connection
\ read+write buffers, reserved as ONE allocate() of MAX-SLOTS*(RBUF+WBUF) =
\ 80000*(4K+8K) = ~960 MB of ADDRESS SPACE per worker, but lazily faulted: only
\ the pages of actually-live connections are committed. Requests here are tiny
\ (a GET is ~100 bytes) and the largest response is /feed at ~5 KB, so 4 KB read
\ + 8 KB write per connection is ample. Measured ~8 KB RSS per live connection
\ in the SQLite entry; at ~8 KB * 62.5k * 16 ~= 8 GB that fits the 32 GB box with
\ room for kernel TCP socket memory (the real consumer at a million sockets).
80000 constant MAX-SLOTS
4096  constant RBUF-SZ        \ read buffer per connection (requests are ~100 B; cap REQ-MAX=4096)
8192  constant WBUF-SZ        \ write buffer per connection (largest response /feed ~5 KB)

\ slot fields (per slot), stored in parallel arrays indexed by slot#
create slot-fd      MAX-SLOTS cells allot    \ fd owned by this slot (-1 if free)
create slot-state   MAX-SLOTS cells allot    \ 0=free 1=reading 2=writing
create slot-rlen    MAX-SLOTS cells allot    \ bytes accumulated in read buffer
create slot-wlen    MAX-SLOTS cells allot    \ total bytes to write
create slot-woff    MAX-SLOTS cells allot    \ bytes already written
create slot-keep    MAX-SLOTS cells allot    \ keep-alive after this response?
\ large buffers live on the heap (too big for the dictionary)
variable rbuf-base   variable wbuf-base
: alloc-bufs ( -- )
  MAX-SLOTS RBUF-SZ * allocate throw rbuf-base !
  MAX-SLOTS WBUF-SZ * allocate throw wbuf-base ! ;

0 constant ST-FREE
1 constant ST-READING
2 constant ST-WRITING

: slot-rbuf@ ( slot -- addr ) RBUF-SZ * rbuf-base @ + ;
: slot-wbuf@ ( slot -- addr ) WBUF-SZ * wbuf-base @ + ;
: scell ( base slot -- addr ) cells + ;

\ --- free-list stack: O(1) alloc/free instead of an O(MAX-SLOTS) scan per accept
create free-stack  MAX-SLOTS cells allot
variable free-top                  \ number of free slots currently on the stack
: free-push ( slot -- ) free-top @ free-stack swap cells + !  1 free-top +! ;
: free-pop  ( -- slot | -1 )
  free-top @ 0= if -1 exit then
  -1 free-top +!  free-top @ free-stack swap cells + @ ;

\ fd -> slot index map. Each WORKER is its own process with its own fd table and
\ accepts only its share of connections (~1M/WORKERS ~= 62.5k at 16 workers), and
\ the kernel hands out the lowest free fd, so a worker's fds stay roughly in
\ [0, its open-conn count). 131072 covers a worker holding ~128k connections with
\ margin (2x the 16-worker share) at 1 MB/worker for the map. Every access is
\ GUARDed: an out-of-range fd is never used to index this array (closing it
\ instead), so an unexpectedly high fd degrades to a dropped connection, never an
\ out-of-bounds write.
131072 constant MAX-FDMAP
create fd->slot MAX-FDMAP cells allot
: fd-ok? ( fd -- f ) dup 0>= swap MAX-FDMAP < and ;
: fdslot! ( slot fd -- ) dup fd-ok? if cells fd->slot + ! else 2drop then ;
: fdslot@ ( fd -- slot ) dup fd-ok? if cells fd->slot + @ else drop -1 then ;

variable epfd
variable lfd
variable lfd2                      \ second listen fd (EXTRA_PORT); -1 if unused
variable listen-armed?             \ 1 if the listen fd is registered for EPOLLIN

: init-slots ( -- )
  0 free-top !
  -1 lfd2 !
  \ push slots in reverse so slot 0 is handed out first (nicer for debugging)
  MAX-SLOTS 0 ?do -1 slot-fd i scell ! ST-FREE slot-state i scell ! loop
  MAX-SLOTS 0 ?do MAX-SLOTS 1- i - free-push loop
  MAX-FDMAP 0 ?do -1 fd->slot i cells + ! loop ;

\ pop a free slot; -1 if none
: alloc-slot ( -- slot | -1 ) free-pop ;

\ --- connection-pressure shedding ---------------------------------------
\ Open connections = MAX-SLOTS - free-top (O(1), no syscalls). In direct mode
\ each keep-alive client holds a slot (and an fd). To pass the pool/fd ceiling
\ honestly we shed connections under pressure by answering with Connection:close
\ --- connection-pressure shedding (OPT-IN, default OFF) ------------------
\ In the $12 challenge (fd limit 65535, one process) we shed connections under
\ pool pressure by answering Connection: close so clients reconnect. For the
\ million-user challenge this default is WRONG: the box grants LimitNOFILE=2M and
\ the whole point is to HOLD ~1M idle keep-alive connections, not cycle them. At
\ a million clients, forcing reconnects triggers dial/handshake storms through
\ the single-RX-queue NIC and costs far more than holding an idle slot (~8 KB).
\ So shedding is OFF by default: every connection stays keep-alive, and the only
\ backpressure is accept-pause when the pool is genuinely full (handled in
\ on-accept / open-slot). It can be re-enabled with SHED=1 for experiments.
\ Thresholds (used only when enabled) as fractions of the pool:
MAX-SLOTS 85 100 */ constant SHED-SOFT
MAX-SLOTS 97 100 */ constant SHED-HARD
MAX-SLOTS 70 100 */ constant SHED-CLEAR
variable shedding?        0 shedding? !    \ hysteresis latch
variable shed-enabled?    0 shed-enabled? ! \ set from env SHED at startup

: shed-config ( -- )   \ called at worker-init: enable shedding only if SHED=1
  s" SHED" getenv dup if s" 1" compare 0= if 1 else 0 then
  else 2drop 0 then  shed-enabled? ! ;

: open-conns ( -- n ) MAX-SLOTS free-top @ - ;

\ update the hysteresis latch from the current open-conn count (no-op if off).
: shed-update ( -- )
  shed-enabled? @ 0= if exit then
  open-conns { oc }
  shedding? @ if
    oc SHED-CLEAR <= if 0 shedding? ! then
  else
    oc SHED-SOFT >= if 1 shedding? ! then
  then ;

\ should THIS finishing connection be kept alive? false => Connection: close.
\ With shedding off (default) this is always true: we hold the connection.
: keep-this-conn? ( -- f )
  shed-enabled? @ 0= if true exit then
  open-conns SHED-HARD >= if false exit then
  shedding? @ 0= ;

: slot-reset-read ( slot -- )
  0 over slot-rlen swap scell !
  ST-READING over slot-state swap scell !
  drop ;

\ --- accept backpressure -------------------------------------------------
\ When the slot pool is exhausted we must NOT keep accepting and immediately
\ closing: with level-triggered EPOLLIN on the listen fd and eager clients that
\ reconnect instantly, that becomes a 100% CPU accept/close storm. Instead we
\ disarm the listen fd until a slot frees, then re-arm. In practice connection
\ shedding (Connection: close under pressure) keeps us off this path, but it is
\ the backstop if the pool fills anyway.
: accept-pause ( -- )
  listen-armed? @ if
    epfd @ EP-MOD lfd @ 0 tdsc_epoll_ctl drop   \ interest=0: stop reporting readability
    0 listen-armed? !
  then ;
: accept-resume ( -- )
  listen-armed? @ 0= if
    epfd @ EP-MOD lfd @ EPOLLIN tdsc_epoll_ctl drop
    1 listen-armed? !
  then ;

: open-slot ( fd -- )
  { fd }
  \ Guard: an fd outside the map range cannot be tracked safely -> close it.
  fd fd-ok? 0= if fd tdsc_close drop exit then
  alloc-slot dup 0< if
    \ pool full: close THIS fd and pause accepting until a slot frees.
    drop fd tdsc_close drop accept-pause exit
  then                                                   ( slot )
  { slot }
  fd slot-fd slot scell !          \ slot-fd[slot]=fd
  slot fd fdslot!                  \ fd->slot[fd]=slot
  slot slot-reset-read
  epfd @ EP-ADD fd EPOLLIN tdsc_epoll_ctl drop ;  \ register for readability

: close-slot ( slot -- )
  { slot }
  \ ignore double-close of an already-free slot
  slot-state slot scell @ ST-FREE = if exit then
  slot-fd slot scell @  { fd }
  fd 0>= if
    epfd @ EP-DEL fd 0 tdsc_epoll_ctl drop
    fd tdsc_close drop
    -1 fd fdslot!
  then
  -1 slot-fd slot scell !
  ST-FREE slot-state slot scell !
  slot free-push                   \ return slot to the free list
  accept-resume ;                  \ a slot is available again -> resume accepting

\ ---------------------------------------------------------------------------
\ Request completeness: copy the slot's bytes into req-buf, run parse-request,
\ and check that headers + Content-Length body have all arrived. parse-request
\ sets r-clen; we locate the header end to know the body offset.
\ ---------------------------------------------------------------------------
: request-complete? ( slot -- f )
  { slot }
  slot slot-rbuf@  req-buf  slot-rlen slot scell @  dup req-len !  move
  req-buf req-len @ s\" \r\n\r\n" findsub  ( idx true | false )
  if                                       ( hend )
    parse-request if                       ( hend )
      4 + r-clen @ +  req-len @ <=
    else drop false then
  else false then ;

\ ---------------------------------------------------------------------------
\ Write durability (event-loop level)
\ ---------------------------------------------------------------------------
\ With the libpq data layer each write runs as a synchronous PQexecPrepared in
\ autocommit, and Postgres is configured with synchronous_commit = on (rule 8),
\ so the INSERT is durably committed by the time the handler returns and buffers
\ its 201 response. There is therefore no end-of-pass batch to commit and no WAL
\ to checkpoint (that was the SQLite design). finalize-batch is kept as a no-op
\ so the event loop's structure is unchanged; the write-batching/pipelining
\ optimization is a later phase (and would re-introduce a real finalize step).
: finalize-batch ( -- ) ;

\ copy slot read buffer into the global req-buf and run the pipeline,
\ leaving the response bytes in resp-buf (resp-len). Then copy into slot wbuf.
: process-slot ( slot -- )
  { slot }
  \ copy request bytes -> req-buf
  slot slot-rbuf@  req-buf  slot-rlen slot scell @  dup req-len !  move
  parse-request 0= if
    \ unparseable: emit 400 and close
    0 keep-alive? !
    s" malformed JSON body" s400 reply-error
  else
    \ keep-alive unless the client said close OR we're shedding under connection
    \ pressure (then answer Connection: close so this client cycles out). Decided
    \ per connection, as its request completes, with hysteresis (see shed-update).
    1 keep-alive? !
    r-close @ if 0 keep-alive? ! then
    keep-this-conn? 0= if 0 keep-alive? ! then
    route
  then
  \ copy resp-buf -> slot wbuf
  resp-buf  slot slot-wbuf@  resp-len @  dup slot-wlen slot scell !  move
  0 slot-woff slot scell !
  keep-alive? @  slot-keep slot scell !
  ST-WRITING slot-state slot scell ! ;

\ arm epoll interest for a slot according to its state
: arm ( slot -- )
  { slot }
  slot-fd slot scell @ { fd }
  slot-state slot scell @ ST-WRITING = if
    epfd @ EP-MOD fd EPOLLOUT tdsc_epoll_ctl drop
  else
    epfd @ EP-MOD fd EPOLLIN tdsc_epoll_ctl drop
  then ;

\ ---------------------------------------------------------------------------
\ Event handlers
\ ---------------------------------------------------------------------------
: on-readable ( slot -- )
  { slot }
  begin
    slot slot-rbuf@  slot-rlen slot scell @ +      ( dst )
    RBUF-SZ  slot-rlen slot scell @ -            ( dst space )
    dup 0<= if drop slot close-slot exit then           \ buffer full, drop
    slot-fd slot scell @ -rot tdsc_recv_nb              ( n )
    dup 0 = if drop slot close-slot exit then           \ EOF
    dup 0< if drop tdsc_errno EAGAIN-ERR = if exit then slot close-slot exit then
    slot-rlen slot scell +!                        \ rlen += n
    slot request-complete? if
      slot process-slot
      slot arm
      exit
    then
  again ;

: on-writable ( slot -- )
  { slot }
  begin
    slot-wlen slot scell @  slot-woff slot scell @ -  ( remaining )
    dup 0<= if drop                                     \ fully written
      slot-keep slot scell @ if
        slot slot-reset-read  slot arm
      else slot close-slot then
      exit
    then
    ( remaining )
    slot slot-wbuf@  slot-woff slot scell @ +      ( remaining src )
    swap                                                ( src remaining )
    slot-fd slot scell @ -rot tdsc_send_nb              ( n )
    dup 0< if drop tdsc_errno EAGAIN-ERR = if exit then slot close-slot exit then
    dup 0 = if drop slot close-slot exit then
    slot-woff slot scell +!
  again ;

\ accept all pending connections on a listen socket (edge-safe: loop until
\ EAGAIN, OR until the pool fills and open-slot pauses accepting).
\ Takes the listen fd so the same word serves both PORT and EXTRA_PORT.
: on-accept ( lfd -- )
  { lsock }
  begin
    lsock tdsc_accept_nb                      ( fd )
    dup 0< if                                 \ accept failed
      drop
      tdsc_errno EAGAIN-ERR = if exit then    \ EAGAIN: drained, just stop
      accept-pause exit
    then
    dup tdsc_set_nodelay drop
    open-slot                                 \ registers (or closes + pauses if full)
    listen-armed? @ 0= if exit then           \ pool full: stop; backlog holds the rest
  again ;

\ ---------------------------------------------------------------------------
\ Event loop
\ ---------------------------------------------------------------------------
: dispatch-event ( i -- )
  { i }
  i tdsc_ev_fd   { fd }
  i tdsc_ev_ev   { ev }
  fd fdslot@     { slot }
  fd lfd  @ = if lfd  @ on-accept exit then
  fd lfd2 @ = if lfd2 @ on-accept exit then
  slot 0< if exit then
  ev EPOLLERR EPOLLHUP or and if slot close-slot exit then
  ev EPOLLIN  and if slot-state slot scell @ ST-READING = if slot on-readable then then
  fd fdslot@ 0< if exit then          \ slot may have closed during on-readable
  ev EPOLLOUT and if slot-state slot scell @ ST-WRITING = if slot on-writable then then ;

: event-loop ( -- )
  begin
    epfd @ -1 tdsc_epoll_wait        ( n )
    dup 0> if
      0 ?do i dispatch-event loop
      \ end of pass: commit this batch's writes (durably, before their
      \ responses are sent next pass) and checkpoint off the request path.
      finalize-batch
      \ refresh the connection-pressure shedding latch (O(1)).
      shed-update
    else drop then
  again ;

\ ---------------------------------------------------------------------------
\ Per-worker initialization. Runs AFTER fork in each worker process, so every
\ worker gets its OWN Postgres connection, its OWN epoll instance, and its OWN
\ SO_REUSEPORT listen sockets on both ports. A libpq connection and an epoll fd
\ must not be shared across fork, hence all of this is post-fork and per-worker.
\ ---------------------------------------------------------------------------
: worker-init ( -- )
  shed-config
  \ --- Postgres (libpq) connection for THIS worker ---
  s" DATABASE_URL" getenv dup 0= abort" DATABASE_URL not set"
  pg-connect
  pg-session-init
  pg-prepare-all
  alloc-bufs
  init-slots
  tdsc_epoll_create dup 0< abort" epoll_create failed" epfd !
  \ --- listener on PORT (SO_REUSEPORT: kernel load-balances across workers) ---
  host>z the-port @ 4096 tdsc_listen
  dup 0< abort" listen failed (PORT)"  dup lfd !
  tdsc_set_nonblock drop
  epfd @ EP-ADD lfd @ EPOLLIN tdsc_epoll_ctl drop
  1 listen-armed? !
  \ --- listener on EXTRA_PORT ---
  the-extra-port @ 0> if
    host>z the-extra-port @ 4096 tdsc_listen
    dup 0< abort" listen failed (EXTRA_PORT)"  dup lfd2 !
    tdsc_set_nonblock drop
    epfd @ EP-ADD lfd2 @ EPOLLIN tdsc_epoll_ctl drop
  then
  cr ." forth-gforth worker pid=" tdsc_getpid .
  ." on " host-buf host-len @ type
  [char] : emit the-port @ .
  the-extra-port @ 0> if ."  and :" the-extra-port @ . then cr ;

\ run one worker: initialize then loop forever.
: run-worker ( -- ) worker-init event-loop ;

\ ---------------------------------------------------------------------------
\ Multi-process bootstrap. config-load (shared) decides WORKERS.
\
\ WORKERS=1: this process is the single worker (no fork) -- simplest, used by
\ the test harness.
\
\ WORKERS>1: the main process becomes a thin SUPERVISOR. It forks WORKERS child
\ workers (each opens its own Postgres connection + its own SO_REUSEPORT
\ listeners on both ports, so the kernel load-balances connections across them),
\ then blocks in wait(). If any worker exits, the supervisor exits the whole
\ process group too: rule 10 says a crashed server is not restarted, so we fail
\ loud rather than keep serving with fewer workers than we reported.
\ ---------------------------------------------------------------------------
\ Worker pid table. The supervisor records the pid of each worker it forks so
\ that, on shutdown, it can SIGTERM exactly those pids. We must NOT use
\ kill(0, SIGTERM): that signals the whole process group, which under
\ test/run.sh also contains the test harness -- the supervisor would kill the
\ harness (exit 143) even on a fully passing run. Signaling explicit worker
\ pids is correct under both run.sh and systemd, and only runs at shutdown so
\ it has no effect on serving performance.
1024 constant MAX-WORKERS             \ generous cap; real runs use nproc (<=16)
create worker-pids MAX-WORKERS cells allot
variable worker-count  0 worker-count !

: record-worker ( pid -- )
  worker-count @ MAX-WORKERS < if
    worker-pids worker-count @ cells + !
    1 worker-count +!
  else drop then ;

: term-workers ( -- )  \ SIGTERM each forked worker pid (never the group)
  worker-count @ 0 ?do
    worker-pids i cells + @ tdsc_kill drop
  loop ;

: epoll-main ( -- )
  tdsc_ignore_sigpipe
  config-load
  the-workers @ 1 = if run-worker exit then      \ single process: just serve
  cr ." forth-gforth: supervisor pid=" tdsc_getpid .
  ." forking " the-workers @ . ." workers" cr
  the-workers @ 0 ?do
    tdsc_fork { pid }
    pid 0= if run-worker bye then                \ child worker: never returns
    pid 0< if cr ." fork failed for worker " i . cr
    else pid record-worker then                  \ supervisor: remember child pid
  loop
  \ supervisor: wait for any worker to exit, then bring the others down by
  \ signaling the specific worker pids (NOT the process group).
  tdsc_wait { dead }
  cr ." worker pid=" dead . ." exited; supervisor terminating all workers" cr
  term-workers           \ SIGTERM each surviving worker we forked
  bye ;
