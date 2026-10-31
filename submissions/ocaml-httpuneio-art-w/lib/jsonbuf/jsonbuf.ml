type 'a t = Buffer.t -> 'a -> unit

let escapes =
  Array.init 0x20 @@ fun c ->
      match Char.chr c with
      | '\n' -> "\\n"
      | '\r' -> "\\r"
      | '\t' -> "\\t"
      | '\b' -> "\\b"
      | '\012' -> "\\f"
      | _ -> Printf.sprintf "\\u%04x" c

let string b s =
  Buffer.add_char b '"';
  let len = String.length s in
  let start = ref 0 in
  for i = 0 to len - 1 do
    let c = String.unsafe_get s i in
    if c = '"' || c = '\\' || c < ' ' then begin
      if i > !start then Buffer.add_substring b s !start (i - !start);
      Buffer.add_string b
        (match c with
         | '"' -> "\\\""
         | '\\' -> "\\\\"
         | c -> Array.unsafe_get escapes (Char.code c));
      start := i + 1
    end
  done;
  if len > !start then Buffer.add_substring b s !start (len - !start);
  Buffer.add_char b '"'

let rec int b n =
  if n < 0 then Buffer.add_string b (string_of_int n)
  else begin
    if n >= 10 then int b (n / 10);
    Buffer.add_char b (Char.unsafe_chr (48 + (n mod 10)))
  end

let bool b x = Buffer.add_string b (if x then "true" else "false")

let list e b l =
  let rec rest b e = function
    | [] -> ()
    | x :: l -> Buffer.add_char b ','; e b x; rest b e l
  in
  Buffer.add_char b '[';
  (match l with [] -> () | x :: l -> e b x; rest b e l);
  Buffer.add_char b ']'

let map f e b x = e b (f x)

let const e v =
  let b = Buffer.create 16 in
  e b v;
  let s = Buffer.contents b in
  fun b _ -> Buffer.add_string b s

type 'a field = { key : string; e : 'a t }

let field key e = { key; e }

let prefixes fields =
  Array.of_list
    (List.mapi
       (fun i { key; e } ->
         let p = Buffer.create 16 in
         Buffer.add_char p (if i = 0 then '{' else ',');
         string p key;
         Buffer.add_char p ':';
         { key = Buffer.contents p; e })
       fields)

let obj fields =
  let fields = prefixes fields in
  fun b x ->
    if Array.length fields = 0 then Buffer.add_char b '{';
    for i = 0 to Array.length fields - 1 do
      let { key; e } = fields.(i) in
      Buffer.add_string b key;
      e b x
    done;
    Buffer.add_char b '}'

let obj_array fields =
  let fields = prefixes fields in
  fun b a ->
    if Array.length fields = 0 then Buffer.add_char b '{';
    for i = 0 to Array.length fields - 1 do
      let { key; e } = fields.(i) in
      Buffer.add_string b key;
      e b a.(i)
    done;
    Buffer.add_char b '}'

let to_buffer e b x = e b x

let to_string e x =
  let b = Buffer.create 128 in
  e b x;
  Buffer.contents b
