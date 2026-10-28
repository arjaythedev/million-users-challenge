/// The response bodies, written by hand as bytes (compact JSON, SPEC.md's key order). Values come straight from the
/// Postgres row (binary columns); strings are escaped (`"`, `\` and control characters), non-ASCII copied as is.
module Json

open System
open System.Buffers
open System.Buffers.Text

let private escapeSet = SearchValues.Create(ReadOnlySpan(Array.init 32 byte |> Array.append [| '"'B; '\\'B |]))
let private hex = "0123456789abcdef"B

/// Where JSON is written: the request's response buffer, pinned (io_uring sends from it), grown on demand. The body
/// starts at `start`, after room for the headers. A sealed class with plain fields: no virtual calls, no closures.
[<Sealed; AllowNullLiteral>]
type Out(size: int, start: int) =
    let mutable wire = GC.AllocateUninitializedArray<byte>(size, pinned = true)
    let mutable n = start
    member _.Wire
        with get () = wire
        and set v = wire <- v
    /// bytes written so far, headers' room included
    member _.N
        with get () = n
        and set v = n <- v
    /// a span of at least `size` free bytes at the end
    member _.Reserve(size: int) : Span<byte> =
        if wire.Length - n < size then
            let bigger = GC.AllocateUninitializedArray<byte>(max (wire.Length * 2) (n + size + 1024), pinned = true)
            Buffer.BlockCopy(wire, 0, bigger, 0, n)
            wire <- bigger
        Span<byte>(wire, n, wire.Length - n)
    member _.Advance(count: int) = n <- n + count
    /// the last day putTime formatted in this response (days since 1970) and its "YYYY-MM-DD" packed as y*10000+m*100+d:
    /// a feed's posts mostly share a day, so the calendar math runs once per response, not once per post
    member val LastDay = Int64.MinValue with get, set
    member val LastYmd = 0 with get, set

let inline put (o: Out) (s: ReadOnlySpan<byte>) =
    s.CopyTo(o.Reserve s.Length)
    o.Advance s.Length

let inline putByte (o: Out) (b: byte) =
    (o.Reserve 1).[0] <- b
    o.Advance 1

/// v at s[p..]; returns the new position
let inline private intAt (s: Span<byte>) (p: int) (v: int64) =
    let mutable n = 0
    Utf8Formatter.TryFormat(v, s.Slice p, &n) |> ignore
    p + n

let putInt (o: Out) (v: int64) = o.Advance(intAt (o.Reserve 24) 0 v)

/// a JSON string with its quotes at s[p..] (room for 6 bytes per input byte + 2); returns the new position
let private stringAt (s: Span<byte>) (p0: int) (v: ReadOnlySpan<byte>) =
    let mutable p = p0
    s.[p] <- '"'B
    p <- p + 1
    let mutable rest = v
    let mutable go = true
    while go do
        let i = rest.IndexOfAny escapeSet
        if i < 0 then
            rest.CopyTo(s.Slice p)
            p <- p + rest.Length
            go <- false
        else
            rest.Slice(0, i).CopyTo(s.Slice p)
            p <- p + i
            let b = rest.[i]
            s.[p] <- '\\'B
            match b with
            | 34uy -> s.[p + 1] <- '"'B; p <- p + 2
            | 92uy -> s.[p + 1] <- '\\'B; p <- p + 2
            | 10uy -> s.[p + 1] <- 'n'B; p <- p + 2
            | 13uy -> s.[p + 1] <- 'r'B; p <- p + 2
            | 9uy -> s.[p + 1] <- 't'B; p <- p + 2
            | _ ->
                s.[p + 1] <- 'u'B
                s.[p + 2] <- '0'B
                s.[p + 3] <- '0'B
                s.[p + 4] <- hex.[int (b >>> 4)]
                s.[p + 5] <- hex.[int (b &&& 15uy)]
                p <- p + 6
            rest <- rest.Slice(i + 1)
    s.[p] <- '"'B
    p + 1

