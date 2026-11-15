(** Writing compact JSON straight into a [Buffer.t], with predefined encoders. *)

type 'a t
(** The type of encoders of ['a] values. *)

(** {2 Rendering} *)

val to_buffer : 'a t -> Buffer.t -> 'a -> unit
(** Appends the encoding of a value. *)

val to_string : 'a t -> 'a -> string
(** Returns the json encoding of the ['a] value as a string. *)

(** {2 Combinators} *)

val int : int t
(** Encoder for integers. *)

val bool : bool t
(** Encoder for booleans. *)

val string : string t
(** Encoder for strings. *)

val list : 'a t -> 'a list t
(** [list e] is an encoder for a list of [e] encoded values. *)

val map : ('a -> 'b) -> 'b t -> 'a t
(** Transforms an existing encoder. *)

val const : 'b t -> 'b -> 'a t
(** [const e v] encodes [v] as a static value. *)

type 'a field
(** The type of ["key":value] in a json object. *)

val field : string -> 'a t -> 'a field
(** [field key e] is the member [key] of an object, its value encoded by [e]. *)

val obj : 'a field list -> 'a t
(** An object with these members. *)

val obj_array : 'a field list -> 'a array t
(** An object with these members, in order, the [i]th encoding the [i]th
    element of the array.

    @raise Invalid_argument if the array is shorter than the list. *)
