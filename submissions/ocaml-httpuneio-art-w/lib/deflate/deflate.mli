(** gzip compression through libdeflate. *)

type compressor
(** Holds the match-finder state, not thread-safe. *)

val compressor : level:int -> compressor
(** Compression level 0 to 12 (0 means no compression). *)

val gzip : compressor -> string -> string
(** [gzip c str] returns [str] compressed by [c]. *)