/// a JSON string with its quotes
let putString (o: Out) (v: ReadOnlySpan<byte>) =
    o.Advance(stringAt (o.Reserve(v.Length * 6 + 2)) 0 v)

let inline private two (s: Span<byte>) (at: int) (v: int) =
    s.[at] <- byte (48 + v / 10)
    s.[at + 1] <- byte (48 + v % 10)

/// days since 1970-01-01 -> y*10000+m*100+d (civil_from_days, H. Hinnant)
let private civil (days: int64) =
    let z = days + 719468L
    let era = (if z >= 0L then z else z - 146096L) / 146097L
    let doe = z - era * 146097L
    let yoe = (doe - doe / 1460L + doe / 36524L - doe / 146096L) / 365L
    let doy = doe - (365L * yoe + yoe / 4L - yoe / 100L)
    let mp = (5L * doy + 2L) / 153L
    let d = int (doy - (153L * mp + 2L) / 5L + 1L)
    let m = int (if mp < 10L then mp + 3L else mp - 9L)
    let y = int (yoe + era * 400L + (if m <= 2 then 1L else 0L))
    y * 10000 + m * 100 + d

/// a Postgres timestamptz (microseconds since 2000-01-01 UTC) as "YYYY-MM-DDTHH:MM:SS.mmmZ", in UTC, truncated
/// (not rounded) to milliseconds, with its quotes, at s[p..] (26 bytes); returns the new position
let private timeAt (o: Out) (s: Span<byte>) (p: int) (micros: int64) =
    let ms = (if micros >= 0L then micros / 1000L else (micros - 999L) / 1000L) + 946_684_800_000L
    let mutable days = ms / 86_400_000L
    let mutable rem = ms % 86_400_000L
    if rem < 0L then
        rem <- rem + 86_400_000L
        days <- days - 1L
    if days <> o.LastDay then
        o.LastYmd <- civil days
        o.LastDay <- days
    let ymd = o.LastYmd
    let y = ymd / 10000
    let r = int rem
    s.[p] <- '"'B
    two s (p + 1) (y / 100)
    two s (p + 3) (y % 100)
    s.[p + 5] <- '-'B
    two s (p + 6) (ymd / 100 % 100)
    s.[p + 8] <- '-'B
    two s (p + 9) (ymd % 100)
    s.[p + 11] <- 'T'B
    two s (p + 12) (r / 3_600_000)
    s.[p + 14] <- ':'B
    two s (p + 15) (r / 60_000 % 60)
    s.[p + 17] <- ':'B
    two s (p + 18) (r / 1000 % 60)
    s.[p + 20] <- '.'B
    let f = r % 1000
    s.[p + 21] <- byte (48 + f / 100)
    two s (p + 22) (f % 100)
    s.[p + 24] <- 'Z'B
    s.[p + 25] <- '"'B
    p + 26

let putTime (o: Out) (micros: int64) = o.Advance(timeAt o (o.Reserve 26) 0 micros)

let private kId = "{\"id\":"B
let private kBody = ",\"body\":"B
let private kCreated = ",\"created_at\":"B
let private kAuthor = ",\"author\":"B
let private kLikes = ",\"like_count\":"B

let inline private lit (s: Span<byte>) (p: int) (k: byte[]) =
    ReadOnlySpan(k).CopyTo(s.Slice p)
    p + k.Length

/// {"id":…,"body":…,"created_at":…,"author":…,"like_count":…}, written into one reserved span
let putPost (o: Out) (id: int64) (body: ReadOnlySpan<byte>) (micros: int64) (author: ReadOnlySpan<byte>) (likes: int64) =
    let s = o.Reserve((body.Length + author.Length) * 6 + 160)
    let mutable p = lit s 0 kId
    p <- intAt s p id
    p <- lit s p kBody
    p <- stringAt s p body
    p <- lit s p kCreated
    p <- timeAt o s p micros
    p <- lit s p kAuthor
    p <- stringAt s p author
    p <- lit s p kLikes
    p <- intAt s p likes
    s.[p] <- '}'B
    o.Advance(p + 1)
