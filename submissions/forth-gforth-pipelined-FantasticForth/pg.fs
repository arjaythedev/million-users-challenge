\ ============================================================================
\ pg.fs - PostgreSQL data layer via libpq (replaces the $12 entry's db.fs).
\ Depends on ffi.fs being loaded first (PQ* bindings + the tdsc_param_* helpers).
\
\ Phase-A: ONE synchronous libpq connection per process. Each request runs its
\ single SPEC.md statement with PQexecPrepared (blocking) and formats the result.
\ A per-worker connection pool and/or async pipelining is layered on later
\ (phases 2-3); the handler-facing words here hide that so http.fs needn't care.
\
\ Model note: unlike SQLite (one statement cursor, stepped row by row), libpq
\ returns a whole PGresult per execution. The accessor words below therefore
\ take explicit (res row col). Every PGresult MUST be PQclear'd exactly once
\ via pg-done.
\ ============================================================================

\ --- libpq status constants -------------------------------------------------
0  constant CONNECTION_OK          \ ConnStatusType
1  constant PGRES_COMMAND_OK       \ command returning no rows
2  constant PGRES_TUPLES_OK        \ command returning rows (SELECT, RETURNING)
7  constant PGRES_FATAL_ERROR
67 constant PG_DIAG_SQLSTATE       \ 'C' - the SQLSTATE field code

\ Status we hand back to http.fs.
0 constant PG-OK          \ success, result handle valid (rows may be 0)
1 constant PG-ERR         \ fatal error (not a FK violation)
2 constant PG-FK          \ foreign-key violation (SQLSTATE 23503) -> 404

\ --- the connection ---------------------------------------------------------
variable pgconn   0 pgconn !

\ --- C-string helpers (each uses its own buffer so several can coexist) ----
: cstr-len ( zaddr -- u )   \ length of a NUL-terminated C string
  dup 0= if drop 0 exit then
  dup begin dup c@ while 1+ repeat swap - ;

create pg-sqlbuf 8192 allot
: >zsql ( addr u -- zaddr )
  dup 8191 > abort" pg >zsql: string too long"
  pg-sqlbuf swap 2dup + >r move 0 r> c! pg-sqlbuf ;

create pg-namebuf 64 allot
: >zname ( addr u -- zaddr )
  dup 63 > abort" pg stmt name too long"
  pg-namebuf swap 2dup + >r move 0 r> c! pg-namebuf ;

: pg-errmsg ( -- addr u )
  pgconn @ PQerrorMessage dup 0= if drop s" (null)" exit then
  dup cstr-len ;

\ SQLSTATE of an error result as a Forth string ( res -- a u )
: res-sqlstate ( res -- a u )
  PG_DIAG_SQLSTATE PQresultErrorField
  dup 0= if drop s" " exit then
  dup cstr-len ;

\ --- connect ----------------------------------------------------------------
\ Connect using the DATABASE_URL conninfo (libpq accepts the URL form directly).
: pg-connect ( addr u -- )
  >zsql PQconnectdb pgconn !
  pgconn @ 0= abort" PQconnectdb returned NULL"
  pgconn @ PQstatus CONNECTION_OK <> if
    cr ." pg connect: " pg-errmsg type cr
    true abort" postgres connection failed"
  then ;

\ Session setup allowed by rule 2 (session-level SET on our own connection).
\ Force UTC so created_at text always carries a +00 offset and the reformatter
\ sees one shape.
: pg-session-init ( -- )
  pgconn @ s" SET TIME ZONE 'UTC'" >zsql PQexec   ( res )
  dup 0= abort" SET TIME ZONE failed (null result)"
  dup PQresultStatus PGRES_COMMAND_OK <> abort" SET TIME ZONE failed"
  PQclear ;

\ --- prepared statements ----------------------------------------------------
\ Prepared once per connection. SQL is EXACTLY the SPEC.md reference SQL (rule 3).
: pg-prepare ( name-a name-u sql-a sql-u -- )
  >zsql { zsql }
  >zname { zname }
  pgconn @ zname zsql 0 0 PQprepare   ( res )
  dup 0= abort" PQprepare returned NULL"
  dup PQresultStatus PGRES_COMMAND_OK <> if
    cr ." pg prepare: " dup res-sqlstate type space pg-errmsg type cr
    PQclear true abort" PQprepare failed"
  then PQclear ;

\ Statement names (counted strings, reused at exec time).
: nm-feed    s" feed" ;
: nm-post    s" post" ;
: nm-insert  s" insert" ;
: nm-like    s" like" ;
: nm-health  s" health" ;

: pg-prepare-all ( -- )
  nm-health s" SELECT 1" pg-prepare
  nm-feed
    s" SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
    pg-prepare
  nm-post
    s" SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1"
    pg-prepare
  nm-insert
    s" INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
    pg-prepare
  nm-like
    s" INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
    pg-prepare ;

\ --- parameter binding ------------------------------------------------------
\ Two scratch buffers so $1 and $2 can both be set and stay valid until exec.
create pg-p0 8192 allot
create pg-p1 8192 allot
: param-buf ( i -- buf ) 0= if pg-p0 else pg-p1 then ;

\ render signed n as decimal text + NUL into buf, using pictured numeric output
: int>cstr ( n buf -- )
  { buf }
  dup abs 0 <# #s rot sign #>        ( addr u )  \ signed decimal text
  tuck buf swap move                 ( u )        \ copy digits into buf
  0 buf rot + c! ;                   \ NUL terminator at buf+u

: pg-set-int ( n i -- )
  { n i }
  n  i param-buf  int>cstr         \ render n into param buffer i
  i  i param-buf  tdsc_param_set ; \ register pointer for slot i

\ pg-set-text: ( addr u i -- ) copy u bytes into param buffer i, NUL-terminate.
: pg-set-text ( addr u i -- )
  { i }                                        ( addr u )
  dup 8191 > abort" pg param text too long"
  i param-buf { d }                            ( addr u )
  >r d r@ move                                 ( ) ( R: u )  \ copy u bytes src->d
  0 d r> + c!                                  \ NUL-terminate at d+u
  i d tdsc_param_set ;

\ --- execute ----------------------------------------------------------------
\ Execute prepared statement <name> with nParams params already set.
\ Returns ( res status ). Caller pg-done's res when finished (res may be 0 on err).
: pg-exec ( name-a name-u nParams -- res status )
  { np }
  >zname { zn }
  pgconn @ zn np
  np 0= if 0 else tdsc_params_ptr then         ( conn zn np paramValues )
  0 0 0                                         ( ... pLengths pFormats resultFmt=0 )
  PQexecPrepared                                ( res )
  dup 0= if drop 0 PG-ERR exit then
  dup PQresultStatus { st }
  st PGRES_TUPLES_OK  = if PG-OK  exit then
  st PGRES_COMMAND_OK = if PG-OK  exit then
  dup res-sqlstate s" 23503" compare 0= if PG-FK else PG-ERR then ;

: pg-done ( res -- ) dup 0= if drop exit then PQclear ;

\ --- result accessors (res row col) ----------------------------------------
: pg-ntuples ( res -- n ) PQntuples ;

\ Owned storage for column text. gforth 0.7.3 crashes if a raw libpq char* is
\ left on the data stack across later operations (verified), so every accessor
\ stashes the pointer in cv-ptr first and copies bytes into our own memory. For
\ text we rotate through COL-RING buffers because a post object needs 3 text
\ columns live at once (body, created_at, author); copies also stay valid after
\ pg-done.
8 constant COL-RING
512 constant COL-BUFSZ          \ >= max body (500) + slack; created_at/username fit
create col-bufs COL-RING COL-BUFSZ * allot
variable col-next   0 col-next !
variable cv-ptr   variable cv-len
: col-buf@ ( -- addr )          \ next ring buffer, then advance
  col-next @ COL-BUFSZ * col-bufs +
  col-next @ 1+ COL-RING mod col-next ! ;

\ integer column value ( res row col -- n )  (parses libpq text)
: pg-col-int ( res row col -- n )
  PQgetvalue cv-ptr !                  \ stash char* in a var, not on the stack
  cv-ptr @ 0= if 0 exit then
  cv-ptr @                             ( p )
  0 0 { neg acc }                      ( p )
  dup c@ [char] - = if 1+ 1 to neg then
  begin dup c@ dup [char] 0 [char] 9 1+ within while
    [char] 0 - acc 10 * + to acc 1+
  repeat drop                          \ drop the non-digit byte
  drop                                 \ drop the walk pointer p (was leaking!)
  neg if acc negate else acc then ;

\ text column value ( res row col -- addr u ). Copied into an owned ring buffer.
: pg-col-text ( res row col -- addr u )
  { res row col }
  res row col PQgetvalue cv-ptr !        \ stash libpq char* in a var (NOT the stack)
  res row col PQgetlength cv-len !
  cv-ptr @ 0= if 0 0 exit then
  cv-len @ COL-BUFSZ >= if cv-len @ COL-BUFSZ 1- min cv-len ! then
  col-buf@ { dst }
  cv-ptr @ dst cv-len @ move             \ copy out of PGresult into owned buffer
  dst cv-len @ ;

\ text column value ( res row col -- addr u ).
\ We copy the bytes out of the PGresult into one of a small ring of owned
\ rows-affected from an INSERT/UPDATE ("1"/"0"/"") ( res -- n )
: pg-cmd-tuples ( res -- n )
  PQcmdTuples cv-ptr !
  cv-ptr @ 0= if 0 exit then
  cv-ptr @ c@ 0= if 0 exit then
  cv-ptr @ 0 { acc }                   ( p )
  begin dup c@ dup [char] 0 [char] 9 1+ within while
    [char] 0 - acc 10 * + to acc 1+
  repeat drop                          \ drop non-digit byte
  drop                                 \ drop walk pointer p
  acc ;

\ --- created_at reformatting ------------------------------------------------
\ libpq (text, UTC session) returns e.g. "2025-12-31 23:59:19.409123+00" or
\ "2025-12-31 23:59:19+00" (no fraction). SPEC wants, truncated to ms:
\   YYYY-MM-DDTHH:MM:SS.mmmZ   (24 bytes, UTC, 'Z').
create ca-out 32 allot
: ca-reformat ( src-a src-u -- out-a out-u )
  { sa su }
  su 19 < if sa su exit then         \ unexpected shape: pass through (safe)
  ca-out { o }
  sa o 10 move                       \ YYYY-MM-DD -> out[0..9]
  [char] T o 10 + c!
  sa 11 + o 11 + 8 move              \ HH:MM:SS (src space at idx 10) -> out[11..18]
  [char] . o 19 + c!
  sa 19 + c@ [char] . = if
    3 0 ?do                          \ up to 3 fraction digits, zero-padded
      su 20 i + > if
        sa 20 i + + c@ dup [char] 0 [char] 9 1+ within
        if o 20 i + + c! else drop [char] 0 o 20 i + + c! then
      else [char] 0 o 20 i + + c! then
    loop
  else
    [char] 0 o 20 + c! [char] 0 o 21 + c! [char] 0 o 22 + c!
  then
  [char] Z o 23 + c!
  o 24 ;

\ --- health -----------------------------------------------------------------
: pg-health-ok? ( -- f )
  nm-health 0 pg-exec                ( res status )
  PG-OK = swap pg-done ;             \ ( f )  -- pg-done the result, leave flag
