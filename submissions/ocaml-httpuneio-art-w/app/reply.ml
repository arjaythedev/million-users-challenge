type scratch = { json : Buffer.t; compressor : Deflate.compressor }

let scratch_key =
  Domain.DLS.new_key @@ fun () ->
  { json = Buffer.create 16384;
    compressor = Deflate.compressor ~level:1 }

type t = { status : int; body : string }

let json ?(status = 200) enc value =
  (* Encoded in the domain's buffer, copied out right away: nothing suspends in
     between so it's safe to reuse. *)
  let b = (Domain.DLS.get scratch_key).json in
  Buffer.clear b;
  Jsonbuf.to_buffer enc b value;
  { status; body = Buffer.contents b }

let of_string ?(status = 200) body = { status; body }

let error_body = Jsonbuf.(obj [ field "error" string ])
let error status msg = json ~status error_body msg

(* The gzip of every body large enough, by its bytes (the feed's, from its
   memo: a hit is the very same string). *)
module Gzip_memo = Memo.Make (struct
  type key = string
  type value = string

  let hash = Hashtbl.hash
  let equal = String.equal
  let compute body = Deflate.gzip (Domain.DLS.get scratch_key).compressor body
end)

let gzip_memo = Gzip_memo.create 4096

let gzip_min = 1024

let gzipped ~accept_encoding body =
  match accept_encoding with
  | Some s when String.length body >= gzip_min && String.includes ~affix:"gzip" s ->
      Some (Gzip_memo.get gzip_memo body)
  | _ -> None
