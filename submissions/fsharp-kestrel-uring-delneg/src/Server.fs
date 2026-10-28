/// The server: one I/O thread per core (Environment.ProcessorCount follows the affinity mask, so 16, 8 and 4 cores
/// size themselves), nothing shared on the request path. Each thread owns:
/// - one io_uring ring (SINGLE_ISSUER | DEFER_TASKRUN | COOP_TASKRUN);
/// - its own SO_REUSEPORT listening sockets on both ports, each with a multishot accept straight into the ring's
///   fixed-file table (client sockets never get a process fd);
/// - one multishot recv per client socket into provided buffers: an idle connection owns no buffer, a request is
///   parsed in place and the buffer goes straight back (only a request split over several receives is copied);
/// - its own Postgres connections (own v3 client, Pg.fs) in the same ring: reads and writes on separate connections,
///   every statement prepared once per connection, the queries of one pass pipelined into one send per connection;
/// - per-connection state as a few small arrays indexed by the fixed-file slot (no object per connection).
module Server

#nowarn "9"

open System
open System.Buffers
open System.Buffers.Binary
open System.Buffers.Text
open System.Collections.Concurrent
open System.Collections.Generic
open System.Net
open System.Net.Sockets
open System.Runtime.InteropServices
open System.Text
open System.Text.Json
open System.Threading
open Microsoft.FSharp.NativeInterop
open Pg
open Ring

// ---------- settings ----------

let private env (name: string) (fallback: int) =
    match Int32.TryParse(Environment.GetEnvironmentVariable name) with
    | true, v when v >= 0 -> v
    | _ -> fallback

let private started = Diagnostics.Stopwatch.StartNew()

// ---------- statements (SPEC.md, one per request) ----------

let private postSelect =
    "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id"

let mutable private sHealth: Stmt = null
let mutable private sFeed: Stmt = null
let mutable private sPost: Stmt = null
let mutable private sCreate: Stmt = null
let mutable private sLike: Stmt = null

