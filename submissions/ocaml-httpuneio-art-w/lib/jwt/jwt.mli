(** HS256 JSON Web Tokens. *)

type claims = {
  sub : string option;
  username : string option;
}

val verify : secret:string -> ?now:float -> string -> (claims, unit) result
(** [Ok] if the token is [header.payload.signature] (base64url, unpadded),
    the HMAC-SHA256 signature matches (constant-time compare), the header's
    [alg] is ["HS256"] and the payload's numeric [exp] is after [now]
    (default: the current time). *)

val sign : secret:string -> sub:string -> username:string -> exp:int -> string
(** A fresh token which {!verify} will accept until [exp] (Unix time). *)
