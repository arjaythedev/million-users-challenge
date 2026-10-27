use pg = "postgres"
use "crypto"
use "encode/base64"
use "json"
use "time"

use "lib:deflate"
use @libdeflate_alloc_compressor[Pointer[_Compressor]](level: I32)
use @libdeflate_gzip_compress_bound[USize](c: Pointer[_Compressor] tag, n: USize)
use @libdeflate_gzip_compress[USize](c: Pointer[_Compressor] tag, data: Pointer[U8] tag, n: USize,
  out: Pointer[U8] tag, avail: USize)
use @libdeflate_free_compressor[None](c: Pointer[_Compressor] tag)

primitive _Compressor

primitive _Gzip
  """
  gzip at level 1 with libdeflate. A compressor is allocated per call: keeping one per connection would cost
  more memory than a million connections can afford.
  """
  fun apply(data: String): (Array[U8] val | None) =>
    let c = @libdeflate_alloc_compressor(1)
    if c.is_null() then return None end
    let bound = @libdeflate_gzip_compress_bound(c, data.size())
    let out = recover iso Array[U8].>undefined(bound) end
    let n = @libdeflate_gzip_compress(c, data.cpointer(), data.size(), out.cpointer(), bound)
    @libdeflate_free_compressor(c)
    if n == 0 then return None end
    out.truncate(n)
    consume out