let defineStatements () =
    sHealth <- Stmt.Define("SELECT 1")
    sFeed <- Stmt.Define(postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
    sPost <- Stmt.Define(postSelect + " WHERE p.id = $1", Oid.Int8)
    sCreate <- Stmt.Define("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", Oid.Int8, Oid.Text)
    sLike <- Stmt.Define("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", Oid.Int8, Oid.Int8)

// ---------- gzip (native/gz.c: libdeflate or ISA-L igzip, built from source by build.sh) ----------

module Gz =
    [<DllImport("libgz", CallingConvention = CallingConvention.Cdecl)>]
    extern nativeint gz_new(int impl, int level, int train)

    [<DllImport("libgz", CallingConvention = CallingConvention.Cdecl)>]
    extern unativeint gz_compress(nativeint g, nativeint input, unativeint inLen, nativeint output, unativeint outCap)

    [<DllImport("libgz", CallingConvention = CallingConvention.Cdecl)>]
    extern unativeint gz_bound(nativeint g, unativeint inLen)

/// GZIP_IMPL: igzip (ISA-L, default, level 0) or libdeflate (level 1). igzip level 0 cost ~40% fewer user cycles per
/// request than libdeflate level 1 (experiments e2) for a 2,243-byte feed instead of 1,901 (still two segments).
let private gzipImpl = if Environment.GetEnvironmentVariable "GZIP_IMPL" = "libdeflate" then 0 else 1
let private gzipLevel = env "GZIP_LEVEL" (if gzipImpl = 1 then 0 else 1)
let private gzipMin = env "GZIP_MIN" 512
/// igzip level 0: rebuild the Huffman table about once a second from a body's symbol histogram (GZIP_TRAIN=1)
let private gzipTrain = env "GZIP_TRAIN" 0

// ---------- responses ----------

type Route =
    | None = 0
    | Health = 1
    | Feed = 2
    | Post = 3
    | Create = 4
    | Like = 5

[<Literal>]
let private Reserve = 192

let private status200 = "HTTP/1.1 200 OK\r\n"B
let private status201 = "HTTP/1.1 201 Created\r\n"B
let private status400 = "HTTP/1.1 400 Bad Request\r\n"B
let private status401 = "HTTP/1.1 401 Unauthorized\r\n"B
let private status404 = "HTTP/1.1 404 Not Found\r\n"B
let private status413 = "HTTP/1.1 413 Content Too Large\r\n"B
let private status431 = "HTTP/1.1 431 Request Header Fields Too Large\r\n"B
let private status500 = "HTTP/1.1 500 Internal Server Error\r\n"B
let private status503 = "HTTP/1.1 503 Service Unavailable\r\n"B
let private hType = "Content-Type: application/json\r\n"B
let private hGzip = "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"B
let private hLength = "Content-Length: "B
let private hClose = "\r\nConnection: close\r\n\r\n"B
let private hKeepAlive = "\r\nConnection: keep-alive\r\n\r\n"B
let private hEnd = "\r\n\r\n"B

let private jFeedOpen = "{\"posts\":["B
let private jFeedClose = "]}"B
let private jPostOpen = "{\"post\":"B
let private jLikeTrue = "{\"liked\":true,\"already_liked\":true,\"post_id\":"B
let private jLikeFalse = "{\"liked\":true,\"already_liked\":false,\"post_id\":"B
let private jHealthOk = "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"B
let private jHealthBad = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"B
let private jError = "{\"error\":"B
let private bInsert1 = "INSERT 0 1"B

let private eNotFound = "not found"B
let private eInvalidId = "invalid post id"B
let private ePostNotFound = "post not found"B
let private eMissingBearer = "missing bearer token"B
let private eInvalidToken = "invalid or expired token"B
let private eBadPayload = "invalid token payload"B
let private eMalformed = "malformed JSON body"B
let private eBodyRequired = "body is required"B
let private eBodyTooLong = "body must be at most 500 characters"B
let private eInternal = "internal server error"B
let private eBadRequest = "bad request"B
let private eTooLarge = "request too large"B

/// One request: what was asked, the response being built (in a pinned buffer that io_uring sends from) and the
/// Postgres query it runs. Pooled per thread; the read paths allocate nothing per request.
[<Sealed; AllowNullLiteral>]
type Ctx(id: int, finish: Ctx -> unit) =
    inherit PgQuery()
    let mutable gz = GC.AllocateUninitializedArray<byte>(4 * 1024, pinned = true)
    let out = Json.Out(8 * 1024, Reserve)
    member val Id = id
    member val Slot = 0 with get, set
    member val Gen = 0u with get, set
    member val CloseAfter = false with get, set
    member val KeepAlive10 = false with get, set
    member val Gzip = false with get, set
    member val Route = Route.None with get, set
    member val Status = 200 with get, set
    member val Found = false with get, set
    member val PostId = 0L with get, set
    member val UserId = 0L with get, set
    member val Name: byte[] = null with get, set
    member val Body: byte[] = null with get, set
    member val InUse = false with get, set
    // the bytes to send: buffer, start, length, and how much went out
    member val SendBuf: byte[] = null with get, set
    member val SendStart = 0 with get, set
    member val SendLen = 0 with get, set
    member val SentOff = 0 with get, set
    member _.Out = out

    member c.Reset() =
        out.N <- Reserve
        c.CloseAfter <- false
        c.KeepAlive10 <- false
        c.Gzip <- false
        c.Route <- Route.None
        c.Status <- 200
        c.Found <- false
        c.PostId <- 0L
        c.UserId <- 0L
        c.Name <- null
        c.Body <- null
        c.SentOff <- 0
        if out.Wire.Length > 64 * 1024 then out.Wire <- GC.AllocateUninitializedArray<byte>(8 * 1024, pinned = true)
        if gz.Length > 64 * 1024 then gz <- GC.AllocateUninitializedArray<byte>(4 * 1024, pinned = true)

    member _.ClearBody() = out.N <- Reserve

    member c.Error(status: int, message: ReadOnlySpan<byte>) =
        out.N <- Reserve
        c.Status <- status
        Json.put out (ReadOnlySpan jError)
        Json.putString out message
        Json.putByte out '}'B

    /// Writes the headers in front of the body (gzipped first when asked and worth it); sets SendBuf/Start/Len.
    member c.Seal(compressor: nativeint) =
        let wire = out.Wire
        let bodyLen = out.N - Reserve
        let mutable buf = wire
        let mutable len = bodyLen
        if c.Gzip && bodyLen >= gzipMin && compressor <> 0n then
            let need = int (Gz.gz_bound (compressor, unativeint bodyLen)) + Reserve
            if gz.Length < need then gz <- GC.AllocateUninitializedArray<byte>(max need (gz.Length * 2), pinned = true)
            let z =
                Gz.gz_compress (
                    compressor,
                    Marshal.UnsafeAddrOfPinnedArrayElement(wire, Reserve),
                    unativeint bodyLen,
                    Marshal.UnsafeAddrOfPinnedArrayElement(gz, Reserve),
                    unativeint (gz.Length - Reserve)
                )
            if z <> 0un then
                buf <- gz
                len <- int z
            else c.Gzip <- false
        else c.Gzip <- false
        let line =
            match c.Status with
            | 200 -> status200
            | 201 -> status201
            | 400 -> status400
            | 401 -> status401
            | 404 -> status404
            | 413 -> status413
            | 431 -> status431
            | 503 -> status503
            | _ -> status500
        let st = NativePtr.stackalloc<byte> Reserve
        let s = Span<byte>(NativePtr.toVoidPtr st, Reserve)
        let mutable k = 0
        line.AsSpan().CopyTo(s)
        k <- line.Length
        hType.AsSpan().CopyTo(s.Slice k)
        k <- k + hType.Length
        if c.Gzip then
            hGzip.AsSpan().CopyTo(s.Slice k)
            k <- k + hGzip.Length
        hLength.AsSpan().CopyTo(s.Slice k)
        k <- k + hLength.Length
        let mutable w = 0
        Utf8Formatter.TryFormat(len, s.Slice k, &w) |> ignore
        k <- k + w
        let tail = if c.CloseAfter then hClose elif c.KeepAlive10 then hKeepAlive else hEnd
        tail.AsSpan().CopyTo(s.Slice k)
        k <- k + tail.Length
        s.Slice(0, k).CopyTo(Span<byte>(buf, Reserve - k, k))
        c.SendBuf <- buf
        c.SendStart <- Reserve - k
        c.SendLen <- k + len

    // ---------- the query ----------

    override c.Write(w: PgWriter) =
        match c.Route with
        | Route.Health ->
            w.Bind sHealth
            w.EndBind()
        | Route.Feed ->
            w.Bind sFeed
            w.EndBind()
        | Route.Post ->
            w.Bind sPost
            w.Int8 c.PostId
            w.EndBind()
        | Route.Create ->
            w.Bind sCreate
            w.Int8 c.UserId
            w.Text(ReadOnlySpan c.Body)
            w.EndBind()
        | _ ->
            w.Bind sLike
            w.Int8 c.UserId
            w.Int8 c.PostId
            w.EndBind()

    override c.Row(_, row) =
        match c.Route with
        | Route.Feed ->
            if c.Found then Json.putByte out ','B
            c.Found <- true
            Json.putPost out (row.Int8 0) (row.Bytes 1) (row.Micros 2) (row.Bytes 3) (int64 (row.Int4 4))
        | Route.Post ->
            c.Found <- true
            Json.put out (ReadOnlySpan jPostOpen)
            Json.putPost out (row.Int8 0) (row.Bytes 1) (row.Micros 2) (row.Bytes 3) (int64 (row.Int4 4))
            Json.putByte out '}'B
        | Route.Create ->
            c.Found <- true
            Json.put out (ReadOnlySpan jPostOpen)
            Json.putPost out (row.Int8 0) (ReadOnlySpan c.Body) (row.Micros 1) (ReadOnlySpan c.Name) 0L
            Json.putByte out '}'B
        | _ -> ()

    override c.Complete(_, tag) =
        if c.Route = Route.Like then c.Found <- tag.SequenceEqual(ReadOnlySpan bInsert1)

    /// sent again after a rolled-back or skipped batch: back to the body as it was when the query was submitted
    override c.Retry() =
        c.Found <- false
        if c.Route = Route.Feed then c.BeginFeed() else c.ClearBody()

    override c.Done(err) =
        if isNull err then
            match c.Route with
            | Route.Health ->
                c.Status <- 200
                Json.put out (ReadOnlySpan jHealthOk)
                Json.putInt out (int64 started.Elapsed.TotalSeconds)
                Json.putByte out '}'B
            | Route.Feed ->
                Json.put out (ReadOnlySpan jFeedClose)
            | Route.Post ->
                if not c.Found then c.Error(404, ReadOnlySpan ePostNotFound)
            | Route.Create ->
                if c.Found then c.Status <- 201 else c.Error(500, ReadOnlySpan eInternal)
            | _ ->
                c.Status <- if c.Found then 201 else 200
                Json.put out (ReadOnlySpan(if c.Found then jLikeFalse else jLikeTrue))
                Json.putInt out c.PostId
                Json.putByte out '}'B
        else
            match c.Route with
            | Route.Health -> c.Unhealthy err.Message
            | Route.Like when err.SqlState = "23503" -> c.Error(404, ReadOnlySpan ePostNotFound)
            | _ ->
                if c.Route <> Route.Health then eprintfn "postgres error on %O: %O" c.Route err
                c.Error(500, ReadOnlySpan eInternal)
        finish c

    member c.Unhealthy(message: string) =
        out.N <- Reserve
        c.Status <- 503
        Json.put out (ReadOnlySpan jHealthBad)
        Json.putString out (ReadOnlySpan(Encoding.UTF8.GetBytes message))
        Json.putByte out '}'B

    /// starts the body of a route whose rows are appended (the feed)
    member c.BeginFeed() =
        out.N <- Reserve
        Json.put out (ReadOnlySpan jFeedOpen)

// ---------- HTTP parsing helpers ----------

let private bCrlf2 = "\r\n\r\n"B
let private bCrlf = "\r\n"B
let private bHttp10 = "HTTP/1.0"B
let private bGet = "GET"B
let private bPost = "POST"B
let private pHealth = "/health"B
let private pFeed = "/feed"B
let private pPosts = "/posts"B
let private pPostsSlash = "/posts/"B
let private pLike = "/like"B
let private bBearer = "Bearer "B
let private hContentLength = "content-length"B
let private hAuthorization = "authorization"B
let private hConnection = "connection"B
let private hTransferEncoding = "transfer-encoding"B
let private hExpect = "expect"B
let private hAcceptEncoding = "accept-encoding"B
let private vClose = "close"B
let private vKeepAlive = "keep-alive"B
let private vGzip = "gzip"B
let private continue100 = GC.AllocateArray<byte>(25, pinned = true)
do "HTTP/1.1 100 Continue\r\n\r\n"B.CopyTo(continue100, 0)

let private isName (name: ReadOnlySpan<byte>) (lower: byte[]) =
    if name.Length <> lower.Length then false
    else
        let mutable ok = true
        let mutable i = 0
        while ok && i < name.Length do
            let mutable b = name.[i]
            if b >= 'A'B && b <= 'Z'B then b <- b + 32uy
            if b <> lower.[i] then ok <- false
            i <- i + 1
        ok

let private contains (value: ReadOnlySpan<byte>) (lower: byte[]) =
    let mutable found = false
    let mutable i = 0
    while not found && i + lower.Length <= value.Length do
        if isName (value.Slice(i, lower.Length)) lower then found <- true
        i <- i + 1
    found

/// a positive post id in decimal digits (at most 18), else 0
let private parseId (s: ReadOnlySpan<byte>) =
    if s.Length = 0 || s.Length > 18 then 0L
    else
        let mutable v = 0L
        let mutable ok = true
        for b in s do
            let d = uint32 (b - '0'B)
            if d > 9u then ok <- false else v <- v * 10L + int64 d
        if ok then v else 0L

/// POST /posts body: null when valid (`body` = the trimmed text as UTF-8), else the error message
let private parseCreateBody (data: ReadOnlySpan<byte>) (body: byref<byte[]>) : byte[] =
    let doc =
        try JsonDocument.Parse(ReadOnlyMemory<byte>(data.ToArray()))
        with :? JsonException -> null
    if isNull doc then eMalformed
    else
        use d = doc
        let root = d.RootElement
        let mutable v = Unchecked.defaultof<JsonElement>
        if root.ValueKind <> JsonValueKind.Object || not (root.TryGetProperty("body", &v)) || v.ValueKind <> JsonValueKind.String then
            eBodyRequired
        else
            let s = v.GetString().Trim()
            if s.Length = 0 then eBodyRequired
            else
                // characters = code points (Postgres char_length)
                let mutable chars = 0
                for ch in s do
                    if not (Char.IsLowSurrogate ch) then chars <- chars + 1
                if chars > 500 then eBodyTooLong
                else
                    body <- Encoding.UTF8.GetBytes s
                    null

// ---------- the I/O threads ----------

// user_data: op (3 bits) | slot (20) | gen (23) | extra (18: the request's id in the thread's pool)
[<Literal>]
let private OpAccept = 1UL
[<Literal>]
let private OpRecv = 2UL
[<Literal>]
let private OpSend = 3UL
[<Literal>]
let private OpEvent = 4UL
[<Literal>]
let private OpIgnore = 5UL
[<Literal>]
let private OpPgRecv = 6UL
[<Literal>]
let private OpPgSend = 7UL

let inline private ud (op: uint64) (slot: int) (gen: uint32) (extra: int) =
    op ||| (uint64 slot <<< 3) ||| (uint64 (gen &&& 0x7FFFFFu) <<< 23) ||| (uint64 extra <<< 46)

let inline private udSlot (u: uint64) = int ((u >>> 3) &&& 0xFFFFFUL)
let inline private udGen (u: uint64) = uint32 ((u >>> 23) &&& 0x7FFFFFUL)
let inline private udExtra (u: uint64) = int (u >>> 46)

// slot state bits
let private Live = 1uy
let private Recving = 2uy
let private Busy = 4uy // a request of this connection is being served
let private Closing = 8uy
let private ContinueSent = 16uy

let private NBufs =
    let n = env "NBUFS" 8192
    if n > 0 && n &&& (n - 1) = 0 && n <= 32768 then n else 8192
let private BufSize = 2048
let private BufGroup = 0us
/// After a pass that did work, io_uring_enter waits for WAIT_N completions or WAIT_US, whichever comes first (with
/// DEFER_TASKRUN the thread isn't woken before), so the requests that arrive meanwhile share one wake-up and their
/// queries one Postgres send (#30's 128 / 2 ms, #10's and #11's epoll sleep). An idle thread waits for one.
/// 32 / 1 ms: ~15% fewer cycles per request than no wait (e4); 128 / 4 ms: another 5-20% fewer server cycles and
/// up to 14% fewer Postgres cycles, more at lower per-thread rates (e14), for a p95 of ~15 ms. WAIT_N=1: no wait.
let private WaitN = uint32 (max 1 (env "WAIT_N" 128))
let private WaitNs = int64 (env "WAIT_US" 4000) * 1000L
/// Postgres dispatch: n > 0 = fill connections in order up to n in flight (see Fill); 0 = least queries in flight.
/// 8: -17% instructions, -23% cycles per request and -33% Postgres CPU vs 0 (e6); 16 with batched reads: -14%
/// Postgres cycles more (e11); 32 with group commit and 128 / 4 ms waits: -9 to -10% Postgres cycles more, same
/// p95 and p99 (e16)
let private pgDepth = env "PG_DEPTH" 32
/// PIN_THREADS=1: I/O thread i runs only on the i-th CPU of the process's affinity mask (read once at startup)
let private pinThreads = Environment.GetEnvironmentVariable "PIN_THREADS" = "1"
let private allowedCpus =
    let mask = Array.zeroCreate<uint64> 16
    if Native.sched_getaffinity (0, unativeint (mask.Length * 8), mask) <> 0 then [||]
    else [| for c in 0 .. 1023 do if mask.[c / 64] &&& (1UL <<< (c % 64)) <> 0UL then yield c |]
let private MaxRequest = 64 * 1024
let private legacyForced = Environment.GetEnvironmentVariable "LEGACY_BUFS" = "1"
let private stats = not (String.IsNullOrEmpty(Environment.GetEnvironmentVariable "MUC_STATS"))

/// a new Postgres connection for a thread (from the connector thread)
[<Sealed; AllowNullLiteral>]
type PgArrival(index: int, sock: Socket) =
    member _.Index = index
    member _.Socket = sock

/// Opens Postgres connections off the I/O threads (connect, SCRAM, prepare are blocking) and hands them over;
/// a lost connection comes back here and is retried with backoff (50 ms, doubling to 1 s).
[<Sealed>]
type Connector(url: string, options: string) =
    let q = new BlockingCollection<struct (Mailbox<PgArrival> * int * int)>()
    let run () =
        for struct (mb, index, delayMs) in q.GetConsumingEnumerable() do
            if delayMs > 0 then Thread.Sleep delayMs
            let s =
                try Pg.connect url options true
                with e ->
                    eprintfn "postgres connect failed: %s" e.Message
                    null
            if isNull s then
                let next = if delayMs = 0 then 50 else min 1000 (delayMs * 2)
                // the next attempt for this one goes behind the others
                ThreadPool.QueueUserWorkItem(fun _ -> q.Add(struct (mb, index, next))) |> ignore
            else mb.Post(PgArrival(index, s))
    let threads =
        [| for i in 0 .. 3 ->
               let t = Thread(run, 256 * 1024, IsBackground = true, Name = "pg-connect-" + string i)
               t.Start()
               t |]
    member _.Request(box: Mailbox<PgArrival>, index: int, delayMs: int) = q.Add(struct (box, index, delayMs))

[<Sealed>]
type Loop(index: int, listenFds: int[], slots: int, reads: int, writes: int, connector: Connector) as this =
    let mutable ring: Ring = null
    let gen = Array.zeroCreate<uint32> slots
    let state = Array.zeroCreate<byte> slots
    let inBuf = Array.zeroCreate<byte[]> slots
    let inLen = Array.zeroCreate<int> slots
    let mailbox = Mailbox<PgArrival>()
    let pool = Stack<Ctx>()
    let ctxs = List<Ctx>()
    let cqes = List<Cqe>(8192)
    let rearm = Queue<struct (int * uint32)>()
    let returned = List<int>()
    let accepting = Array.zeroCreate<bool> listenFds.Length
    let mutable acceptBlocked = false
    let mutable openCount = 0
    let mutable threadId = -1
    let ready = new ManualResetEventSlim(false)
    let mutable initError: exn = null
    let mutable compressor = 0n
    // Postgres: connections 0..reads-1 read, the rest write; epoch tells a replaced connection's completions apart
    let pg = Array.zeroCreate<PgConn> (reads + writes)
    let pgEpoch = Array.zeroCreate<int> (reads + writes)
    let pgFreed = fun (_: PgConn) -> ()
    let finish = fun (c: Ctx) -> this.SendNow c
    // stats
    let mutable stPasses = 0L
    let mutable stCqes = 0L
    let mutable stNoBufs = 0L
    let mutable stReqs = 0L
    let mutable stLast = Environment.TickCount64

    // receive buffers, and the provided-buffer ring when the kernel takes one
    let bufMem =
        let m = Native.mmap (0n, unativeint (NBufs * BufSize), 3, 0x2 ||| 0x20, -1, 0L)
        if m = -1n then raise (OutOfMemoryException "receive buffers")
        m
    let bufRing =
        let m = Native.mmap (0n, unativeint (NBufs * 16), 3, 0x2 ||| 0x20, -1, 0L)
        if m = -1n then raise (OutOfMemoryException "buffer ring")
        m
    let bufMask = uint16 (NBufs - 1)
    let mutable bufTail = 0us
    let mutable legacy = true

    do
        let t = Thread((fun () -> this.Run()), 1024 * 1024, IsBackground = true, Name = "io-" + string index)
        t.Start()
        ready.Wait()
        if not (isNull initError) then raise initError

    member _.Open = openCount

    member private _.Buffer(bid: int) = bufMem + nativeint (bid * BufSize)

    member private _.Recycle(bid: int) =
        if legacy then returned.Add bid
        else
            let e = bufRing + nativeint (int (bufTail &&& bufMask) * 16)
            wr64 e (uint64 (bufMem + nativeint (bid * BufSize)))
            wr32 (e + 8n) (uint32 BufSize)
            wr16 (e + 12n) (uint16 bid)
            bufTail <- bufTail + 1us

    /// legacy provided buffers: one PROVIDE_BUFFERS per run of consecutive ids given back in this pass;
    /// buffer ring: one store of the tail
    member private _.PublishBuffers() =
        if legacy then
            if returned.Count > 0 then
                returned.Sort()
                let mutable i = 0
                while i < returned.Count do
                    let first = returned.[i]
                    let mutable k = 1
                    while i + k < returned.Count && returned.[i + k] = first + k do
                        k <- k + 1
                    ring.Provide(BufGroup, first, k, bufMem + nativeint (first * BufSize), BufSize, OpIgnore)
                    i <- i + k
                returned.Clear()
        else storeRelease (NativePtr.ofNativeInt<uint16> (bufRing + 14n)) bufTail

    member private _.Rent() =
        let c =
            if pool.Count > 0 then pool.Pop()
            elif ctxs.Count >= (1 <<< 18) then raise (InvalidOperationException "too many requests in flight")
            else
                let c = Ctx(ctxs.Count, finish)
                ctxs.Add c
                c
        c.InUse <- true
        c

    member private _.Return(c: Ctx) =
        c.InUse <- false
        c.Reset()
        pool.Push c

    member private _.Recv(slot: int) =
        ring.RecvMulti(slot, BufGroup, ud OpRecv slot gen.[slot] 0)
        state.[slot] <- state.[slot] ||| Recving

    /// Ends a connection: frees its buffer and queues SHUTDOWN + CLOSE. The generation changes, so completions still
    /// in flight for it are recognised as stale; a request of it still being served is dropped when it comes back.
    member private _.CloseSlot(slot: int) =
        let st = state.[slot]
        if st &&& Live <> 0uy then
            let b = inBuf.[slot]
            if not (isNull b) then
                ArrayPool<byte>.Shared.Return b
                inBuf.[slot] <- null
            inLen.[slot] <- 0
            state.[slot] <- Closing
            gen.[slot] <- (gen.[slot] + 1u) &&& 0x7FFFFFu
            openCount <- openCount - 1
            acceptBlocked <- false
            ring.ShutdownClose(slot, OpIgnore)

    member private this.OnAccept(which: int, res: int, flags: uint32) =
        if flags &&& CQE_F_MORE = 0u then accepting.[which] <- false
        if res >= 0 then
            let slot = res
            state.[slot] <- Live
            openCount <- openCount + 1
            this.Recv slot
        elif res = -23 || res = -24 then acceptBlocked <- true // ENFILE / EMFILE: the fixed-file table is full

    // ---------- Postgres ----------

    member private _.ArmPgRecv(i: int) =
        let c = pg.[i]
        let mutable addr = 0n
        let mutable len = 0
        c.RecvTarget(&addr, &len)
        ring.RecvInto(c.Fd, addr, len, OpPgRecv ||| (uint64 i <<< 3) ||| (uint64 pgEpoch.[i] <<< 16))

    member private this.PgLost(i: int, why: string) =
        let c = pg.[i]
        if not (isNull c) then
            pg.[i] <- null
            pgEpoch.[i] <- pgEpoch.[i] + 1
            (try c.Socket.Shutdown SocketShutdown.Both with _ -> ())
            c.Fail why // fails its queries (their responses go out) and closes the socket
            connector.Request(mailbox, i, 50)

    member private this.PgArrived(a: PgArrival) =
        let i = a.Index
        pg.[i] <- PgConn(a.Socket, i, i >= reads)
        this.ArmPgRecv i

    /// the live connection of the kind with the fewest queries in flight
    member private _.Least(first: int, count: int) =
        let mutable best: PgConn = null
        for i in first .. first + count - 1 do
            let c = pg.[i]
            if not (isNull c) && not c.Broken && (isNull best || c.Inflight < best.Inflight) then best <- c
        best

    /// PG_DEPTH > 0: the first live connection of the kind with fewer than PG_DEPTH queries in flight (a burst fills
    /// a few connections, so it wakes few backends and leaves in few sends); the least loaded when all are that deep
    member private this.Fill(first: int, count: int) =
        let mutable pick: PgConn = null
        let mutable i = first
        while isNull pick && i < first + count do
            let c = pg.[i]
            if not (isNull c) && not c.Broken && c.Inflight < pgDepth then pick <- c
            i <- i + 1
        if isNull pick then this.Least(first, count) else pick

    member private this.Query(c: Ctx, write: bool) =
        let conn =
            if pgDepth > 0 then (if write then this.Fill(reads, writes) else this.Fill(0, reads))
            elif write then this.Least(reads, writes)
            else this.Least(0, reads)
        if isNull conn then c.Done(PgError("08006", "no database connection"))
        else conn.Submit c |> ignore

    member private _.FlushPg() =
        for i in 0 .. pg.Length - 1 do
            let c = pg.[i]
            if not (isNull c) && not c.Broken && not c.SendInFlight && c.HasPending then
                let mutable addr = 0n
                let mutable len = 0
                c.TakeSend(&addr, &len)
                if len > 0 then ring.Send(c.Fd, false, addr, len, MSG_NOSIGNAL, OpPgSend ||| (uint64 i <<< 3) ||| (uint64 pgEpoch.[i] <<< 16))

    member private this.OnPg(u: uint64, res: int) =
        let i = int ((u >>> 3) &&& 0x1FFFUL)
        let epoch = int (u >>> 16)
        let c = pg.[i]
        if not (isNull c) && pgEpoch.[i] = epoch then
            if u &&& 7UL = OpPgRecv then
                if res > 0 then
                    c.Received(res, pgFreed)
                    if not c.Broken then this.ArmPgRecv i
                    else this.PgLost(i, "failed while reading")
                else this.PgLost(i, (if res = 0 then "closed by server" else sprintf "recv errno %d" -res))
            else
                let mutable addr = 0n
                let mutable len = 0
                if res = -EAGAIN || res = -EINTR then
                    c.Sent(0, &addr, &len) |> ignore
                    if len > 0 then ring.Send(c.Fd, false, addr, len, MSG_NOSIGNAL, u)
                elif not (c.Sent(res, &addr, &len)) then this.PgLost(i, sprintf "send errno %d" -res)
                elif len > 0 then ring.Send(c.Fd, false, addr, len, MSG_NOSIGNAL, u)

    // ---------- requests ----------

    /// a response the thread answers at once (errors)
    member private this.Answer(slot: int, c: Ctx, status: int, message: byte[]) =
        c.Error(status, ReadOnlySpan message)
        this.SendNow c

    member private this.Reject(slot: int, status: int, message: byte[]) =
        let c = this.Rent()
        c.Slot <- slot
        c.Gen <- gen.[slot]
        c.CloseAfter <- true
        state.[slot] <- state.[slot] ||| Busy
        this.Answer(slot, c, status, message)

    /// Starts a parsed request.
    member private this.Start(c: Ctx, meth: ReadOnlySpan<byte>, path: ReadOnlySpan<byte>, hasAuth: bool, auth: ReadOnlySpan<byte>, body: ReadOnlySpan<byte>) =
        stReqs <- stReqs + 1L
        let isGet = meth.SequenceEqual(ReadOnlySpan bGet)
        if isGet && path.SequenceEqual(ReadOnlySpan pFeed) then
            c.Route <- Route.Feed
            c.BeginFeed()
            this.Query(c, false)
        elif isGet && path.StartsWith(ReadOnlySpan pPostsSlash) then
            let rest = path.Slice pPostsSlash.Length
            if rest.Contains '/'B then this.Answer(c.Slot, c, 404, eNotFound)
            else
                let id = parseId rest
                if id = 0L then this.Answer(c.Slot, c, 400, eInvalidId)
                else
                    c.Route <- Route.Post
                    c.PostId <- id
                    this.Query(c, false)
        elif isGet && path.SequenceEqual(ReadOnlySpan pHealth) then
            c.Route <- Route.Health
            if isNull (this.Least(0, reads)) then
                c.Unhealthy "no database connection"
                this.SendNow c
            else this.Query(c, false)
        elif meth.SequenceEqual(ReadOnlySpan bPost) then
            let isCreate = path.SequenceEqual(ReadOnlySpan pPosts)
            let isLike = not isCreate && path.StartsWith(ReadOnlySpan pPostsSlash) && path.EndsWith(ReadOnlySpan pLike)
                         && path.Length > pPostsSlash.Length + pLike.Length - 1
            if not isCreate && not isLike then this.Answer(c.Slot, c, 404, eNotFound)
            elif not hasAuth || not (auth.StartsWith(ReadOnlySpan bBearer)) then this.Answer(c.Slot, c, 401, eMissingBearer)
            else
                let mutable sub = 0L
                let mutable name: string = null
                match Jwt.verify (auth.Slice bBearer.Length) isCreate &sub &name with
                | Jwt.Invalid -> this.Answer(c.Slot, c, 401, eInvalidToken)
                | Jwt.BadPayload -> this.Answer(c.Slot, c, 401, eBadPayload)
                | _ ->
                    c.UserId <- sub
                    if isLike then
                        let idPart = path.Slice(pPostsSlash.Length, path.Length - pPostsSlash.Length - pLike.Length)
                        let id = if idPart.Contains '/'B then -1L else parseId idPart
                        if id = -1L then this.Answer(c.Slot, c, 404, eNotFound)
                        elif id = 0L then this.Answer(c.Slot, c, 400, eInvalidId)
                        else
                            c.Route <- Route.Like
                            c.PostId <- id
                            this.Query(c, true)
                    else
                        let mutable bytes: byte[] = null
                        match parseCreateBody body &bytes with
                        | null ->
                            c.Route <- Route.Create
                            c.Body <- bytes
                            c.Name <- Encoding.UTF8.GetBytes name
                            this.Query(c, true)
                        | e -> this.Answer(c.Slot, c, 400, e)
        else this.Answer(c.Slot, c, 404, eNotFound)

    /// Parses one request from the connection's bytes and starts it. Returns the bytes it used (0: need more).
    member private this.TryServe(slot: int, data: ReadOnlySpan<byte>) : int =
        let headEnd = data.IndexOf(ReadOnlySpan bCrlf2)
        if headEnd < 0 then
            if data.Length >= MaxRequest then
                this.Reject(slot, 431, eTooLarge)
                data.Length
            else 0
        else
            let head = data.Slice(0, headEnd)
            let lineEnd = head.IndexOf(ReadOnlySpan bCrlf)
            let line = if lineEnd < 0 then head else head.Slice(0, lineEnd)
            let sp1 = line.IndexOf ' 'B
            let sp2 = if sp1 < 0 then -1 else line.Slice(sp1 + 1).IndexOf ' 'B
            if sp1 <= 0 || sp2 <= 0 then
                this.Reject(slot, 400, eBadRequest)
                data.Length
            else
                let meth = line.Slice(0, sp1)
                let target = line.Slice(sp1 + 1, sp2)
                let version = line.Slice(sp1 + 2 + sp2)
                let http10 = version.SequenceEqual(ReadOnlySpan bHttp10)
                let mutable contentLength = 0L
                let mutable closeReq = http10
                let mutable keepAlive = false
                let mutable chunked = false
                let mutable expect = false
                let mutable gzip = false
                let mutable bad = false
                let mutable auth = ReadOnlySpan<byte>.Empty
                let mutable hasAuth = false
                let mutable rest = if lineEnd < 0 then ReadOnlySpan<byte>.Empty else head.Slice(lineEnd + 2)
                while rest.Length > 0 do
                    let e = rest.IndexOf(ReadOnlySpan bCrlf)
                    let h = if e < 0 then rest else rest.Slice(0, e)
                    rest <- if e < 0 then ReadOnlySpan<byte>.Empty else rest.Slice(e + 2)
                    let colon = h.IndexOf ':'B
                    if colon > 0 then
                        let name = h.Slice(0, colon)
                        let value = h.Slice(colon + 1).Trim(' 'B).Trim('\t'B)
                        match name.Length with
                        | 14 when isName name hContentLength ->
                            let mutable used = 0
                            if not (Utf8Parser.TryParse(value, &contentLength, &used)) || used <> value.Length || contentLength < 0L then
                                bad <- true
                        | 13 when isName name hAuthorization ->
                            auth <- value
                            hasAuth <- true
                        | 15 when isName name hAcceptEncoding -> gzip <- contains value vGzip
                        | 10 when isName name hConnection ->
                            if contains value vClose then closeReq <- true
                            elif contains value vKeepAlive then keepAlive <- true
                        | 17 when isName name hTransferEncoding -> chunked <- true
                        | 6 when isName name hExpect -> expect <- true
                        | _ -> ()
                if bad then
                    this.Reject(slot, 400, eBadRequest)
                    data.Length
                elif chunked then
                    this.Reject(slot, 400, eBadRequest)
                    data.Length
                elif contentLength > int64 MaxRequest then
                    this.Reject(slot, 413, eTooLarge)
                    data.Length
                else
                    let total = headEnd + 4 + int contentLength
                    if data.Length < total then
                        if expect && state.[slot] &&& ContinueSent = 0uy then
                            state.[slot] <- state.[slot] ||| ContinueSent
                            ring.Send(slot, true, Marshal.UnsafeAddrOfPinnedArrayElement(continue100, 0), continue100.Length,
                                      MSG_NOSIGNAL, OpIgnore)
                        0
                    else
                        state.[slot] <- (state.[slot] &&& ~~~ContinueSent) ||| Busy
                        let q = target.IndexOf '?'B
                        let path = if q < 0 then target else target.Slice(0, q)
                        let c = this.Rent()
                        c.Slot <- slot
                        c.Gen <- gen.[slot]
                        c.CloseAfter <- closeReq && not (http10 && keepAlive)
                        c.KeepAlive10 <- http10 && keepAlive
                        c.Gzip <- gzip
                        (try this.Start(c, meth, path, hasAuth, auth, data.Slice(headEnd + 4, int contentLength))
                         with e ->
                             eprintfn "%O" e
                             if c.InUse && c.Route = Route.None then this.Answer(slot, c, 500, eInternal))
                        total

    /// keeps bytes not used yet in the connection's own buffer
    member private _.Keep(slot: int, data: ReadOnlySpan<byte>) =
        let il = inLen.[slot]
        let mutable b = inBuf.[slot]
        if isNull b || b.Length - il < data.Length then
            let bigger = ArrayPool<byte>.Shared.Rent(max 1024 (il + data.Length))
            if not (isNull b) then
                Buffer.BlockCopy(b, 0, bigger, 0, il)
                ArrayPool<byte>.Shared.Return b
            b <- bigger
            inBuf.[slot] <- b
        data.CopyTo(Span<byte>(b, il, data.Length))
        inLen.[slot] <- il + data.Length

    /// serves the next request waiting in the connection's own buffer, if complete
    member private this.ServeBuffered(slot: int) =
        let il = inLen.[slot]
        if il > 0 && state.[slot] &&& (Live ||| Busy ||| Closing) = Live then
            let b = inBuf.[slot]
            let used = this.TryServe(slot, ReadOnlySpan<byte>(b, 0, il))
            if state.[slot] &&& Live <> 0uy && Object.ReferenceEquals(inBuf.[slot], b) then
                if used >= il then
                    ArrayPool<byte>.Shared.Return b
                    inBuf.[slot] <- null
                    inLen.[slot] <- 0
                elif used > 0 then
                    Buffer.BlockCopy(b, used, b, 0, il - used)
                    inLen.[slot] <- il - used

    member private this.OnData(slot: int, src: nativeint, n: int) =
        let data = ReadOnlySpan<byte>(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> src), n)
        if state.[slot] &&& Busy <> 0uy || inLen.[slot] > 0 then
            if inLen.[slot] + n > MaxRequest then this.CloseSlot slot
            else
                this.Keep(slot, data)
                this.ServeBuffered slot
        else
            let used = this.TryServe(slot, data)
            if used < n && state.[slot] &&& Live <> 0uy then this.Keep(slot, data.Slice used)

    member private this.OnRecv(u: uint64, res: int, flags: uint32) =
        let slot = udSlot u
        let valid = state.[slot] &&& Live <> 0uy && gen.[slot] = udGen u
        if valid then
            if flags &&& CQE_F_MORE = 0u then state.[slot] <- state.[slot] &&& ~~~Recving
            if res > 0 then
                (try this.OnData(slot, this.Buffer(int (flags >>> 16)), res)
                 with e ->
                     eprintfn "%O" e
                     this.CloseSlot slot)
                if state.[slot] &&& (Live ||| Recving) = Live then this.Recv slot
            elif res = -ENOBUFS then
                stNoBufs <- stNoBufs + 1L
                if state.[slot] &&& Recving = 0uy then rearm.Enqueue(struct (slot, gen.[slot]))
            else this.CloseSlot slot
        if flags &&& CQE_F_BUFFER <> 0u then this.Recycle(int (flags >>> 16))

    /// seals a finished response and queues its send (I/O thread)
    member this.SendNow(c: Ctx) =
        let slot = c.Slot
        if state.[slot] &&& Live = 0uy || gen.[slot] <> c.Gen then this.Return c
        else
            c.Seal compressor
            this.SendRest c

    member private _.SendRest(c: Ctx) =
        ring.Send(c.Slot, true, Marshal.UnsafeAddrOfPinnedArrayElement(c.SendBuf, c.SendStart + c.SentOff), c.SendLen - c.SentOff,
                  MSG_NOSIGNAL, ud OpSend c.Slot c.Gen c.Id)

    member private this.OnSend(u: uint64, res: int) =
        let c = ctxs.[udExtra u]
        let slot = udSlot u
        let valid = state.[slot] &&& Live <> 0uy && gen.[slot] = udGen u
        if not valid then this.Return c
        elif res = -EAGAIN || res = -EINTR then this.SendRest c
        elif res <= 0 then
            this.Return c
            this.CloseSlot slot
        elif c.SentOff + res < c.SendLen then
            c.SentOff <- c.SentOff + res
            this.SendRest c
        else
            let close = c.CloseAfter
            this.Return c
            state.[slot] <- state.[slot] &&& ~~~Busy
            if close then this.CloseSlot slot else this.ServeBuffered slot

    member private this.Handle(e: Cqe) =
        match e.U &&& 7UL with
        | OpAccept -> this.OnAccept(udSlot e.U, e.Res, e.Flags)
        | OpRecv -> this.OnRecv(e.U, e.Res, e.Flags)
        | OpSend -> this.OnSend(e.U, e.Res)
        | OpEvent ->
            mailbox.Rearmed()
            ring.Read(mailbox.Fd, mailbox.Sink, 8, OpEvent)
        | OpPgRecv
        | OpPgSend -> this.OnPg(e.U, e.Res)
        | _ -> ()

    member private this.Run() =
        try
            try
                threadId <- Thread.CurrentThread.ManagedThreadId
                if pinThreads && allowedCpus.Length > 0 then
                    let m = Array.zeroCreate<uint64> 16
                    let cpu = allowedCpus.[index % allowedCpus.Length]
                    m.[cpu / 64] <- 1UL <<< (cpu % 64)
                    if Native.sched_setaffinity (0, unativeint (m.Length * 8), m) <> 0 then
                        eprintfn "io-%d: sched_setaffinity cpu %d failed: errno %d" index cpu (Native.errno ())
                ring <- Ring(4096u, 32768u)
                // fixed-file table, sparse
                let reg = Marshal.AllocHGlobal 32
                NativeMemory.Clear(NativePtr.toVoidPtr (NativePtr.ofNativeInt<byte> reg), unativeint 32)
                wr32 reg (uint32 slots)
                wr32 (reg + 4n) 1u // IORING_RSRC_REGISTER_SPARSE
                try ring.Register(REGISTER_FILES2, reg, 32L) finally Marshal.FreeHGlobal reg
                // receive buffers: a buffer ring if the kernel takes one, else IORING_OP_PROVIDE_BUFFERS
                let how = if legacyForced then 0 else ring.RegisterBufRing(bufRing, NBufs, BufGroup)
                legacy <- how = 0
                if legacy then ring.Provide(BufGroup, 0, NBufs, bufMem, BufSize, OpIgnore)
                else
                    for bid in 0 .. NBufs - 1 do this.Recycle bid
                    this.PublishBuffers()
                if index = 0 then
                    eprintfn "receive buffers: %s"
                        (match how with
                         | 1 -> "buffer ring"
                         | 2 -> "buffer ring (registered on the retry with resv[0] = 1: Ubuntu 6.8 kernel)"
                         | _ -> "IORING_OP_PROVIDE_BUFFERS")
                compressor <- Gz.gz_new (gzipImpl, gzipLevel, gzipTrain)
                for i in 0 .. listenFds.Length - 1 do
                    ring.AcceptMulti(listenFds.[i], ud OpAccept i 0u 0)
                    accepting.[i] <- true
                ring.Read(mailbox.Fd, mailbox.Sink, 8, OpEvent)
                ring.Submit() |> ignore
                for i in 0 .. pg.Length - 1 do connector.Request(mailbox, i, 0)
            with e -> initError <- e
        finally
            ready.Set()
        if isNull initError then
            let mutable busy = false
            while true do
                if busy && WaitN > 1u then ring.Enter(WaitN, WaitNs) |> ignore else ring.Enter(1u, 0L) |> ignore
                cqes.Clear()
                ring.Reap(cqes, Int32.MaxValue) |> ignore
                for i in 0 .. cqes.Count - 1 do
                    try this.Handle cqes.[i] with e -> eprintfn "%O" e
                busy <- cqes.Count > 0
                // Postgres connections from the connector
                let mutable a: PgArrival = null
                while mailbox.TryTake(&a) do
                    try this.PgArrived a with e -> eprintfn "%O" e
                this.FlushPg()
                let back = if legacy then returned.Count else 32
                this.PublishBuffers()
                // receives that found no buffer: re-arm as many as buffers came back
                let mutable budget = min rearm.Count (max back 32)
                while budget > 0 do
                    let struct (slot, g) = rearm.Dequeue()
                    if gen.[slot] = g && state.[slot] &&& (Live ||| Recving) = Live then this.Recv slot
                    budget <- budget - 1
                if not acceptBlocked then
                    for i in 0 .. listenFds.Length - 1 do
                        if not accepting.[i] then
                            ring.AcceptMulti(listenFds.[i], ud OpAccept i 0u 0)
                            accepting.[i] <- true
                if stats then
                    stPasses <- stPasses + 1L
                    stCqes <- stCqes + int64 cqes.Count
                    if Environment.TickCount64 - stLast > 5000L then
                        stLast <- Environment.TickCount64
                        let inflight = pg |> Array.sumBy (fun c -> if isNull c then 0 else c.Inflight)
                        eprintfn "io-%d passes=%d cqes=%d reqs=%d nobufs=%d open=%d ctxs=%d pg=%d inflight=%d gc=%d/%d/%d"
                            index stPasses stCqes stReqs stNoBufs openCount ctxs.Count
                            (pg |> Array.sumBy (fun c -> if isNull c then 0 else 1)) inflight
                            (GC.CollectionCount 0) (GC.CollectionCount 1) (GC.CollectionCount 2)
                        stPasses <- 0L
                        stCqes <- 0L
                        stNoBufs <- 0L
                        stReqs <- 0L

