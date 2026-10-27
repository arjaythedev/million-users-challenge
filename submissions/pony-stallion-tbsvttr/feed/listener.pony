use stallion = "stallion"
use "net"

actor Listener is TCPListenerActor
  """
  Accepts connections on one port and gives each one its own `Conn` actor, pinned to one of the Postgres
  connections round-robin.
  """
  var _tcp_listener: TCPListener = TCPListener.none()
  let _server_auth: TCPServerAuth
  let _config: stallion.ServerConfig
  let _ctx: Context
  let _out: OutStream
  let _err: OutStream
  var _accepted: USize = 0

  new create(auth: TCPListenAuth, host: String, port: String, ctx: Context, out: OutStream,
    err: OutStream)
  =>
    _server_auth = TCPServerAuth(auth)
    _ctx = ctx
    _out = out
    _err = err
    _config = stallion.ServerConfig(host, port where
      max_body_size' = 65_536,
      idle_timeout' = _Limits.idle_timeout(),
      read_buffer_size' = _Limits.read_buffer())
    // limit = None: the listener's default caps it at 100,000 connections
    _tcp_listener = TCPListener(auth, host, port, this where limit = None)

  fun ref _listener(): TCPListener => _tcp_listener

  fun ref _on_accept(fd: U32): TCPConnectionActor =>
    _accepted = _accepted + 1
    // Hand each connection only its own DbSlot, not the Context: Pony's GC keeps a record per connection of
    // every actor reachable from what it was given, and the Database reaches all of them (~290 KB a connection).
    Conn(_server_auth, fd, _config, _ctx.secret, _ctx.started, _ctx.db.slot(_accepted))

  fun ref _on_listening() =>
    try
      (let host, let port) = _tcp_listener.local_address().name()?
      _out.print("listening on " + host + ":" + port)
    end

  fun ref _on_listen_failure() =>
    _err.print("could not listen")

primitive _Limits
  fun idle_timeout(): (IdleTimeout | None) =>
    // Users idle up to ~40 s between requests; the spec asks for at least 75 s (the default is 60 s).
    match MakeIdleTimeout(120_000)
    | let t: IdleTimeout => t
    else None
    end

  fun read_buffer(): ReadBufferSize =>
    // Every connection keeps a read buffer of this size for its whole life (16 KB by default). Requests here
    // are under 1 KB; a bigger one is read in more than one piece.
    match MakeReadBufferSize(1024)
    | let r: ReadBufferSize => r
    else DefaultReadBufferSize()
    end
