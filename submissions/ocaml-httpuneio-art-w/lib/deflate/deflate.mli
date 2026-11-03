(** gzip compression through ISA-L's igzip, level 0. *)

type compressor
(** Holds igzip's work buffer, not thread-safe. *)

val compressor : unit -> compressor

val gzip : compressor -> string -> string
(** [gzip c str] returns [str] compressed by [c], as one gzip member. *)