primitive _Http
  fun reason(status: U16): String =>
    match status
    | 200 => "OK"
    | 201 => "Created"
    | 400 => "Bad Request"
    | 401 => "Unauthorized"
    | 404 => "Not Found"
    | 503 => "Service Unavailable"
    else "Internal Server Error"
    end

  fun response(status: U16, body: String): String =>
    let reason' = reason(status)
    recover val
      let out = String(body.size() + 96)
      out.append("HTTP/1.1 ")
      out.append(status.string())
      out.push(' ')
      out.append(reason')
      out.append("\r\nContent-Type: application/json\r\nContent-Length: ")
      out.append(body.size().string())
      out.append("\r\n\r\n")
      out.append(body)
      out
    end

  fun gzip_response(status: U16, body: Array[U8] val): Array[U8] val =>
    let reason' = reason(status)
    recover val
      let head = String(160)
      head.append("HTTP/1.1 ")
      head.append(status.string())
      head.push(' ')
      head.append(reason')
      head.append("\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding")
      head.append("\r\nContent-Length: ")
      head.append(body.size().string())
      head.append("\r\n\r\n")
      let out = Array[U8](head.size() + body.size())
      out.append(head)
      out.append(body)
      out
    end

primitive _Json
  """
  Response JSON, written by hand in the spec's key order.
  """
  fun post(out: String ref, row: pg.Row) ? =>
    let f = row.fields
    post_fields(out, f(0)?.value as I64, f(1)?.value as String, f(2)?.value as pg.PgTimestamp,
      f(3)?.value as String, (f(4)?.value as I32).i64())

  fun post_fields(out: String ref, id: I64, body: String box, created: pg.PgTimestamp, author: String box,
    like_count: I64)
  =>
    out.append("{\"id\":")
    out.append(id.string())
    out.append(",\"body\":")
    string(out, body)
    out.append(",\"created_at\":\"")
    _Iso8601(out, created.microseconds)
    out.append("\",\"author\":")
    string(out, author)
    out.append(",\"like_count\":")
    out.append(like_count.string())
    out.push('}')

  fun string(out: String ref, s: String box) =>
    """
    A JSON string, escaped like JavaScript's JSON.stringify: quote, backslash and control characters.
    """
    out.push('"')
    for c in s.values() do
      match c
      | '"' => out.append("\\\"")
      | '\\' => out.append("\\\\")
      | '\n' => out.append("\\n")
      | '\r' => out.append("\\r")
      | '\t' => out.append("\\t")
      | 0x08 => out.append("\\b")
      | 0x0C => out.append("\\f")
      else
        if c < 0x20 then
          out.append("\\u00")
          out.push(_hex(c >> 4))
          out.push(_hex(c and 0x0F))
        else
          out.push(c)
        end
      end
    end
    out.push('"')

  fun _hex(n: U8): U8 => if n < 10 then '0' + n else 'a' + (n - 10) end

primitive _Iso8601
  """
  A Postgres timestamp (microseconds since 2000-01-01 UTC) as 2025-12-31T23:59:19.409Z, truncated (not rounded)
  to milliseconds.
  """
  fun apply(out: String ref, pg_micros: I64) =>
    let unix_ms = (pg_micros / 1000) + 946_684_800_000
    let ms_of_day = unix_ms % 86_400_000
    // civil_from_days (Howard Hinnant)
    let z = (unix_ms / 86_400_000) + 719_468
    let era = (if z >= 0 then z else z - 146_096 end) / 146_097
    let doe = z - (era * 146_097)
    let yoe = (((doe - (doe / 1460)) + (doe / 36_524)) - (doe / 146_096)) / 365
    let doy = doe - (((365 * yoe) + (yoe / 4)) - (yoe / 100))
    let mp = ((5 * doy) + 2) / 153
    let day = (doy - (((153 * mp) + 2) / 5)) + 1
    let month = if mp < 10 then mp + 3 else mp - 9 end
    let year = (yoe + (era * 400)) + (if month <= 2 then 1 else 0 end)
    _pad(out, year, 4)
    out.push('-')
    _pad(out, month, 2)
    out.push('-')
    _pad(out, day, 2)
    out.push('T')
    _pad(out, ms_of_day / 3_600_000, 2)
    out.push(':')
    _pad(out, (ms_of_day / 60_000) % 60, 2)
    out.push(':')
    _pad(out, (ms_of_day / 1000) % 60, 2)
    out.push('.')
    _pad(out, ms_of_day % 1000, 3)
    out.push('Z')

  fun _pad(out: String ref, value: I64, width: USize) =>
    let digits = value.string()
    var n = digits.size()
    while n < width do
      out.push('0')
      n = n + 1
    end
    out.append(consume digits)

primitive _Ids
  """
  A positive integer written in digits, or None (0, -1, abc, 1.5, beyond 2^53).
  """
  fun apply(s: String box): (I64 | None) =>
    if (s.size() == 0) or (s.size() > 16) then return None end
    var n: I64 = 0
    for c in s.values() do
      if (c < '0') or (c > '9') then return None end
      n = (n * 10) + (c - '0').i64()
    end
    if (n > 0) and (n <= 9_007_199_254_740_991) then n else None end

primitive Jwt
  """
  HS256 only. The signature and exp are checked on every call; nothing is remembered.
  """
  fun verify(token: String, secret: String): (JSONObject | None) =>
    try
      let parts = token.split_by(".")
      if parts.size() != 3 then error end
      let header = _object(parts(0)?)?
      if (header("alg")? as String) != "HS256" then error end
      let signed: String val = parts(0)? + "." + parts(1)?
      let expected: String val = Base64.encode_url(HmacSha256(secret, signed)?)
      if not ConstantTimeCompare(expected, parts(2)?) then error end
      let claims = _object(parts(1)?)?
      (let sec, let nsec) = Time.now()
      let now = sec.f64() + (nsec.f64() / 1e9)
      // exp is required and must be in the future; nbf, when present, must not be
      if _number(claims("exp")?)? <= now then error end
      if claims.contains("nbf") and (_number(claims("nbf")?)? > now) then error end
      claims
    else
      None
    end

  fun _object(segment: String): JSONObject ? =>
    let json: String val = String.from_iso_array(Base64.decode_url(segment)?)
    match JSONParser.parse(json)
    | let o: JSONObject => o
    else error
    end

  fun _number(value: JSONValue): F64 ? =>
    match value
    | let i: I64 => i.f64()
    | let f: F64 => f
    else error
    end
