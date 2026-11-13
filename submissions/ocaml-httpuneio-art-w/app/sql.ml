let select_post =
  {|SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id|}

let feed = Pgwire.statement (select_post ^ " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
let post = Pgwire.statement (select_post ^ " WHERE p.id = $1")

let insert_post =
  Pgwire.statement ~write:true
    "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"

let insert_like =
  Pgwire.statement ~write:true
    "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"

let health = Pgwire.statement "SELECT 1"
let all = [ feed; post; insert_post; insert_like; health ]

(* "2026-10-27 17:19:37.5941+00" (the session runs with TimeZone=UTC, DateStyle=ISO) to
   "2026-10-27T17:19:37.594Z" *)
let iso8601 s =
  let b = Bytes.of_string "0000-00-00T00:00:00.000Z" in
  Bytes.blit_string s 0 b 0 10;
  Bytes.blit_string s 11 b 11 8;
  if String.length s > 19 && s.[19] = '.' then begin
    let i = ref 20 in
    while !i < String.length s && !i < 23 && s.[!i] >= '0' && s.[!i] <= '9' do
      Bytes.set b !i s.[!i];
      incr i
    done
  end;
  Bytes.unsafe_to_string b
