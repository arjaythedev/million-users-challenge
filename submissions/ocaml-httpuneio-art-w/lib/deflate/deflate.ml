type compressor

external compressor : int -> compressor = "ld_alloc_compressor"
external gzip_bound : compressor -> int -> int = "ld_gzip_bound" [@@noalloc]
external gzip_ : compressor -> string -> bytes -> int = "ld_gzip" [@@noalloc]

let compressor ~level = compressor level

let gzip c s =
  let dst = Bytes.create (gzip_bound c (String.length s)) in
  let len = gzip_ c s dst in
  Bytes.sub_string dst 0 len
