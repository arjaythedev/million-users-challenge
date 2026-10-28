/// The response bodies, written by hand as bytes (compact JSON, SPEC.md's key order). Values come straight from the
/// Postgres row (binary columns); strings are escaped (`"`, `\` and control characters), non-ASCII copied as is.
module Json

open System
open System.Buffers
open System.Buffers.Text

let private escapeSet = SearchValues.Create(ReadOnlySpan(Array.init 32 byte |> Array.append [| '"'B; '\\'B |]))
let private hex = "0123456789abcdef"B

/// Where JSON is written: a growable byte buffer (the request's response buffer).
[<AbstractClass>]
type Out() =
    /// a span of at least `size` free bytes at the end
    abstract Reserve: size: int -> Span<byte>
    abstract Advance: count: int -> unit

let inline put (o: Out) (s: ReadOnlySpan<byte>) =
    s.CopyTo(o.Reserve s.Length)
    o.Advance s.Length

let inline putByte (o: Out) (b: byte) =
    (o.Reserve 1).[0] <- b
    o.Advance 1

let putInt (o: Out) (v: int64) =
    let s = o.Reserve 24
    let mutable n = 0
    Utf8Formatter.TryFormat(v, s, &n) |> ignore
    o.Advance n

/// a JSON string with its quotes
let putString (o: Out) (v: ReadOnlySpan<byte>) =
    putByte o '"'B
    let mutable rest = v
    let mutable go = true
    while go do
        let i = rest.IndexOfAny escapeSet
        if i < 0 then
            put o rest
            go <- false
        else
            put o (rest.Slice(0, i))
            let b = rest.[i]
            let s = o.Reserve 6
            match b with
            | 34uy -> s.[0] <- '\\'B; s.[1] <- '"'B; o.Advance 2
            | 92uy -> s.[0] <- '\\'B; s.[1] <- '\\'B; o.Advance 2
            | 10uy -> s.[0] <- '\\'B; s.[1] <- 'n'B; o.Advance 2
            | 13uy -> s.[0] <- '\\'B; s.[1] <- 'r'B; o.Advance 2
            | 9uy -> s.[0] <- '\\'B; s.[1] <- 't'B; o.Advance 2
            | _ ->
                s.[0] <- '\\'B
                s.[1] <- 'u'B
                s.[2] <- '0'B
                s.[3] <- '0'B
                s.[4] <- hex.[int (b >>> 4)]
                s.[5] <- hex.[int (b &&& 15uy)]
                o.Advance 6
            rest <- rest.Slice(i + 1)
    putByte o '"'B

let inline private two (s: Span<byte>) (at: int) (v: int) =
    s.[at] <- byte (48 + v / 10)
    s.[at + 1] <- byte (48 + v % 10)

/// a Postgres timestamptz (microseconds since 2000-01-01 UTC) as "YYYY-MM-DDTHH:MM:SS.mmmZ", in UTC, truncated
/// (not rounded) to milliseconds, with its quotes
let putTime (o: Out) (micros: int64) =
    let ms = (if micros >= 0L then micros / 1000L else (micros - 999L) / 1000L) + 946_684_800_000L
    let mutable days = ms / 86_400_000L
    let mutable rem = ms % 86_400_000L
    if rem < 0L then
        rem <- rem + 86_400_000L
        days <- days - 1L
    // civil_from_days (H. Hinnant)
    let z = days + 719468L
    let era = (if z >= 0L then z else z - 146096L) / 146097L
    let doe = z - era * 146097L
    let yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L
    let doy = doe - (365L * yoe + yoe / 4L - yoe / 100L)
    let mp = (5L * doy + 2L) / 153L
    let d = int (doy - (153L * mp + 2L) / 5L + 1L)
    let m = int (if mp < 10L then mp + 3L else mp - 9L)
    let y = int (yoe + era * 400L + (if m <= 2 then 1L else 0L))
    let r = int rem
    let s = o.Reserve 26
    s.[0] <- '"'B
    two s 1 (y / 100)
    two s 3 (y % 100)
    s.[5] <- '-'B
    two s 6 m
    s.[8] <- '-'B
    two s 9 d
    s.[11] <- 'T'B
    two s 12 (r / 3_600_000)
    s.[14] <- ':'B
    two s 15 (r / 60_000 % 60)
    s.[17] <- ':'B
    two s 18 (r / 1000 % 60)
    s.[20] <- '.'B
    let f = r % 1000
    s.[21] <- byte (48 + f / 100)
    two s 22 (f % 100)
    s.[24] <- 'Z'B
    s.[25] <- '"'B
    o.Advance 26

let private kId = "{\"id\":"B
let private kBody = ",\"body\":"B
let private kCreated = ",\"created_at\":"B
let private kAuthor = ",\"author\":"B
let private kLikes = ",\"like_count\":"B

/// {"id":…,"body":…,"created_at":…,"author":…,"like_count":…}
let putPost (o: Out) (id: int64) (body: ReadOnlySpan<byte>) (micros: int64) (author: ReadOnlySpan<byte>) (likes: int64) =
    put o (ReadOnlySpan kId)
    putInt o id
    put o (ReadOnlySpan kBody)
    putString o body
    put o (ReadOnlySpan kCreated)
    putTime o micros
    put o (ReadOnlySpan kAuthor)
    putString o author
    put o (ReadOnlySpan kLikes)
    putInt o likes
    putByte o '}'B
