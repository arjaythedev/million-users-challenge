(** Serving an [httpun] connection from a single Eio fiber, for better
    performances for an API endpoint. *)

val serve :
  ?config:Httpun.Config.t ->
  error_handler:Httpun.Server_connection.error_handler ->
  Httpun.Server_connection.request_handler ->
  _ Eio.Flow.two_way ->
  unit
(** Serves requests on the flow until either side closes, then returns.
    Errors (other than cancellation) end the connection, after httpun has had
    the chance to answer with [error_handler]. Does not close the flow. *)

val listen :
  sw:Eio.Switch.t ->
  net:_ Eio.Net.t ->
  ?backlog:int ->
  ?config:Httpun.Config.t ->
  error_handler:Httpun.Server_connection.error_handler ->
  Eio.Net.Sockaddr.stream ->
  Httpun.Server_connection.request_handler ->
  'a
(** Listens on the address and {!serve}s each connection in its own fiber
    (with [TCP_NODELAY]), closing it afterwards. Never returns. The socket has
    [SO_REUSEPORT]: each domain can listen on the same port with its own
    socket and the kernel will spread the connections. Failed accepts are
    skipped. [backlog] defaults to 65535. *)

val with_body : Httpun.Reqd.t -> (string -> unit) -> unit
(** Reads the whole request body and passes it to the callback. *)
