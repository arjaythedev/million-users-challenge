module C = Httpun.Server_connection

(* Waits for at least one byte more than httpun left unconsumed, hands it
   every buffered byte and drops the ones it consumed. *)
let read conn r =
  let eof =
    match Eio.Buf_read.(ensure r (buffered_bytes r + 1)) with
    | () -> false
    | exception End_of_file -> true
  in
  let { Cstruct.buffer; off; len } = Eio.Buf_read.peek r in
  let read = if eof then C.read_eof else C.read in
  Eio.Buf_read.consume r (read conn buffer ~off ~len)

let writev flow iovecs =
  let len = List.fold_left (fun a { Httpun.IOVec.len; _ } -> a + len) 0 iovecs in
  let of_iovec { Httpun.IOVec.buffer; off; len } = Cstruct.of_bigarray buffer ~off ~len in
  let cs = List.map of_iovec iovecs in
  match Eio.Flow.write flow cs with
  | () -> `Ok len
  | exception (Eio.Cancel.Cancelled _ as e) -> raise e
  | exception _ -> `Closed

let wait conn ~reader_open =
  let p, r = Eio.Promise.create () in
  let wake () = ignore (Eio.Promise.try_resolve r ()) in
  C.yield_writer conn wake;
  if reader_open then C.yield_reader conn wake;
  Eio.Promise.await p

(* Large enough for any request head; bodies stream through httpun. *)
let max_request_head = 1 lsl 20

let run conn ~read_buffer_size flow =
  let r = Eio.Buf_read.of_flow ~initial_size:read_buffer_size ~max_size:max_request_head flow in
  let reader_open = ref true in
  let rec loop () =
    match C.next_write_operation conn with
    | `Write iovecs ->
        C.report_write_result conn (writev flow iovecs);
        loop ()
    | `Close _ -> (try Eio.Flow.shutdown flow `Send with _ -> ())
    | `Yield ->
        if not !reader_open then (wait conn ~reader_open:false; loop ())
        else begin
          match C.next_read_operation conn with
          | `Read -> read conn r; loop ()
          | `Yield -> wait conn ~reader_open:true; loop ()
          | `Close -> reader_open := false; loop ()
        end
  in
  let rec guarded () =
    match loop () with
    | () -> ()
    | exception (Eio.Cancel.Cancelled _ as exn) -> raise exn
    | exception exn ->
        C.report_exn conn exn;
        guarded ()
  in
  guarded ()

let serve ?(config = Httpun.Config.default) ~error_handler request_handler flow =
  let conn = C.create ~config ~error_handler request_handler in
  try run conn ~read_buffer_size:config.read_buffer_size flow with
  | Eio.Cancel.Cancelled _ as e -> raise e
  | _ -> ()

let listen ~sw ~net ?(backlog = 65535) ?config ~error_handler addr request_handler =
  let socket = Eio.Net.listen net ~sw ~backlog ~reuse_addr:true ~reuse_port:true addr in
  let connection flow =
    Fun.protect ~finally:(fun () -> Eio.Resource.close flow) @@ fun () ->
    match Eio.Net.setsockopt flow Eio.Net.Sockopt.TCP_NODELAY true with
    | () -> serve ?config ~error_handler request_handler flow
    | exception (Eio.Cancel.Cancelled _ as e) -> raise e
    | exception _ -> () (* the peer is already gone *)
  in
  let rec loop () =
    (match Eio.Net.accept ~sw socket with
     | flow, _ -> Eio.Fiber.fork ~sw (fun () -> connection flow)
     | exception (Eio.Cancel.Cancelled _ as e) -> raise e
     | exception _ -> ());
    loop ()
  in
  loop ()

let with_body reqd k =
  let body = Httpun.Reqd.request_body reqd in
  let buf = Buffer.create 256 in
  let rec on_read bs ~off ~len =
    Buffer.add_string buf (Bigstringaf.substring bs ~off ~len);
    Httpun.Body.Reader.schedule_read body ~on_eof ~on_read
  and on_eof () = k (Buffer.contents buf) in
  Httpun.Body.Reader.schedule_read body ~on_eof ~on_read
