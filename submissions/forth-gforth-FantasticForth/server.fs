\ server.fs - load the layers in order and start the epoll event loop.
\ Load order: ffi (C bindings) -> pg (libpq data layer) -> jwt -> json -> http
\ (handlers/routing) -> epoll (I/O driver). epoll-main (in epoll.fs) runs the
\ bootstrap (config-load, pg-connect via DATABASE_URL, pg-session-init,
\ pg-prepare-all, listeners on PORT and EXTRA_PORT) and then the event loop.
require ffi.fs
require pg.fs
require jwt.fs
require json.fs
require http.fs
require epoll.fs
epoll-main
bye
