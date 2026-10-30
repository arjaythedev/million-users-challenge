// Feed API (SPEC.md) as a plain ASP.NET Core app: Kestrel, endpoint routing, Npgsql, System.Text.Json, gzip.
module Program

open System
open System.Buffers.Text
open System.Diagnostics
open System.Globalization
open System.IO
open System.IO.Compression
open System.Net
open System.Security.Cryptography
open System.Text
open System.Text.Json
open System.Threading.Tasks
open Microsoft.AspNetCore.Builder
open Microsoft.AspNetCore.Hosting
open Microsoft.AspNetCore.Http
open Microsoft.Extensions.DependencyInjection
open Microsoft.Extensions.Logging
open Npgsql

// ---- JSON bodies: the field names are the keys, in this order ----

type Post = { id: int64; body: string; created_at: string; author: string; like_count: int }
type Feed = { posts: ResizeArray<Post> }
type OnePost = { post: Post }
type Liked = { liked: bool; already_liked: bool; post_id: int64 }
type Health = { status: string; db: string; uptime_s: int64 }
type Degraded = { status: string; db: string; error: string }
type Error = { error: string }

// Only the feed (~5 KB) is worth gzipping; setting up a deflate stream costs more than it saves on the small
// bodies (worklog e4). Compressed in memory, so every response has a Content-Length.
let send (ctx: HttpContext) (status: int) (body: 'T) : Task =
    let json = JsonSerializer.SerializeToUtf8Bytes body
    let response = ctx.Response
    response.StatusCode <- status
    response.ContentType <- "application/json"
    if json.Length > 1024 && ctx.Request.Headers.AcceptEncoding.ToString().Contains "gzip" then
        let compressed = new MemoryStream()
        using (new GZipStream(compressed, CompressionLevel.Fastest, leaveOpen = true)) (fun gzip -> gzip.Write json)
        response.Headers.ContentEncoding <- "gzip"
        response.ContentLength <- compressed.Length
        response.Body.WriteAsync(compressed.GetBuffer(), 0, int compressed.Length)
    else
        response.ContentLength <- json.Length
        response.Body.WriteAsync(json, 0, json.Length)

let fail ctx status message = send ctx status { error = message }

// ---- Postgres: the SPEC's queries, one per request ----

let postSelect =
    """SELECT p.id, p.body, p.created_at, u.username,
              (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
         FROM posts p JOIN users u ON u.id = p.user_id"""

let feedSql = postSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
let postSql = postSelect + " WHERE p.id = $1"
let insertPostSql = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
let insertLikeSql = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"

// timestamptz arrives as a UTC DateTime with microseconds; "fff" truncates to milliseconds.
let iso (t: DateTime) = t.ToString("yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture)

let readPost (r: Data.Common.DbDataReader) =
    { id = r.GetInt64 0; body = r.GetString 1; created_at = iso (r.GetDateTime 2); author = r.GetString 3; like_count = r.GetInt32 4 }

// DATABASE_URL is postgres://user:pass@host:port/db; Npgsql takes a keyword connection string.
let connectionString (url: string) poolSize =
    let uri = Uri url
    let user, pass = match uri.UserInfo.Split(':', 2) with [| u; p |] -> u, p | a -> a[0], ""
    NpgsqlConnectionStringBuilder(
        Host = uri.Host, Port = (if uri.Port > 0 then uri.Port else 5432),
        Username = Uri.UnescapeDataString user, Password = Uri.UnescapeDataString pass,
        Database = uri.AbsolutePath.TrimStart '/',
        MaxPoolSize = poolSize,
        Timeout = 5,              // connect, and the wait for a free pooled connection
        MaxAutoPrepare = 10,      // prepare the five statements after their second use
        AutoPrepareMinUsages = 2,
        NoResetOnClose = true     // we never change session state
    ).ConnectionString

// ---- Auth: HS256 JWT checked on every request ----

type User = { userId: int64; username: string }

let positiveId (s: string) =
    match Int64.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture) with
    | true, id when id > 0L -> Some id
    | _ -> None

let verifyToken (key: byte[]) (token: string) : Result<User, string> =
    let invalid = Error "invalid or expired token"
    let json (part: string) = JsonDocument.Parse(Base64Url.DecodeFromChars(part.AsSpan())).RootElement
    let str (e: JsonElement) name =
        match e.TryGetProperty(name: string) with
        | true, v when v.ValueKind = JsonValueKind.String -> Some (v.GetString())
        | _ -> None
    try
        match token.Split '.' with
        | [| header; payload; signature |] ->
            let mac = HMACSHA256.HashData(key, Encoding.ASCII.GetBytes(header + "." + payload))
            let claims = json payload
            let expired =
                match claims.TryGetProperty "exp" with
                | true, exp when exp.ValueKind = JsonValueKind.Number -> exp.GetDouble() <= float (DateTimeOffset.UtcNow.ToUnixTimeSeconds())
                | _ -> true
            if not (CryptographicOperations.FixedTimeEquals(mac, Base64Url.DecodeFromChars(signature.AsSpan())))
               || str (json header) "alg" <> Some "HS256" || expired then invalid
            else
                match Option.bind positiveId (str claims "sub"), str claims "username" with
                | Some id, Some name -> Ok { userId = id; username = name }
                | _ -> Error "invalid token payload"
        | _ -> invalid
    with _ -> invalid

let authenticate key (ctx: HttpContext) =
    let header = string ctx.Request.Headers.Authorization
    if header.StartsWith("Bearer ", StringComparison.Ordinal) then verifyToken key (header.Substring 7)
    else Error "missing bearer token"

// ---- Handlers ----

let routeId (ctx: HttpContext) = positiveId (string ctx.Request.RouteValues["id"])

let started = Stopwatch.StartNew()

let health (db: NpgsqlDataSource) (ctx: HttpContext) : Task = task {
    try
        use cmd = db.CreateCommand "SELECT 1"
        let! _ = cmd.ExecuteScalarAsync()
        return! send ctx 200 { Health.status = "ok"; db = "ok"; uptime_s = int64 started.Elapsed.TotalSeconds }
    with ex ->
        return! send ctx 503 { Degraded.status = "degraded"; db = "unreachable"; error = ex.Message } }

let feed (db: NpgsqlDataSource) (ctx: HttpContext) : Task = task {
    use cmd = db.CreateCommand feedSql
    use! reader = cmd.ExecuteReaderAsync()
    let posts = ResizeArray 20
    while! reader.ReadAsync() do posts.Add(readPost reader)
    return! send ctx 200 { posts = posts } }

let getPost (db: NpgsqlDataSource) (ctx: HttpContext) : Task = task {
    match routeId ctx with
    | None -> return! fail ctx 400 "invalid post id"
    | Some id ->
        use cmd = db.CreateCommand postSql
        cmd.Parameters.AddWithValue id |> ignore
        use! reader = cmd.ExecuteReaderAsync()
        match! reader.ReadAsync() with
        | true -> return! send ctx 200 { post = readPost reader }
        | false -> return! fail ctx 404 "post not found" }

// "malformed JSON body" vs "body is required" needs the raw document, not model binding.
let readBody (request: HttpRequest) : Task<Result<string, string>> = task {
    try
        use! doc = JsonDocument.ParseAsync request.Body
        match doc.RootElement.ValueKind, doc.RootElement with
        | JsonValueKind.Object, root ->
            match root.TryGetProperty "body" with
            | true, b when b.ValueKind = JsonValueKind.String -> return Ok (b.GetString().Trim())
            | _ -> return Ok ""
        | _ -> return Ok ""
    with :? JsonException -> return Error "malformed JSON body" }

let createPost (db: NpgsqlDataSource) (user: User) (ctx: HttpContext) : Task = task {
    match! readBody ctx.Request with
    | Error message -> return! fail ctx 400 message
    | Ok "" -> return! fail ctx 400 "body is required"
    | Ok body when body.Length > 500 -> return! fail ctx 400 "body must be at most 500 characters"
    | Ok body ->
        use cmd = db.CreateCommand insertPostSql
        cmd.Parameters.AddWithValue user.userId |> ignore
        cmd.Parameters.AddWithValue body |> ignore
        use! reader = cmd.ExecuteReaderAsync()
        let! _ = reader.ReadAsync()
        let post = { id = reader.GetInt64 0; body = body; created_at = iso (reader.GetDateTime 1); author = user.username; like_count = 0 }
        return! send ctx 201 { post = post } }

let like (db: NpgsqlDataSource) (user: User) (ctx: HttpContext) : Task = task {
    match routeId ctx with
    | None -> return! fail ctx 400 "invalid post id"
    | Some id ->
        use cmd = db.CreateCommand insertLikeSql
        cmd.Parameters.AddWithValue user.userId |> ignore
        cmd.Parameters.AddWithValue id |> ignore
        try
            let! inserted = cmd.ExecuteNonQueryAsync()
            return! send ctx (if inserted > 0 then 201 else 200) { liked = true; already_liked = (inserted = 0); post_id = id }
        with :? PostgresException as e when e.SqlState = PostgresErrorCodes.ForeignKeyViolation ->
            return! fail ctx 404 "post not found" }

// ---- The app ----

[<EntryPoint>]
let main args =
    let env name fallback = match Environment.GetEnvironmentVariable name with null | "" -> fallback | v -> v
    let host = IPAddress.Parse(env "HOST" "127.0.0.1")
    let ports = [ env "PORT" "3000"; env "EXTRA_PORT" "" ] |> List.filter ((<>) "") |> List.map int
    let poolSize = int (env "DB_POOL" (string (min 128 (8 * Environment.ProcessorCount))))
    let key = Encoding.UTF8.GetBytes(env "JWT_SECRET" "")
    let db = NpgsqlDataSource.Create(connectionString (env "DATABASE_URL" "") poolSize)

    let builder = WebApplication.CreateSlimBuilder args
    builder.Logging.SetMinimumLevel LogLevel.Warning |> ignore
    builder.WebHost.UseSockets(fun o -> o.Backlog <- 65535) |> ignore  // 1M clients connecting; default 512
    builder.WebHost.ConfigureKestrel(fun o ->
        o.Limits.KeepAliveTimeout <- TimeSpan.FromSeconds 130.        // clients idle up to ~40 s
        for port in ports do o.Listen(host, port)) |> ignore

    let app = builder.Build()
    app.UseExceptionHandler(fun errors -> errors.Run(fun ctx -> fail ctx 500 "internal server error")) |> ignore

    let authed handler = RequestDelegate(fun ctx ->
        match authenticate key ctx with
        | Ok user -> handler db user ctx
        | Error message -> fail ctx 401 message)
    app.MapGet("/health", RequestDelegate(health db)) |> ignore
    app.MapGet("/feed", RequestDelegate(feed db)) |> ignore
    app.MapGet("/posts/{id}", RequestDelegate(getPost db)) |> ignore
    app.MapPost("/posts", authed createPost) |> ignore
    app.MapPost("/posts/{id}/like", authed like) |> ignore
    app.MapFallback(RequestDelegate(fun ctx -> fail ctx 404 "not found")) |> ignore

    printfn "cores=%d db_pool=%d server_gc=%b ports=%A" Environment.ProcessorCount poolSize Runtime.GCSettings.IsServerGC ports
    app.Run()
    0
