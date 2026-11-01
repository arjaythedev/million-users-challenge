type error =
  | Server of { code : string; message : string }
  | Closed of string
  | Unsupported of string

let show_error = function
  | Server { code; message } -> Printf.sprintf "%s (SQLSTATE %s)" message code
  | Closed msg -> "connection: " ^ msg
  | Unsupported msg -> msg

let foreign_key_violation = "23503"

(* The connection can't go on (or can't be opened). *)
exception Fatal of string

type result = {
  rows : string array list;
  tag : string;
}

let affected r =
  String.take_last_while (( <> ) ' ') r.tag
  |> int_of_string_opt
  |> Option.value ~default:0

type statement = { name : string; sql : string; write : bool }

(* Named after its SQL (short: the name is in every Bind): equal SQL, one
   prepared statement. *)
let statement ?(write = false) sql =
  { name = "s" ^ String.sub (Digest.to_hex (Digest.string sql)) 0 8; sql; write }

module Out = struct
  type t = { mutable b : bytes; mutable len : int }

  let create n = { b = Bytes.create n; len = 0 }

  let ensure t n =
    if t.len + n > Bytes.length t.b then begin
      let b = Bytes.create (max (2 * Bytes.length t.b) (t.len + n)) in
      Bytes.blit t.b 0 b 0 t.len;
      t.b <- b
    end

  let char t c =
    ensure t 1;
    Bytes.unsafe_set t.b t.len c;
    t.len <- t.len + 1

  let int16 t n =
    ensure t 2;
    Bytes.set_uint16_be t.b t.len n;
    t.len <- t.len + 2

  let int32 t n =
    ensure t 4;
    Bytes.set_int32_be t.b t.len (Int32.of_int n);
    t.len <- t.len + 4

  let string t s =
    let n = String.length s in
    ensure t n;
    Bytes.blit_string s 0 t.b t.len n;
    t.len <- t.len + n

  let cstring t s = string t s; char t '\000'

  let start t c =
    char t c;
    let at = t.len in
    int32 t 0;
    at

  let finish t at = Bytes.set_int32_be t.b at (Int32.of_int (t.len - at))
end

