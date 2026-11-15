// The JavaScript side of host/epoll.bend. Route A′ runs on the native lane
// only (docs/specs/S08-pg-effect-and-interpreter.md): the reactor is epoll in
// C (host/epoll.c), which bun:ffi does not offer. On this lane every effect
// stops the run with that message.
function epoll_native_only() {
  throw new Error("route A' effects are native only: build with bench/servers/bend-epoll/build.sh");
}

io_eff(CID(Epoll.open), epoll_native_only);
io_eff(CID(Epoll.turn), epoll_native_only);
io_eff(CID(Epoll.close), epoll_native_only);
