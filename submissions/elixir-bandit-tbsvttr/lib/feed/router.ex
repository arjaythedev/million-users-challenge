defmodule Feed.Router do
  @moduledoc false
  # The five endpoints as a bare Plug: routing on method and path segments, no Plug.Router or Plug.Parsers.
  # Response JSON is built as iodata in the spec's key order.
  @behaviour Plug
  import Plug.Conn
  require Logger

  @post_select """
  SELECT p.id, p.body, p.created_at, u.username,
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
    FROM posts p JOIN users u ON u.id = p.user_id
  """
  @feed_sql @post_select <> " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
  @post_sql @post_select <> " WHERE p.id = $1"
  @insert_post_sql "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
  @insert_like_sql "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"

  # ids beyond 2^53 are out of scope; they get "invalid post id"
  @max_id 9_007_199_254_740_991
  @gzip_min 1024

  @impl true
  def init(opts), do: opts

  @impl true
  def call(conn, _opts) do
    route(conn.method, conn.path_info, conn)
  rescue
    e ->
      Logger.error(Exception.format(:error, e, __STACKTRACE__))
      error(conn, 500, "internal server error")
  end

  defp route("GET", ["feed"], conn), do: feed(conn)
  defp route("GET", ["posts", id], conn), do: get_post(conn, id)
  defp route("POST", ["posts", id, "like"], conn), do: with_user(conn, &like(&1, &2, id))
  defp route("POST", ["posts"], conn), do: with_user(conn, &create_post/2)
  defp route("GET", ["health"], conn), do: health(conn)
  defp route(_method, _path, conn), do: error(conn, 404, "not found")

  # ---- handlers ----

  defp health(conn) do
    case Feed.DB.query("SELECT 1", [], "health") do
      {:ok, _} ->
        uptime = div(System.monotonic_time(:millisecond) - :persistent_term.get(:feed_started_ms), 1000)
        json(conn, 200, [~s({"status":"ok","db":"ok","uptime_s":), Integer.to_string(uptime), ?}])

      {:error, e} ->
        json(conn, 503, [~s({"status":"degraded","db":"unreachable","error":),
                         JSON.encode_to_iodata!(Exception.message(e)), ?}])
    end
  end

  defp feed(conn) do
    %{rows: rows} = Feed.DB.query!(@feed_sql, [], "feed")
    json(conn, 200, [~s({"posts":[), Enum.map_intersperse(rows, ?,, &row_json/1), "]}"], :gzip)
  end

  defp get_post(conn, raw_id) do
    with id when is_integer(id) <- parse_id(raw_id),
         %{rows: [row]} <- Feed.DB.query!(@post_sql, [id], "post") do
      json(conn, 200, [~s({"post":), row_json(row), ?}], :gzip)
    else
      :error -> error(conn, 400, "invalid post id")
      %{rows: []} -> error(conn, 404, "post not found")
    end
  end

  defp create_post(conn, {user_id, username}) do
    {:ok, raw, conn} = read_body(conn, length: 65_536)

    case JSON.decode(raw) do
      {:ok, %{"body" => text}} when is_binary(text) -> insert_post(conn, user_id, username, String.trim(text))
      {:ok, _} -> error(conn, 400, "body is required")
      {:error, _} -> error(conn, 400, "malformed JSON body")
    end
  end

  defp insert_post(conn, _user_id, _username, ""), do: error(conn, 400, "body is required")

  defp insert_post(conn, user_id, username, text) do
    # "characters" are code points, like Postgres's char_length() in the CHECK constraint
    if byte_size(text) > 500 and length(String.to_charlist(text)) > 500 do
      error(conn, 400, "body must be at most 500 characters")
    else
      %{rows: [[id, created_at]]} = Feed.DB.query!(@insert_post_sql, [user_id, text], "insert_post")
      json(conn, 201, [~s({"post":), post_json(id, text, created_at, username, 0), ?}])
    end
  end

  defp like(conn, {user_id, _username}, raw_id) do
    case parse_id(raw_id) do
      :error ->
        error(conn, 400, "invalid post id")

      id ->
        case Feed.DB.query(@insert_like_sql, [user_id, id], "insert_like") do
          {:ok, %{num_rows: inserted}} ->
            json(conn, if(inserted == 1, do: 201, else: 200),
              [~s({"liked":true,"already_liked":), Atom.to_string(inserted != 1), ~s(,"post_id":), Integer.to_string(id), ?}])

          {:error, %Postgrex.Error{postgres: %{code: :foreign_key_violation}}} ->
            error(conn, 404, "post not found")

          {:error, e} ->
            raise e
        end
    end
  end

  # ---- auth: checked before the id and the body ----

  defp with_user(conn, handler) do
    case authenticate(conn) do
      {:ok, user} -> handler.(conn, user)
      {:error, message} -> error(conn, 401, message)
    end
  end

  defp authenticate(conn) do
    case get_req_header(conn, "authorization") do
      ["Bearer " <> token | _] ->
        case Feed.JWT.verify(token) do
          {:ok, %{"sub" => sub, "username" => username}} when is_binary(sub) and is_binary(username) ->
            case parse_id(sub) do
              :error -> {:error, "invalid token payload"}
              id -> {:ok, {id, username}}
            end

          {:ok, _claims} ->
            {:error, "invalid token payload"}

          :error ->
            {:error, "invalid or expired token"}
        end

      _ ->
        {:error, "missing bearer token"}
    end
  end

  # ---- helpers ----

  # A positive integer written in digits, or :error (0, -1, abc, 1.5, beyond 2^53)
  defp parse_id(raw) do
    with true <- raw != "" and digits?(raw),
         id when id > 0 and id <= @max_id <- String.to_integer(raw) do
      id
    else
      _ -> :error
    end
  end

  defp digits?(<<c, rest::binary>>) when c in ?0..?9, do: digits?(rest)
  defp digits?(<<>>), do: true
  defp digits?(_), do: false

  # body and username are slices of the Postgres socket's receive buffer; copy them so the response doesn't keep
  # that whole buffer alive.
  defp row_json([id, body, created_at, username, like_count]),
    do: post_json(id, :binary.copy(body), created_at, :binary.copy(username), like_count)

  # Postgrex gives TIMESTAMPTZ as a UTC DateTime with microseconds; truncate (not round) to milliseconds.
  defp post_json(id, body, created_at, author, like_count) do
    [~s({"id":), Integer.to_string(id), ~s(,"body":), JSON.encode_to_iodata!(body),
     ~s(,"created_at":"), DateTime.to_iso8601(DateTime.truncate(created_at, :millisecond)),
     ~s(","author":), JSON.encode_to_iodata!(author), ~s(,"like_count":), Integer.to_string(like_count), ?}]
  end

  defp error(conn, status, message), do: json(conn, status, [~s({"error":), JSON.encode_to_iodata!(message), ?}])

  defp json(conn, status, body), do: send_resp(%{conn | resp_headers: [{"content-type", "application/json"}]}, status, body)

  # gzip (level 1) for bodies of 1 KB or more when the client asks for it: in practice the feed, 5.1 KB -> ~2 KB.
  defp json(conn, status, body, :gzip) do
    if :erlang.iolist_size(body) >= @gzip_min and accepts_gzip?(conn) do
      headers = [{"content-type", "application/json"}, {"content-encoding", "gzip"}, {"vary", "accept-encoding"}]
      send_resp(%{conn | resp_headers: headers}, status, gzip(body))
    else
      json(conn, status, body)
    end
  end

  defp accepts_gzip?(conn), do: Enum.any?(get_req_header(conn, "accept-encoding"), &String.contains?(&1, "gzip"))

  defp gzip(body) do
    z = :zlib.open()
    try do
      # window bits 15 + 16: a gzip wrapper instead of zlib's
      :ok = :zlib.deflateInit(z, 1, :deflated, 31, 8, :default)
      :zlib.deflate(z, body, :finish)
    after
      :zlib.close(z)
    end
  end
end
