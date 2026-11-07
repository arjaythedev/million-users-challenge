(** A small Postgres client for Eio speaking the v3 wire protocol directly.

    Built for few syscalls per query: every query is Bind/Execute on a
    statement prepared at connect time and is pipelined (sent without waiting
    for earlier answers); a writer fiber per connection flushes a batch of
    queries in one [write], a reader fiber completes every answer it can from
    one [read]. A connection belongs to the domain that created it: no locks.

    Consecutive reads in a batch share one Sync, so one implicit transaction:
    an error in one of them also fails the reads after it in that batch
    (with [Closed]). Each write has its own Sync, its own transaction, and
    is answered once committed.

    Text format both ways, without NULL: parameters are never NULL, and a
    NULL column fails its query with [Unsupported]. Auth: trust/peer,
    cleartext, md5, SCRAM-SHA-256. Lost connections fail their in-flight
    queries and reconnect every second in the background (the host name is
    looked up on every attempt). *)

type error =
  | Server of { code : string; message : string }  (** ErrorResponse, with its SQLSTATE *)
  | Closed of string  (** connection down, or lost with the query in flight *)
  | Unsupported of string  (** an answer this client can't represent *)

val show_error : error -> string

val foreign_key_violation : string
(** The SQLSTATE of a [Server] error for a violated foreign key, ["23503"]. *)

type result = {
  rows : string array list;  (** in order *)
  tag : string;  (** CommandComplete, e.g. ["INSERT 0 1"] *)
}

val affected : result -> int
(** The row count of the command tag. *)

type statement

val statement : ?write:bool -> string -> statement
(** A statement, to pass to {!create_pool} so that it is prepared on every
    connection when it opens. [~write:true] (writes, which wait for the
    commit) runs on the pool's write connections, so reads never queue behind
    a commit. *)

type pool
(** Pipelined connections for one domain, reads and writes apart. Queries
    fill one connection's batch, sent once it holds [batch_bytes] or
    [batch_wait] after its first query; consecutive reads share one Sync, so
    one round trip and one implicit transaction. The next batch goes to the
    next idle connection (nothing in flight), round-robin. When all existing
    connections are in use, the group opens one more (one at a time) and
    meanwhile uses the least loaded. *)

type config = {
  uri : Uri.t;
      (** [postgres://user:password@host:port/database] (a [host] starting
          with [/], or none, is a unix socket directory; [PGUSER] and
          [PGPASSWORD] fill in what it lacks) *)
  statements : statement list;
      (** prepared on every connection (a statement listed twice, once) *)
  size : int;  (** read connections at start *)
  max : int;  (** read connections at most (at least [size]) *)
  write_size : int;  (** write connections at start ([0]: writes share the read connections) *)
  write_max : int;  (** write connections at most (at least [write_size]) *)
  batch_bytes : int;  (** a batch is sent once it holds this many bytes (e.g. 1400) *)
  batch_wait : float;
      (** or this many seconds after its first query (e.g. 0.002; [0.]: at once) *)
}

val create_pool :
  sw:Eio.Switch.t ->
  net:[ `Generic | `Unix ] Eio.Net.ty Eio.Resource.t ->
  clock:_ Eio.Time.Mono.t ->
  config ->
  pool
(** Starts opening the connections in the background, retrying every second
    (failures are logged to stderr): queries fail with [Closed] until a
    connection is up. Background fibers live on [sw], which must outlive the
    pool. *)

val query : pool -> statement -> string array -> (result, error) Stdlib.result
(** Runs the statement with these parameters in the group's current batch
    (see {!pool}) and waits for its answer. A statement the pool wasn't
    created with fails with [Server] (SQLSTATE 26000). *)
