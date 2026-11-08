defmodule Feed.JWT do
  @moduledoc false
  # HS256 only. The signature and exp are checked on every call; nothing is remembered.

  def verify(token) do
    key = :persistent_term.get({__MODULE__, :key})

    with [header, payload, signature] <- :binary.split(token, ".", [:global]),
         {:ok, header_json} <- Base.url_decode64(header, padding: false),
         {:ok, %{"alg" => "HS256"}} <- JSON.decode(header_json),
         {:ok, given} <- Base.url_decode64(signature, padding: false),
         expected = :crypto.mac(:hmac, :sha256, key, [header, ?., payload]),
         true <- byte_size(given) == byte_size(expected) and :crypto.hash_equals(given, expected),
         {:ok, payload_json} <- Base.url_decode64(payload, padding: false),
         {:ok, %{} = claims} <- JSON.decode(payload_json),
         true <- live?(claims, System.os_time(:millisecond)) do
      {:ok, claims}
    else
      _ -> :error
    end
  end

  # exp is required and must be in the future; nbf, when present, must not be.
  defp live?(%{"exp" => exp} = claims, now) when is_number(exp) and exp * 1000 > now do
    case claims do
      %{"nbf" => nbf} -> is_number(nbf) and nbf * 1000 <= now
      _ -> true
    end
  end

  defp live?(_claims, _now), do: false
end
