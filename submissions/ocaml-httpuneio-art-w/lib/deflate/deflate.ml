type compressor

external compressor : unit -> compressor = "igz_alloc_compressor"
external gzip_ : compressor -> string -> bytes -> int = "igz_gzip" [@@noalloc]

(* Room for incompressible input: igzip then falls back to stored blocks
   (5 bytes per 64 KB block), plus the gzip header and trailer. *)
let bound n = n + (n / 16) + 1024

let gzip c s =
  let dst = Bytes.create (bound (String.length s)) in
  let len = gzip_ c s dst in
  if len < 0 then failwith "Deflate.gzip";
  Bytes.sub_string dst 0 len
