type claims = { sub : string option; username : string option }

let b64_decode s = Base64.decode ~pad:false ~alphabet:Base64.uri_safe_alphabet s
let b64_encode s = Base64.encode_string ~pad:false ~alphabet:Base64.uri_safe_alphabet s
let hmac ~secret s = Digestif.SHA256.(to_raw_string (hmac_string ~key:secret s))

let str = Jsont.map Jsont.json ~dec:(function Jsont.String (s, _) -> Some s | _ -> None)

let header_codec =
  Jsont.Object.map Option.join
  |> Jsont.Object.opt_mem "alg" str
  |> Jsont.Object.skip_unknown
  |> Jsont.Object.finish

let payload_codec =
  Jsont.Object.map (fun exp sub username -> (exp, Option.join sub, Option.join username))
  |> Jsont.Object.opt_mem "exp" Jsont.number
  |> Jsont.Object.opt_mem "sub" str
  |> Jsont.Object.opt_mem "username" str
  |> Jsont.Object.skip_unknown
  |> Jsont.Object.finish

let decode codec s =
  match Jsont_bytesrw.decode_string codec s with Ok v -> v | Error e -> failwith e

let verify ~secret ?now token =
  match String.split_on_char '.' token with
  | [ h; p; s ] -> (
      let signed = String.sub token 0 (String.length h + 1 + String.length p) in
      match (b64_decode h, b64_decode p, b64_decode s) with
      | Ok header, Ok payload, Ok s when Eqaf.equal s (hmac ~secret signed) -> (
          let now = match now with Some t -> t | None -> Unix.gettimeofday () in
          match (decode header_codec header, decode payload_codec payload) with
          | exception _ -> Error ()
          | Some "HS256", (Some exp, sub, username) when exp > now -> Ok { sub; username }
          | _ -> Error ())
      | _ -> Error ())
  | _ -> Error ()

let payload_json =
  Jsonbuf.(
    obj
      [ field "sub" (map (fun (sub, _, _) -> sub) string);
        field "username" (map (fun (_, username, _) -> username) string);
        field "exp" (map (fun (_, _, exp) -> exp) int) ])

let sign ~secret ~sub ~username ~exp =
  let payload = Jsonbuf.to_string payload_json (sub, username, exp) in
  let signed = b64_encode {|{"alg":"HS256","typ":"JWT"}|} ^ "." ^ b64_encode payload in
  signed ^ "." ^ b64_encode (hmac ~secret signed)