// ---------- listening sockets ----------

let private AF_INET = 2
let private SOCK_STREAM = 1
let private SOCK_CLOEXEC = 0x80000
let private SOL_SOCKET = 1
let private SO_REUSEADDR = 2
let private SO_REUSEPORT = 15
let private SO_SNDBUF = 7
let private SO_RCVBUF = 8
let private IPPROTO_TCP = 6
let private TCP_NODELAY = 1

/// a listening socket of its own for each thread and port (SO_REUSEPORT: the kernel spreads new connections)
let listenSocket (ip: IPAddress) (port: int) =
    let fd = Native.socket (AF_INET, SOCK_STREAM ||| SOCK_CLOEXEC, 0)
    if fd < 0 then failwithf "socket: errno %d" (Native.errno ())
    let mutable one = 1
    Native.setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, 4) |> ignore
    Native.setsockopt (fd, SOL_SOCKET, SO_REUSEPORT, &one, 4) |> ignore
    // accepted sockets inherit TCP_NODELAY and the buffer sizes (a fixed file has no fd for setsockopt)
    Native.setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &one, 4) |> ignore
    let rcv = env "SO_RCVBUF" 0
    if rcv > 0 then
        let mutable v = rcv
        Native.setsockopt (fd, SOL_SOCKET, SO_RCVBUF, &v, 4) |> ignore
    let snd = env "SO_SNDBUF" 0
    if snd > 0 then
        let mutable v = snd
        Native.setsockopt (fd, SOL_SOCKET, SO_SNDBUF, &v, 4) |> ignore
    let addr = Array.zeroCreate<byte> 16
    addr.[0] <- byte AF_INET
    addr.[2] <- byte (port >>> 8)
    addr.[3] <- byte port
    ip.GetAddressBytes().CopyTo(addr, 4)
    let rc =
        use p = fixed addr
        Native.bind (fd, NativePtr.toNativeInt p, 16)
    if rc <> 0 then raise (IO.IOException(sprintf "Failed to bind to %O:%d: errno %d" ip port (Native.errno ())))
    if Native.listen (fd, 65535) <> 0 then failwithf "listen: errno %d" (Native.errno ())
    fd

let loops = List<Loop>()

/// Starts the I/O threads: `threads` threads, each listening on every port.
let start (ip: IPAddress) (ports: int[]) (threads: int) (reads: int) (writes: int) (url: string) (options: string) =
    defineStatements ()
    // fixed-file table per thread: room for ~4M connections over all threads (the search goes to 3M on 4 and 8
    // cores), at most 2^20 (IORING_MAX_FIXED_FILES) and the open-files limit
    let lim = Array.zeroCreate<uint64> 2
    let soft = if Native.getrlimit (7, lim) = 0 then lim.[0] else 65536UL
    let want =
        match env "SLOTS" 0 with
        | 0 ->
            let per = 4_000_000 / threads
            let mutable p = 65536
            while p < per do p <- p * 2
            p
        | s -> s
    let slots = int (min (uint64 (min want (1 <<< 20))) soft)
    let connector = Connector(url, options)
    for i in 0 .. threads - 1 do
        let fds = ports |> Array.map (listenSocket ip)
        loops.Add(Loop(i, fds, slots, reads, writes, connector))
    slots
