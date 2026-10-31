let env_int name default =
  match Sys.getenv_opt name with Some v -> int_of_string v | None -> default

let host = Option.value (Sys.getenv_opt "HOST") ~default:"0.0.0.0"
let port = env_int "PORT" 3000
let extra_port = env_int "EXTRA_PORT" 0
let ports = port :: if extra_port > 0 then [ extra_port ] else []
let domains = env_int "DOMAINS" (Domain.recommended_domain_count ())

let database_url =
  match Sys.getenv_opt "DATABASE_URL" with
  | Some url -> Uri.of_string url
  | None -> prerr_endline "DATABASE_URL is not set"; exit 2

(* Per domain: 2 + 2 connections at start, growing up to a share of 500
   (under the database's cap of 512), 2/3 reads and 1/3 writes, at most 24 + 12. *)
let per_domain = 500 / max 1 domains
let db =
  let size = env_int "DB_POOL" 2 and write_size = env_int "DB_WRITE_POOL" 2 in
  { Pgwire.uri = database_url;
    statements = Sql.all;
    size;
    max = env_int "DB_POOL_MAX" (max size (min 24 (per_domain * 2 / 3)));
    write_size;
    write_max = env_int "DB_WRITE_POOL_MAX" (max write_size (min 12 (per_domain / 3)));
    batch_bytes = env_int "DB_BATCH_BYTES" 1400;
    batch_wait = float (env_int "DB_BATCH_WAIT_US" 2000) /. 1e6 }

let () =
  if Sys.getenv_opt "OCAMLRUNPARAM" = None then
    Gc.set { (Gc.get ()) with space_overhead = 400; minor_heap_size = 1 lsl 21 }

let () = Sys.set_signal Sys.sigpipe Sys.Signal_ignore

let () =
  Sys.set_signal Sys.sigusr2
    (Sys.Signal_handle
       (fun _ ->
         let s = Gc.quick_stat () in
         Printf.eprintf
           "gc heap_mb=%d top_heap_mb=%d minor_words=%.0f promoted_words=%.0f \
            major_words=%.0f minor_gcs=%d major_gcs=%d\n%!"
           (s.heap_words * 8 / 1048576)
           (s.top_heap_words * 8 / 1048576)
           s.minor_words s.promoted_words s.major_words s.minor_collections s.major_collections))

let config =
  { Httpun.Config.
    read_buffer_size = 1024;
    request_body_buffer_size = 4096;
    response_buffer_size = 1024;
    response_body_buffer_size = 64 }

let respond reqd ~accept_encoding { Reply.status; body } =
  let body, encoding =
    match Reply.gzipped ~accept_encoding body with
    | Some gz -> (gz, [ ("content-encoding", "gzip") ])
    | None -> (body, [])
  in
  let headers =
    Httpun.Headers.of_list
      (("content-type", "application/json")
      :: ("content-length", string_of_int (String.length body))
      :: encoding)
  in
  let response = Httpun.Response.create ~headers (Httpun.Status.of_code status) in
  Httpun.Reqd.respond_with_string reqd response body

let request_handler pool reqd =
  let { Httpun.Request.meth; target; headers; _ } = Httpun.Reqd.request reqd in
  let accept_encoding = Httpun.Headers.get headers "accept-encoding" in
  let authorization = Httpun.Headers.get headers "authorization" in
  let handle body =
    respond reqd ~accept_encoding
    @@ Api.handle pool ~meth ~target ~authorization ~body
  in
  match meth with
  | `POST -> Httpun_fiber.with_body reqd handle
  | _ -> handle ""

let error_handler ?request:_ _err start_response =
  let w = start_response Httpun.Headers.empty in
  Httpun.Body.Writer.write_string w {|{"error":"internal server error"}|};
  Httpun.Body.Writer.close w

let serve env () =
  Eio.Switch.run @@ fun sw ->
  let net = Eio.Stdenv.net env in
  let clock = Eio.Stdenv.mono_clock env in
  let pool = Pgwire.create_pool ~sw ~net ~clock db in
  let ip = Eio_unix.Net.Ipaddr.of_unix (Unix.inet_addr_of_string host) in
  let listen port () =
    Httpun_fiber.listen ~sw ~net ~config ~error_handler (`Tcp (ip, port))
    @@ request_handler pool
  in
  Eio.Fiber.all (List.map listen ports)

let () =
  Eio_main.run @@ fun env ->
  Printf.printf "listening on %s:%d%s with %d domain(s)\n%!" host port
    (if extra_port > 0 then " and " ^ string_of_int extra_port else "")
    domains;
  let dmgr = Eio.Stdenv.domain_mgr env in
  Eio.Fiber.all @@ List.init domains @@ fun i () ->
  if i = 0 then serve env () else Eio.Domain_manager.run dmgr (serve env)
