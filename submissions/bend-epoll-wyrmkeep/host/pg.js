// The JavaScript side of host/pg.bend. The database effect runs on the
// native lane only (docs/specs/S08-pg-effect-and-interpreter.md): the pool
// is C (host/pg.c, its wire client host/pgwire.c). On this lane Pg.open stops
// the run with that message.
function pg_native_only() {
  throw new Error("route A' database effects are native only: build with bench/servers/bend-epoll/build.sh");
}

io_eff(CID(Pg.open), pg_native_only);
