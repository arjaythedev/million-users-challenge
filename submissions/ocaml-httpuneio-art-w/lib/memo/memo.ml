module type S = sig
  type key
  type value

  val hash : key -> int
  val equal : key -> key -> bool
  val compute : key -> value
end

module Make (K : S) = struct
  type t = (K.key * K.value) option Atomic.t array

  let create n =
    if n < 1 then invalid_arg "Memo.create";
    Array.init n (fun _ -> Atomic.make None)

  let get t key =
    let n = Array.length t in
    let slot = t.((K.hash key land max_int) mod n) in
    match Atomic.get slot with
    | Some (k, v) when K.equal k key -> v
    | _ ->
        let v = K.compute key in
        Atomic.set slot (Some (key, v));
        v
end
