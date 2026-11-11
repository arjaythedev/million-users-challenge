\ server.fs - load the layers in order and start the epoll event loop.
\ Load order (pipelined prototype):
\   ffi      C bindings (incl. pipeline PQ* bindings)
\   pg       blocking libpq data layer (kept for /health + param/accessor words)
\   pg-pipe  pooled, pipelined, non-blocking engine (feed/get/create/like)
\   jwt json formatting/auth helpers
\   http     synchronous handlers + shared helpers (reused by http-async)
\   epoll    I/O driver: slots, event loop, PG-fd dispatch, worker-init
\   http-async  async submit/complete split + router (needs epoll slot fields)
\ Then glue: point the engine's deferred completion + fd-register hooks at the
\ event loop's words, and start.
require ffi.fs
require pg.fs
require pg-pipe.fs
require jwt.fs
require json.fs
require http.fs
require epoll.fs
require http-async.fs

\ --- glue: wire the pipelined engine's callbacks to the event loop ---
' route-complete   is pgp-complete            \ DB result ready -> format+finish slot
' pg-fd-register   is pgp-epoll-register-fn   \ register a PGsocket in epoll

epoll-main
bye
