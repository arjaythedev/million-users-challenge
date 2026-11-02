defmodule Feed.Router do
  use Plug.Router
  plug :match
  plug :dispatch

  defp secret(), do: :persistent_term.get(:feed_jwt_secret)
  defp send_json(conn, status, iodata) do
    conn
    |> Plug.Conn.put_resp_content_type("application/json")
    |> Plug.Conn.send_resp(status, iodata)
  end
  defp gzip_accepted?(conn) do
    Enum.any?(Plug.Conn.get_req_header(conn, "accept-encoding"), fn v ->
      String.contains?(String.downcase(v), "gzip")
    end)
  end
  defp maybe_gzip(conn, status, iodata) do
    if gzip_accepted?(conn) and :erlang.iolist_size(iodata) >= 1024 do
      z = :zlib.open()
      :ok = :zlib.deflateInit(z, 1, :deflated, 31, 8, :default)
      gz = :zlib.deflate(z, iodata, :finish)
      :ok = :zlib.deflateEnd(z)
      :ok = :zlib.close(z)
      conn
      |> Plug.Conn.put_resp_content_type("application/json")
      |> Plug.Conn.put_resp_header("content-encoding", "gzip")
      |> Plug.Conn.put_resp_header("vary", "Accept-Encoding")
      |> Plug.Conn.send_resp(status, :erlang.iolist_to_binary(gz))
    else
      send_json(conn, status, iodata)
    end
  end
  defp err(conn, status, msg) do
    send_json(conn, status, ["{\"error\":", Feed.Json.esc(msg), "}"])
  end

  defp parse_id(s) when is_binary(s) do
    case s do
      <<c, _::binary>> when c >= ?0 and c <= ?9 ->
        case Integer.parse(s) do
          {n, ""} when n > 0 -> {:ok, n}
          _ -> :error
        end
      _ -> :error
    end
  end
  defp parse_id(_), do: :error

  defp row_to_iodata([id, body, created, author, like_count]) do
    # :binary.copy detaches PG socket recv-buffer sub-binaries so responses
    # don't retain the driver's large receive buffer.
    Feed.Json.post_object(id, :binary.copy(body), Feed.Json.format_created_at(created), :binary.copy(author), like_count)
  end

  get "/health" do
    case Feed.DB.ping() do
      {:ok, _} ->
        up = System.monotonic_time(:second) - :persistent_term.get(:feed_started)
        send_json(conn, 200, ["{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":", Integer.to_string(up), "}"])
      {:error, e} ->
        msg = Exception.message(e) |> String.slice(0, 200)
        conn
        |> Plug.Conn.put_resp_content_type("application/json")
        |> Plug.Conn.send_resp(503, ["{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":", Feed.Json.esc(msg), "}"])
    end
  end

  get "/feed" do
    case Feed.DB.feed(20) do
      {:ok, %Postgrex.Result{rows: rows}} ->
        posts = Enum.map(rows, &row_to_iodata/1) |> Enum.intersperse(",")
        maybe_gzip(conn, 200, ["{\"posts\":[", posts, "]}"])
      {:error, _} ->
        err(conn, 500, "internal server error")
    end
  end

  get "/posts/:id" do
    case parse_id(id) do
      :error -> err(conn, 400, "invalid post id")
      {:ok, pid} ->
        case Feed.DB.get_post(pid) do
          {:ok, %Postgrex.Result{rows: []}} -> err(conn, 404, "post not found")
          {:ok, %Postgrex.Result{rows: [row]}} ->
            maybe_gzip(conn, 200, ["{\"post\":", row_to_iodata(row), "}"])
          {:error, _} -> err(conn, 500, "internal server error")
        end
    end
  end

  post "/posts" do
    case Feed.Auth.verify(conn, secret()) do
      {:error, :missing} -> err(conn, 401, "missing bearer token")
      {:error, :invalid} -> err(conn, 401, "invalid or expired token")
      {:error, :payload} -> err(conn, 401, "invalid token payload")
      {:ok, uid, uname} ->
        case Plug.Conn.read_body(conn, length: 16_384) do
          {:more, _, conn} -> err(conn, 400, "malformed JSON body")
          {:ok, raw, conn} ->
            case Jason.decode(raw) do
              {:error, _} -> err(conn, 400, "malformed JSON body")
              {:ok, decoded} ->
                case decoded do
                  %{"body" => b} when is_binary(b) ->
                    trimmed = String.trim(b)
                    cond do
                      trimmed == "" -> err(conn, 400, "body is required")
                      Feed.Json.utf16_len(trimmed) > 500 -> err(conn, 400, "body must be at most 500 characters")
                      true ->
                        case Feed.DB.insert_post(uid, trimmed) do
                          {:ok, %Postgrex.Result{rows: [[new_id, created]]}} ->
                            post = Feed.Json.post_object(new_id, :binary.copy(trimmed), Feed.Json.format_created_at(created), :binary.copy(uname), 0)
                            send_json(conn, 201, ["{\"post\":", post, "}"])
                          {:error, _} -> err(conn, 500, "internal server error")
                        end
                    end
                  _ -> err(conn, 400, "body is required")
                end
            end
        end
    end
  end

  post "/posts/:id/like" do
    case Feed.Auth.verify(conn, secret()) do
      {:error, :missing} -> err(conn, 401, "missing bearer token")
      {:error, :invalid} -> err(conn, 401, "invalid or expired token")
      {:error, :payload} -> err(conn, 401, "invalid token payload")
      {:ok, uid, _} ->
        case parse_id(id) do
          :error -> err(conn, 400, "invalid post id")
          {:ok, pid} ->
            case Feed.DB.like(uid, pid) do
              {:ok, %Postgrex.Result{num_rows: n}} when n > 0 ->
                send_json(conn, 201, ["{\"liked\":true,\"already_liked\":false,\"post_id\":", Integer.to_string(pid), "}"])
              {:ok, %Postgrex.Result{num_rows: _}} ->
                send_json(conn, 200, ["{\"liked\":true,\"already_liked\":true,\"post_id\":", Integer.to_string(pid), "}"])
              {:error, %Postgrex.Error{postgres: %{code: code}}} when code in [:foreign_key_violation, "23503"] ->
                err(conn, 404, "post not found")
              {:error, _} -> err(conn, 500, "internal server error")
            end
        end
    end
  end

  match _ do
    err(conn, 404, "not found")
  end
end
