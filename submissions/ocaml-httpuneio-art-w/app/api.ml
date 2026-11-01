let jwt_secret = Option.value (Sys.getenv_opt "JWT_SECRET") ~default:"million-users-challenge"
let started = Unix.gettimeofday ()
let uptime () = int_of_float (Unix.gettimeofday () -. started)

let internal e =
  prerr_endline (Pgwire.show_error e);
  Reply.error 500 "internal server error"

(* A positive integer of at most 18 digits: always fits an OCaml int and a
   Postgres bigint. *)
let parse_int s =
  if s = "" || String.length s > 18 || String.exists (function '0' .. '9' -> false | _ -> true) s
  then None
  else match int_of_string s with 0 -> None | n -> Some n

let with_auth authorization k =
  match authorization with
  | Some a when String.starts_with ~prefix:"Bearer " a -> (
      match Jwt.verify ~secret:jwt_secret (String.drop_first 7 a) with
      | Error () -> Reply.error 401 "invalid or expired token"
      | Ok { sub = Some sub; username = Some username } -> (
          match parse_int sub with
          | Some uid -> k (uid, username)
          | None -> Reply.error 401 "invalid token payload")
      | Ok _ -> Reply.error 401 "invalid token payload")
  | _ -> Reply.error 401 "missing bearer token"

let body_codec =
  let str = Jsont.map Jsont.json ~dec:(function Jsont.String (s, _) -> Some s | _ -> None) in
  Jsont.Object.map Option.join
  |> Jsont.Object.opt_mem "body" str
  |> Jsont.Object.skip_unknown
  |> Jsont.Object.finish

let decode_body s =
  match Jsont_bytesrw.decode_string body_codec s with
  | Ok (Some v) -> Ok (String.trim v)
  | Ok None -> Ok ""
  | Error _ -> (
      match Jsont_bytesrw.decode_string Jsont.json s with
      | Ok _ -> Ok ""
      | Error _ -> Error ())

(* Code points, as Postgres' char_length: the bytes that aren't continuations (10xxxxxx). *)
let utf8_length s = String.fold_left (fun n c -> if Char.code c land 0xC0 = 0x80 then n else n + 1) 0 s

let post_text body =
  match decode_body body with
  | Error () -> Error (Reply.error 400 "malformed JSON body")
  | Ok "" -> Error (Reply.error 400 "body is required")
  | Ok s when utf8_length s > 500 -> Error (Reply.error 400 "body must be at most 500 characters")
  | Ok s -> Ok s

(* A post from its row: id, body, created_at, author, like_count. *)
let post =
  let open Jsonbuf in
  obj_array
    [ field "id" (map int_of_string int);
      field "body" string;
      field "created_at" (map Sql.iso8601 string);
      field "author" string;
      field "like_count" (map int_of_string int) ]

let feed_reply = Jsonbuf.(obj [ field "posts" (list post) ])

module Feed_memo = Memo.Make (struct
  (* The feed's JSON, by the rows it is built from: when this request's query
     returns the very same rows as an earlier feed, the JSON is the same and
     isn't built again (the query still runs on every request, only the json
     rendering is optimized). *)
  type key = string array list
  type value = string
  let hash = Hashtbl.hash
  let equal = List.equal (Array.equal String.equal)
  let compute = Jsonbuf.to_string feed_reply
end)

let feed_memo = Feed_memo.create 4096

let feed pool =
  match Pgwire.query pool Sql.feed [||] with
  | Error e -> internal e
  | Ok { rows; _ } -> Reply.of_string (Feed_memo.get feed_memo rows)

let post_reply = Jsonbuf.(obj [ field "post" post ])

let get_post pool id =
  match parse_int id with
  | None -> Reply.error 400 "invalid post id"
  | Some id -> (
      match Pgwire.query pool Sql.post [| string_of_int id |] with
      | Error e -> internal e
      | Ok { rows = []; _ } -> Reply.error 404 "post not found"
      | Ok { rows = r :: _; _ } -> Reply.json post_reply r)

let create pool ~authorization body =
  with_auth authorization @@ fun (uid, username) ->
  match post_text body with
  | Error e -> e
  | Ok text -> (
      match Pgwire.query pool Sql.insert_post [| string_of_int uid; text |] with
      | Error e -> internal e
      | Ok { rows = [| id; created_at |] :: _; _ } ->
          Reply.json ~status:201 post_reply [| id; text; created_at; username; "0" |]
      | Ok _ -> Reply.error 500 "internal server error")

let liked =
  let open Jsonbuf in
  obj [ field "liked" (const bool true);
        field "already_liked" (map fst bool);
        field "post_id" (map snd int) ]

let like pool ~authorization id =
  with_auth authorization @@ fun (uid, _) ->
  match parse_int id with
  | None -> Reply.error 400 "invalid post id"
  | Some post_id -> (
      match Pgwire.query pool Sql.insert_like [| string_of_int uid; string_of_int post_id |] with
      | Ok r ->
          let already = Pgwire.affected r = 0 in
          Reply.json ~status:(if already then 200 else 201) liked (already, post_id)
      | Error (Pgwire.Server { code; _ }) when code = Pgwire.foreign_key_violation ->
          Reply.error 404 "post not found"
      | Error e -> internal e)

let healthy =
  let open Jsonbuf in
  obj [ field "status" (const string "ok");
        field "db" (const string "ok");
        field "uptime_s" int ]

let degraded =
  let open Jsonbuf in
  obj [ field "status" (const string "degraded");
        field "db" (const string "unreachable");
        field "error" string ]

let health pool =
  match Pgwire.query pool Sql.health [| |] with
  | Ok _ -> Reply.json healthy (uptime ())
  | Error e -> Reply.json ~status:503 degraded (Pgwire.show_error e)

let handle pool ~meth ~target ~authorization ~body =
  let path = String.take_first_while (( <> ) '?') target in
  match meth, String.split_on_char '/' path with
  | `GET, [ ""; "feed" ] -> feed pool
  | `GET, [ ""; "posts"; id ] -> get_post pool id
  | `GET, [ ""; "health" ] -> health pool
  | `POST, [ ""; "posts" ] -> create pool ~authorization body
  | `POST, [ ""; "posts"; id; "like" ] -> like pool ~authorization id
  | _ -> Reply.error 404 "not found"
