defmodule Feed.Auth do
  @moduledoc false
  @header_b64 Base.url_encode64(~s({"alg":"HS256","typ":"JWT"}), padding: false)
  def verify(conn, secret) do
    case Plug.Conn.get_req_header(conn, "authorization") do
      ["Bearer " <> token | _] -> verify_token(String.trim(token), secret)
      _ -> {:error, :missing}
    end
  end
  defp verify_token("", _), do: {:error, :missing}
  defp verify_token(token, secret) do
    case :binary.split(token, ".", [:global]) do
      [hb64, pb64, sb64] -> verify_parts(hb64, pb64, sb64, secret)
      _ -> {:error, :invalid}
    end
  end
  defp verify_parts(hb64, pb64, sb64, secret) when hb64 == @header_b64 do
    verify_sig_claims(hb64, pb64, sb64, secret)
  end
  defp verify_parts(hb64, pb64, sb64, secret) do
    if header_hs256?(hb64) do
      verify_sig_claims(hb64, pb64, sb64, secret)
    else
      {:error, :invalid}
    end
  end
  defp header_hs256?(hb64) do
    with {:ok, hjson} <- Base.url_decode64(hb64, padding: false),
         {:ok, header} <- Jason.decode(hjson),
         true <- is_map(header) and header["alg"] == "HS256" do
      true
    else
      _ -> false
    end
  end
  defp verify_sig_claims(hb64, pb64, sb64, secret) do
    with signing = hb64 <> "." <> pb64,
         {:ok, sig} <- b64decode(sb64),
         true <- sig_ok?(signing, sig, secret),
         {:ok, pjson} <- b64decode(pb64),
         {:ok, payload} <- Jason.decode(pjson),
         {:ok, uid, uname} <- check_claims(payload) do
      {:ok, uid, uname}
    else
      {:error, :payload} -> {:error, :payload}
      _ -> {:error, :invalid}
    end
  end
  defp b64decode(s) do
    case Base.url_decode64(s, padding: false) do
      {:ok, b} -> {:ok, b}
      :error -> :error
    end
  end
  defp sig_ok?(signing, sig, secret) do
    exp = :crypto.mac(:hmac, :sha256, secret, signing)
    if byte_size(exp) != byte_size(sig), do: false, else: do_compare(exp, sig)
  end
  defp do_compare(a, b) do
    if function_exported?(:crypto, :hash_equals, 2) do
      :crypto.hash_equals(a, b)
    else
      Plug.Crypto.secure_compare(a, b)
    end
  end
  defp check_claims(%{"sub" => sub, "username" => uname, "exp" => exp}) when is_binary(uname) do
    now = System.system_time(:second)
    exp_ok = is_integer(exp) and exp > now
    case parse_sub(sub) do
      {:ok, uid} when exp_ok -> {:ok, uid, uname}
      {:ok, _} -> {:error, :invalid}
      _ -> {:error, :payload}
    end
  end
  defp check_claims(%{"sub" => _, "username" => uname, "exp" => _}) when not is_binary(uname),
    do: {:error, :payload}
  defp check_claims(%{"sub" => _, "username" => _}), do: {:error, :invalid}
  defp check_claims(_), do: {:error, :payload}
  defp parse_sub(s) when is_binary(s) do
    s = String.trim(s)
    case Integer.parse(s) do
      {n, ""} when n > 0 -> {:ok, n}
      _ -> :error
    end
  end
  defp parse_sub(_), do: :error
end
