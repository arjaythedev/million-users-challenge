defmodule FeedTest do
  use ExUnit.Case, async: true
  import Plug.Test
  import Plug.Conn, only: [put_req_header: 3]

  @secret "test-secret"

  defp b64url(bin), do: Base.url_encode64(bin, padding: false)

  defp sign(payload_map) do
    h = b64url(~s({"alg":"HS256","typ":"JWT"}))
    p = b64url(Jason.encode!(payload_map))
    s = :crypto.mac(:hmac, :sha256, @secret, h <> "." <> p) |> b64url()
    h <> "." <> p <> "." <> s
  end

  defp conn_with_auth(token) do
    conn(:post, "/posts", "{}") |> put_req_header("authorization", "Bearer #{token}")
  end

  test "missing token" do
    assert Feed.Auth.verify(conn(:post, "/posts", "{}"), @secret) == {:error, :missing}
    assert Feed.Auth.verify(conn(:post, "/posts", "{}") |> put_req_header("authorization", "Basic abc"), @secret) == {:error, :missing}
  end

  test "garbage and bad signature are invalid" do
    assert Feed.Auth.verify(conn_with_auth("abc"), @secret) == {:error, :invalid}
    now = System.system_time(:second)
    good = sign(%{"sub" => "1", "username" => "u1", "iat" => now, "exp" => now + 3600})
    [_h, _p, _s] = String.split(good, ".")
    bad = good |> String.slice(0..-2//1) |> Kernel.<>("x")
    assert Feed.Auth.verify(conn_with_auth(bad), @secret) == {:error, :invalid}
  end

  test "alg=none rejected" do
    h = b64url(~s({"alg":"none","typ":"JWT"}))
    p = b64url(Jason.encode!(%{"sub" => "1", "username" => "u", "exp" => System.system_time(:second) + 60}))
    assert Feed.Auth.verify(conn_with_auth(h <> "." <> p <> "."), @secret) == {:error, :invalid}
  end

  test "expired rejected" do
    now = System.system_time(:second)
    t = sign(%{"sub" => "1", "username" => "u", "iat" => now - 7200, "exp" => now - 10})
    assert Feed.Auth.verify(conn_with_auth(t), @secret) == {:error, :invalid}
  end

  test "bad sub and missing username are payload errors" do
    now = System.system_time(:second)
    assert Feed.Auth.verify(conn_with_auth(sign(%{"sub" => "abc", "username" => "u", "exp" => now + 60})), @secret) == {:error, :payload}
    assert Feed.Auth.verify(conn_with_auth(sign(%{"sub" => "1", "exp" => now + 60})), @secret) == {:error, :payload}
  end

  test "valid token returns uid and username" do
    now = System.system_time(:second)
    t = sign(%{"sub" => "42", "username" => "golden_ember_1", "iat" => now, "exp" => now + 60})
    assert Feed.Auth.verify(conn_with_auth(t), @secret) == {:ok, 42, "golden_ember_1"}
  end

  test "esc quotes, controls, unicode" do
    assert Feed.Json.esc(~s(a"b\\c)) |> IO.iodata_to_binary() == ~s("a\\"b\\\\c")
    assert Feed.Json.esc("a\nb\tc\bd") |> IO.iodata_to_binary() == ~s("a\\nb\\tc\\bd")
    assert Feed.Json.esc(<<1, 31>>) |> IO.iodata_to_binary() == ~s("\\u0001\\u001f")
    assert Feed.Json.esc("café ✓") |> IO.iodata_to_binary() == ~s("café ✓")
    assert Feed.Json.esc(nil) == "\"\""
  end

  test "utf16 length counts astral as 2" do
    assert Feed.Json.utf16_len("abc") == 3
    assert Feed.Json.utf16_len("a😀") == 3
    assert Feed.Json.utf16_len(String.duplicate("a", 500)) == 500
    assert Feed.Json.utf16_len(String.duplicate("😀", 500)) == 1000
  end

  test "created_at truncates to ms, does not round" do
    ndt = ~N[2025-12-31 23:59:19.409876]
    assert Feed.Json.format_created_at(ndt) == "2025-12-31T23:59:19.409Z"
    assert Feed.Json.format_created_at(~N[2025-12-31 23:59:19.999999]) == "2025-12-31T23:59:19.999Z"
    dt = DateTime.from_naive!(~N[2025-01-01 00:00:00.001500], "Etc/UTC")
    assert Feed.Json.format_created_at(dt) == "2025-01-01T00:00:00.001Z"
  end

  test "post_object keeps spec key order" do
    iodata = Feed.Json.post_object(1, "hi", "2025-01-01T00:00:00.000Z", "u1", 0)
    s = IO.iodata_to_binary(iodata)
    assert s == ~s({"id":1,"body":"hi","created_at":"2025-01-01T00:00:00.000Z","author":"u1","like_count":0})
  end
end