module IO = struct
  type socket = [ `Generic | `Unix ] Eio.Net.stream_socket_ty Eio.Resource.t
  type fd = { flow : socket; mutable rd : Cstruct.t; mutable wr : Cstruct.t }

  let scratch cs len =
    if Cstruct.length cs >= len then cs else Cstruct.create (max len (2 * Cstruct.length cs))

  let read fd b off len =
    fd.rd <- scratch fd.rd len;
    let cs = Cstruct.sub fd.rd 0 len in
    match Eio.Flow.single_read fd.flow cs with
    | n -> Cstruct.blit_to_bytes cs 0 b off n; n
    | exception End_of_file -> 0

  let write fd b off len =
    fd.wr <- scratch fd.wr len;
    Cstruct.blit_from_bytes b off fd.wr 0 len;
    Eio.Flow.write fd.flow [ Cstruct.sub fd.wr 0 len ]

  let close fd = Eio.Resource.close fd.flow
  let shutdown fd = try Eio.Flow.shutdown fd.flow `All with _ -> ()

  type 'a ivar = 'a Eio.Promise.t * 'a Eio.Promise.u

  let ivar () = Eio.Promise.create ()
  let fill (_, u) v = ignore (Eio.Promise.try_resolve u v)
  let await (p, _) = Eio.Promise.await p
  let is_cancellation = function Eio.Cancel.Cancelled _ -> true | _ -> false
end

type runtime = {
  connect : Unix.sockaddr -> IO.fd;
  spawn : (unit -> unit) -> unit;
  sleep : float -> unit;
}

type conn_config = {
  addr : unit -> Unix.sockaddr;  (* resolved on every connection attempt *)
  user : string;
  password : string option;
  database : string;
  statements : statement list;
}

let config_of_uri uri statements =
  let q k = Uri.get_query_param uri k in
  let user =
    match Uri.user uri with
    | Some u -> u
    | None -> (try Sys.getenv "PGUSER" with Not_found -> (Unix.getpwuid (Unix.getuid ())).pw_name)
  in
  let password =
    match Uri.password uri with Some p -> Some p | None -> Sys.getenv_opt "PGPASSWORD"
  in
  let database =
    match String.drop_first_while (( = ) '/') (String.trim (Uri.path uri)) with
    | "" -> user
    | db -> db
  in
  let port = Option.value (Uri.port uri) ~default:5432 in
  let host =
    match q "host", Uri.host uri with
    | Some h, _ -> h
    | None, (Some h) when h <> "" -> h
    | _ -> "/var/run/postgresql"
  in
  (* A name is looked up when connecting, not here: a failed lookup is then
     an ordinary connection failure, retried, rather than a crash at start. *)
  let addr () =
    if String.length host > 0 && host.[0] = '/' then
      Unix.ADDR_UNIX (Printf.sprintf "%s/.s.PGSQL.%d" host port)
    else
      let a =
        try Unix.inet_addr_of_string host
        with Failure _ -> (
          match Unix.gethostbyname host with
          | h -> h.Unix.h_addr_list.(0)
          | exception Not_found -> raise (Fatal ("cannot resolve host " ^ host)))
      in
      Unix.ADDR_INET (a, port)
  in
  { addr; user; password; database; statements }

type item = {
  mutable rows : string array list;
  mutable tag : string;
  mutable err : error option;
  mutable last : bool;  (* followed by a Sync: the end of its batch *)
  k : (result, error) Stdlib.result IO.ivar;
}

type conn = {
  config : conn_config;
  rt : runtime;
  mutable fd : IO.fd option;
  mutable out : Out.t;
  mutable spare : Out.t;
  pending : item Queue.t;
  mutable in_flight : int;
  mutable writer_wait : unit IO.ivar option;
  mutable rbuf : bytes;
  mutable rpos : int;
  mutable rlen : int;
  mutable down_reason : string;
  mutable open_batch : item option;  (* the last read in [out], its Sync not written yet *)
  mutable skipping : bool;  (* an error aborted the batch: the rest of it gets no answer *)
  batch_bytes : int;
  batch_wait : float;
  mutable full_wait : unit IO.ivar option;  (* the writer, waiting for [batch_bytes] *)
}

(* Receive buffer per connection: a batch's answers in a few reads. *)
let rbuf_size = 131_072

let read_more c fd =
  if c.rpos > 0 && c.rpos = c.rlen then (c.rpos <- 0; c.rlen <- 0);
  (* Compact (or grow, when one message fills the buffer) once less than an
     eighth is free, rather than read a few bytes at its end. *)
  if c.rpos > 0 && Bytes.length c.rbuf - c.rlen < Bytes.length c.rbuf / 8
     || c.rlen = Bytes.length c.rbuf then begin

    let live = c.rlen - c.rpos in
    let b = if c.rpos = 0 then Bytes.create (2 * Bytes.length c.rbuf) else c.rbuf in
    Bytes.blit c.rbuf c.rpos b 0 live;
    c.rbuf <- b; c.rpos <- 0; c.rlen <- live
  end;
  match IO.read fd c.rbuf c.rlen (Bytes.length c.rbuf - c.rlen) with
  | 0 -> raise End_of_file
  | n -> c.rlen <- c.rlen + n

let rec next_message c fd =
  let avail = c.rlen - c.rpos in
  if avail >= 5 then begin
    let len = Int32.to_int (Bytes.get_int32_be c.rbuf (c.rpos + 1)) in
    if avail >= 1 + len then begin
      let typ = Bytes.unsafe_get c.rbuf c.rpos in
      let off = c.rpos + 5 in
      c.rpos <- c.rpos + 1 + len;
      (typ, off, len - 4)
    end
    else (read_more c fd; next_message c fd)
  end
  else (read_more c fd; next_message c fd)

let cstring_at b off =
  let e = Bytes.index_from b off '\000' in
  (Bytes.sub_string b off (e - off), e + 1)

let parse_error b off len =
  let stop = off + len in
  let rec go off code message =
    if off >= stop || Bytes.get b off = '\000' then
      Server { code; message }
    else
      let f = Bytes.get b off in
      let v, off = cstring_at b (off + 1) in
      match f with
      | 'C' -> go off v message
      | 'M' -> go off code v
      | _ -> go off code message
  in
  go off "" ""

exception Null_column

let parse_row b off =
  let n = Bytes.get_uint16_be b off in
  let row = Array.make n "" in
  let pos = ref (off + 2) in
  for i = 0 to n - 1 do
    let l = Int32.to_int (Bytes.get_int32_be b !pos) in
    if l < 0 then raise Null_column;
    row.(i) <- Bytes.sub_string b (!pos + 4) l;
    pos := !pos + 4 + l
  done;
  row

let hmac key data = Digestif.SHA256.(to_raw_string (hmac_string ~key data))
let sha256 s = Digestif.SHA256.(to_raw_string (digest_string s))
let xor a b = String.init (String.length a) (fun i -> Char.chr (Char.code a.[i] lxor Char.code b.[i]))

let pbkdf2 password salt iterations =
  let u = ref (hmac password (salt ^ "\000\000\000\001")) in
  let acc = ref !u in
  for _ = 2 to iterations do
    u := hmac password !u;
    acc := xor !acc !u
  done;
  !acc

let random_nonce () =
  let ic = open_in_bin "/dev/urandom" in
  let s = Fun.protect ~finally:(fun () -> close_in ic) (fun () -> really_input_string ic 18) in
  Base64.encode_string s

let attrs s =
  List.filter_map
    (fun kv ->
      if String.length kv >= 2 && kv.[1] = '=' then
        Some (kv.[0], String.drop_first 2 kv)
      else None)
    (String.split_on_char ',' s)

let send fd (o : Out.t) =
  IO.write fd o.b 0 o.len;
  o.len <- 0

let password c =
  match c.config.password with
  | Some p -> p
  | None -> raise (Fatal "the server asks for a password and none was given")

let sasl_initial fd o mechanisms =
  if not (List.mem "SCRAM-SHA-256" mechanisms) then
    raise (Fatal ("unsupported SASL mechanisms: " ^ String.concat "," mechanisms));
  let cnonce = random_nonce () in
  let client_first_bare = "n=,r=" ^ cnonce in
  let client_first = "n,," ^ client_first_bare in
  let at = Out.start o 'p' in
  Out.cstring o "SCRAM-SHA-256";
  Out.int32 o (String.length client_first);
  Out.string o client_first;
  Out.finish o at;
  send fd o;
  (cnonce, client_first_bare)

let sasl_continue c fd o (cnonce, client_first_bare) server_first =
  let a = attrs server_first in
  let nonce = List.assoc 'r' a in
  let salt = Base64.decode_exn (List.assoc 's' a) in
  let iterations = int_of_string (List.assoc 'i' a) in
  if not (String.starts_with ~prefix:cnonce nonce) then raise (Fatal "SCRAM: bad nonce");
  let salted = pbkdf2 (password c) salt iterations in
  let client_key = hmac salted "Client Key" in
  let stored_key = sha256 client_key in
  let without_proof = "c=biws,r=" ^ nonce in
  let auth_message = String.concat "," [ client_first_bare; server_first; without_proof ] in
  let proof = xor client_key (hmac stored_key auth_message) in
  let at = Out.start o 'p' in
  Out.string o (without_proof ^ ",p=" ^ Base64.encode_string proof);
  Out.finish o at;
  send fd o;
  hmac (hmac salted "Server Key") auth_message

let startup c fd =
  let o = Out.create 256 in
  Out.int32 o 0;
  Out.int32 o 196608;
  List.iter (fun (k, v) -> Out.cstring o k; Out.cstring o v)
    [ ("user", c.config.user); ("database", c.config.database);
      ("client_encoding", "UTF8"); ("TimeZone", "UTC"); ("DateStyle", "ISO") ];
  Out.char o '\000';
  Bytes.set_int32_be o.b 0 (Int32.of_int o.len);
  send fd o;
  let scram = ref None and server_signature = ref "" in
  let rec auth () =
    match next_message c fd with
    | 'R', off, len -> (
        let b = c.rbuf in
        let payload = Bytes.sub_string b (off + 4) (len - 4) in
        match Int32.to_int (Bytes.get_int32_be b off) with
        | 0 -> ()
        | 3 ->
            let at = Out.start o 'p' in
            Out.cstring o (password c); Out.finish o at; send fd o; auth ()
        | 5 ->
            let hex s = Digestif.MD5.(to_hex (digest_string s)) in
            let at = Out.start o 'p' in
            Out.cstring o ("md5" ^ hex (hex (password c ^ c.config.user) ^ payload));
            Out.finish o at; send fd o; auth ()
        | 10 ->
            let mechanisms = String.split_all ~sep:"\000" ~drop:String.is_empty payload in
            scram := Some (sasl_initial fd o mechanisms); auth ()
        | 11 ->
            let st = match !scram with Some st -> st | None -> raise (Fatal "SCRAM: unexpected") in
            server_signature := sasl_continue c fd o st payload; auth ()
        | 12 ->
            let v = try List.assoc 'v' (attrs payload) with Not_found -> "" in
            if not (Eqaf.equal (Base64.decode_exn v) !server_signature) then
              raise (Fatal "SCRAM: bad server signature");
            auth ()
        | n -> raise (Fatal (Printf.sprintf "unsupported authentication method %d" n)))
    | 'E', off, len -> raise (Fatal (show_error (parse_error c.rbuf off len)))
    | _ -> auth ()
  in
  auth ();

  let rec ready () =
    match next_message c fd with
    | 'Z', _, _ -> ()
    | 'E', off, len -> raise (Fatal (show_error (parse_error c.rbuf off len)))
    | _ -> ready ()
  in
  ready ();

  List.iter
    (fun s ->
      let at = Out.start o 'P' in
      Out.cstring o s.name; Out.cstring o s.sql; Out.int16 o 0; Out.finish o at)
    c.config.statements;
  let at = Out.start o 'S' in
  Out.finish o at;
  send fd o;

  ready ()

let connect_fd c =
  let fd = c.rt.connect (c.config.addr ()) in
  match c.rpos <- 0; c.rlen <- 0; startup c fd with
  | () -> fd
  | exception e -> IO.close fd; raise e

let complete c it =
  c.in_flight <- c.in_flight - 1;
  let r =
    match it.err with
    | Some e -> Error e
    | None -> Ok { rows = List.rev it.rows; tag = it.tag }
  in
  IO.fill it.k r

let wake_writer c =
  match c.writer_wait with
  | Some w -> c.writer_wait <- None; IO.fill w ()
  | None -> ()

(* Ends the open batch of reads with one Sync: they share a round trip and an
   implicit transaction. *)
let close_batch c =
  match c.open_batch with
  | Some it ->
      it.last <- true;
      c.open_batch <- None;
      Out.finish c.out (Out.start c.out 'S')
  | None -> ()

let enqueue c stmt params it =
  if stmt.write then close_batch c;
  let o = c.out in
  let at = Out.start o 'B' in
  Out.char o '\000';
  Out.cstring o stmt.name;
  Out.int16 o 0;
  Out.int16 o (Array.length params);
  Array.iter (fun p -> Out.int32 o (String.length p); Out.string o p) params;
  Out.int16 o 0;
  Out.finish o at;
  let at = Out.start o 'E' in
  Out.char o '\000';
  Out.int32 o 0;
  Out.finish o at;
  Queue.push it c.pending;
  if stmt.write then (Out.finish o (Out.start o 'S'); it.last <- true)
  else c.open_batch <- Some it;
  (match c.full_wait with
   | Some w when o.len >= c.batch_bytes -> c.full_wait <- None; IO.fill w ()
   | _ -> ());
  wake_writer c

let rec reader c fd =
  let typ, off, len = next_message c fd in
  (if Queue.is_empty c.pending then begin
     if typ = 'E' then raise (Fatal (show_error (parse_error c.rbuf off len)))
   end
   else
     let it = Queue.peek c.pending in
     match typ with
     | 'D' -> (
         match parse_row c.rbuf off with
         | row -> it.rows <- row :: it.rows
         | exception Null_column -> it.err <- Some (Unsupported "NULL column"))
     (* A query ending its batch completes at ReadyForQuery, after the
        implicit transaction committed; the others at CommandComplete. *)
     | 'C' ->
         it.tag <- Bytes.sub_string c.rbuf off (max 0 (len - 1));
         if not it.last then complete c (Queue.take c.pending)
     | 'E' ->
         it.err <- Some (parse_error c.rbuf off len);
         if not it.last then (c.skipping <- true; complete c (Queue.take c.pending))
     | 'Z' when c.skipping ->
         let rec skip () =
           let it = Queue.take c.pending in
           it.err <- Some (Closed "aborted by an earlier error in its batch");
           complete c it;
           if not it.last then skip ()
         in
         c.skipping <- false;
         skip ()
     | 'Z' -> complete c (Queue.take c.pending)
     | _ -> ());
  reader c fd

let rec writer c =
  let ready () = c.out.len > 0 in
  if not (ready ()) then begin
    let w = IO.ivar () in
    c.writer_wait <- Some w;
    IO.await w
  end;
  (* Fewer, larger writes: wait (at most [batch_wait]) for [batch_bytes] of
     queries, which also share one Sync and so one round trip. *)
  if c.out.len < c.batch_bytes && c.batch_wait > 0. then begin
    let w = IO.ivar () in
    c.full_wait <- Some w;
    Eio.Fiber.first (fun () -> IO.await w) (fun () -> c.rt.sleep c.batch_wait);
    c.full_wait <- None
  end;
  close_batch c;
  (match c.fd with
   | Some fd when ready () ->
       let o = c.out in
       c.out <- c.spare;
       c.spare <- o;
       (try send fd o
        with e when not (IO.is_cancellation e) ->
          o.len <- 0;

          IO.shutdown fd)
   | Some _ -> ()
   | None -> c.out.len <- 0);
  writer c

let fail_all c reason =
  c.out.len <- 0;
  c.open_batch <- None;
  c.skipping <- false;
  let fail it = it.err <- Some (Closed reason); complete c it in
  Queue.iter fail c.pending;
  Queue.clear c.pending

let rec manage c =
  match c.fd with
  | None -> (
      match connect_fd c with
      | fd -> c.fd <- Some fd; manage c
      | exception e when not (IO.is_cancellation e) ->
          let reason = match e with Fatal m -> m | e -> Printexc.to_string e in
          if reason <> c.down_reason then
            Printf.eprintf "pgwire: cannot connect, retrying every second: %s\n%!" reason;
          c.down_reason <- reason;
          c.rt.sleep 1.;
          manage c)
  | Some fd ->
      let reason =
        match reader c fd with
        | () -> "closed"
        | exception End_of_file -> "closed by the server"
        | exception Fatal m -> m
        | exception Unix.Unix_error (e, _, _) -> Unix.error_message e
        | exception e when not (IO.is_cancellation e) -> Printexc.to_string e
      in
      c.fd <- None;
      c.down_reason <- reason;
      (try IO.close fd with _ -> ());
      fail_all c reason;
      c.rt.sleep 0.1;
      manage c

let submit c stmt params =
  let it = { rows = []; tag = ""; err = None; last = false; k = IO.ivar () } in
  c.in_flight <- c.in_flight + 1;
  enqueue c stmt params it;
  it.k

type group = {
  name : string;
  rt : runtime;
  config : conn_config;
  mutable conns : conn array;
  max : int;
  batch_bytes : int;
  batch_wait : float;
  mutable growing : bool;
  mutable wake : unit IO.ivar option;
  mutable filling : conn option;  (* the connection whose batch is being filled *)
  mutable next : int;  (* where the round-robin search for an idle connection starts *)
}

type pool = { reads : group; writes : group }

let new_conn rt config ~batch_bytes ~batch_wait =
  { config; rt; fd = None; out = Out.create 4096; spare = Out.create 4096;
    pending = Queue.create (); in_flight = 0; writer_wait = None;
    rbuf = Bytes.create rbuf_size; rpos = 0; rlen = 0;
    down_reason = "not connected"; open_batch = None; skipping = false;
    batch_bytes; batch_wait; full_wait = None }

let start rt c =
  rt.spawn (fun () -> manage c);
  rt.spawn (fun () -> writer c)

let rec grower g =
  if Array.length g.conns >= g.max then IO.await (IO.ivar ())
  else begin
    let w = IO.ivar () in
    g.wake <- Some w;
    IO.await w;
    let c = new_conn g.rt g.config ~batch_bytes:g.batch_bytes ~batch_wait:g.batch_wait in
    (match connect_fd c with
     | fd ->
         c.fd <- Some fd;
         g.conns <- Array.append g.conns [| c |];
         start g.rt c;
         Printf.eprintf "pgwire: %s pool %d -> %d connections\n%!" g.name
           (Array.length g.conns - 1) (Array.length g.conns)
     | exception e when not (IO.is_cancellation e) ->

         Printf.eprintf "pgwire: growing the %s pool failed: %s\n%!" g.name
           (match e with Fatal m -> m | e -> Printexc.to_string e));
    g.growing <- false;
    grower g
  end

let grow g =
  match g.wake with
  | Some w -> g.wake <- None; g.growing <- true; IO.fill w ()
  | None -> ()

type config = {
  uri : Uri.t;
  statements : statement list;
  size : int;
  max : int;
  write_size : int;
  write_max : int;
  batch_bytes : int;
  batch_wait : float;
}

let create_pool_rt rt p =
  let batch_bytes = p.batch_bytes and batch_wait = p.batch_wait in
  let statements = List.sort_uniq (fun a b -> String.compare a.sql b.sql) p.statements in
  let config = config_of_uri p.uri statements in
  let group name n max =
    let conns = Array.init n (fun _ -> new_conn rt config ~batch_bytes ~batch_wait) in
    Array.iter (start rt) conns;
    let g =
      { name; rt; config; conns; max = Stdlib.max n max; batch_bytes; batch_wait; growing = false;
        wake = None; filling = None; next = 0 }
    in
    rt.spawn (fun () -> grower g);
    g
  in
  let reads = group "read" p.size p.max in
  let writes = if p.write_size = 0 then reads else group "write" p.write_size p.write_max
  in
  { reads; writes }

let pick conns =
  let best = ref None in
  Array.iter
    (fun c ->
      if c.fd <> None then
        match !best with
        | Some b when b.in_flight <= c.in_flight -> ()
        | _ -> best := Some c)
    conns;
  !best

(* The next idle connection (nothing queued or in flight), round-robin. *)
let idle g =
  let n = Array.length g.conns in
  let rec go i =
    if i = n then None
    else
      let c = g.conns.((g.next + i) mod n) in
      if c.fd <> None && Queue.is_empty c.pending
      then (g.next <- (g.next + i + 1) mod n; Some c)
      else go (i + 1)
  in
  go 0

(* A query joins the batch being filled until it is sent or full; the next
   batch starts on an idle connection. None idle: open one more (one at a time,
   up to [max]) and meanwhile use the least loaded. *)
let query pool s params =
  let g = if s.write then pool.writes else pool.reads in
  let chosen =
    match g.filling with
    | Some c when c.fd <> None && c.out.len > 0 && c.out.len < c.batch_bytes -> g.filling
    | _ -> (
        match idle g with
        | Some _ as c -> c
        | None ->
            if (not g.growing) && Array.length g.conns < g.max then grow g;
            pick g.conns)
  in
  g.filling <- chosen;
  match chosen with
  | None -> Error (Closed g.conns.(0).down_reason)
  | Some c -> IO.await (submit c s params)

let create_pool ~sw ~net ~clock config =
  let connect addr =
    let options =
      match addr with
      | Unix.ADDR_INET _ ->
          (* Keepalive notices a vanished peer on an idle connection; the user
             timeout one with queries in flight, generous so that a slow
             (zero window) server is never cut. *)
          Eio.Net.Sockopt.
            [ (TCP_NODELAY, true); (SO_KEEPALIVE, true); (TCP_KEEPIDLE, 5); (TCP_KEEPINTVL, 2);
              (TCP_KEEPCNT, 3); (TCP_USER_TIMEOUT, 30_000) ]
      | Unix.ADDR_UNIX _ -> Eio.Net.Sockopt.[]
    in
    let flow = Eio.Net.connect ~sw ~options net (Eio_unix.Net.sockaddr_of_unix_stream addr) in
    { IO.flow; rd = Cstruct.create rbuf_size; wr = Cstruct.create 4096 }
  in
  let rt = { connect; spawn = Eio.Fiber.fork ~sw; sleep = Eio.Time.Mono.sleep clock } in
  create_pool_rt rt config
