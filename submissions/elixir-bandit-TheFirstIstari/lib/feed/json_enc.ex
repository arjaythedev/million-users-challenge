defmodule Feed.Json do
  @moduledoc false
  def post_object(id, body, created_at, author, like_count) do
    ["{\"id\":", Integer.to_string(id),
     ",\"body\":", esc(body),
     ",\"created_at\":", esc(created_at),
     ",\"author\":", esc(author),
     ",\"like_count\":", Integer.to_string(like_count), "}"]
  end
  def esc(s) when is_binary(s), do: [?", esc_bin(s), ?"]
  def esc(_), do: "\"\""
  defp esc_bin(<<>>), do: []
  defp esc_bin(<<"\"", r::binary>>), do: ["\\\"" | esc_bin(r)]
  defp esc_bin(<<"\\", r::binary>>), do: ["\\\\" | esc_bin(r)]
  defp esc_bin(<<"\b", r::binary>>), do: ["\\b" | esc_bin(r)]
  defp esc_bin(<<"\f", r::binary>>), do: ["\\f" | esc_bin(r)]
  defp esc_bin(<<"\n", r::binary>>), do: ["\\n" | esc_bin(r)]
  defp esc_bin(<<"\r", r::binary>>), do: ["\\r" | esc_bin(r)]
  defp esc_bin(<<"\t", r::binary>>), do: ["\\t" | esc_bin(r)]
  defp esc_bin(<<c, r::binary>>) when c < 0x20 do
    ["\\u00", hex2(c) | esc_bin(r)]
  end
  defp esc_bin(<<c, r::binary>>) when c < 0x80, do: [<<c>> | esc_bin(r)]
  defp esc_bin(<<c::utf8, r::binary>>), do: [<<c::utf8>> | esc_bin(r)]
  defp hex2(c) do
    Integer.to_string(c, 16) |> String.pad_leading(2, "0") |> String.downcase()
  end
  def utf16_len(s), do: utf16_len(s, 0)
  defp utf16_len(<<>>, acc), do: acc
  defp utf16_len(<<c::utf8, r::binary>>, acc) when c > 0xFFFF, do: utf16_len(r, acc + 2)
  defp utf16_len(<<_::utf8, r::binary>>, acc), do: utf16_len(r, acc + 1)
  def format_created_at(%DateTime{time_zone: tz} = dt) when tz != "Etc/UTC" do
    dt |> DateTime.shift_zone!("Etc/UTC") |> format_created_at()
  end
  def format_created_at(%DateTime{} = dt) do
    ms = div(elem(dt.microsecond, 0), 1000)
    [pad4(dt.year), ?-, pad2(dt.month), ?-, pad2(dt.day), ?T,
     pad2(dt.hour), ?:, pad2(dt.minute), ?:, pad2(dt.second), ?.,
     pad3(ms), ?Z] |> IO.iodata_to_binary()
  end
  def format_created_at(%NaiveDateTime{} = ndt) do
    ms = div(elem(ndt.microsecond, 0), 1000)
    [pad4(ndt.year), ?-, pad2(ndt.month), ?-, pad2(ndt.day), ?T,
     pad2(ndt.hour), ?:, pad2(ndt.minute), ?:, pad2(ndt.second), ?.,
     pad3(ms), ?Z] |> IO.iodata_to_binary()
  end
  defp pad2(n) when n < 10, do: [?0, ?0 + n]
  defp pad2(n), do: Integer.to_string(n)
  defp pad3(n) when n < 10, do: [?0, ?0, ?0 + n]
  defp pad3(n) when n < 100, do: [?0, ?0 + div(n, 10), ?0 + rem(n, 10)]
  defp pad3(n), do: Integer.to_string(n)
  defp pad4(n), do: Integer.to_string(n) |> String.pad_leading(4, "0")
end
