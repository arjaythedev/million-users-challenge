(** Memoizing a pure function in a fixed number of slots, thread-safe. *)

module type S = sig
  type key
  type value

  val hash : key -> int
  val equal : key -> key -> bool

  val compute : key -> value
  (** Computing the value from a key must be a pure function over immutable
      keys and values. *)
end

module Make (K : S) : sig
  type t

  val create : int -> t
  (** [create n]: memoize at most [n] key-values. *)

  val get : t -> K.key -> K.value
  (** Returns the value computed from the key. *)
end
