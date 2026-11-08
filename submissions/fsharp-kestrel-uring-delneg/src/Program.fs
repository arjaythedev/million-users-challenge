module Program

open System
open System.Net
open System.Threading
open System.Threading.Tasks
open Microsoft.AspNetCore.Builder
open Microsoft.AspNetCore.Connections
open Microsoft.AspNetCore.Hosting
open Microsoft.AspNetCore.Hosting.Server
open Microsoft.Extensions.DependencyInjection

let env name fallback =
    match Environment.GetEnvironmentVariable name with
    | null
    | "" -> fallback
    | v -> v

let envNum name (fallback: int) = int (env name (string fallback))

/// Kestrel needs an IHttpApplication to start; the io_uring threads answer every request themselves, so Kestrel's
/// HTTP layer never runs.
type private Unused() =
    interface IHttpApplication<obj> with
        member _.CreateContext(_) = null
        member _.ProcessRequestAsync(_) = Task.CompletedTask
        member _.DisposeContext(_, _) = ()

/// The io_uring threads own the listening sockets and every connection; Kestrel's accept loop just waits here.
type private Listener(endpoint: EndPoint) =
    let never = TaskCompletionSource<ConnectionContext>(TaskCreationOptions.RunContinuationsAsynchronously)
    interface IConnectionListener with
        member _.EndPoint = endpoint
        member _.AcceptAsync(_) = ValueTask<ConnectionContext>(never.Task)
        member _.UnbindAsync(_) =
            never.TrySetResult null |> ignore
            ValueTask.CompletedTask
        member _.DisposeAsync() =
            never.TrySetResult null |> ignore
            ValueTask.CompletedTask

/// Kestrel's transport: the endpoints are served by the io_uring threads (Server.fs).
type private UringTransportFactory() =
    interface IConnectionListenerFactory with
        member _.BindAsync(endpoint, _) = ValueTask<IConnectionListener>(Listener(endpoint))

[<EntryPoint>]
let main _ =
    let url = env "DATABASE_URL" "postgres://feed:feed@127.0.0.1:5432/feed"
    Jwt.init (env "JWT_SECRET" "million-users-challenge")
    let host = env "HOST" "0.0.0.0"
    let ip =
        match host with
        | "localhost" -> IPAddress.Loopback
        | h ->
            match IPAddress.TryParse h with
            | true, a -> a
            | _ -> Dns.GetHostAddresses(h) |> Array.find (fun a -> a.AddressFamily = Sockets.AddressFamily.InterNetwork)
    let ports =
        [| yield envNum "PORT" 3000
           match Environment.GetEnvironmentVariable "EXTRA_PORT" with
           | null
           | "" -> ()
           | p -> yield int p |]
        |> Array.distinct
    let threads = max 1 (envNum "THREADS" Environment.ProcessorCount)
    // Postgres connections in total (#1's 64 + 64 shape; reads and writes apart), split evenly over the threads
    let perThread total = max 1 ((total + threads - 1) / threads)
    let reads = perThread (envNum "DB_READ_CONNS" 64)
    let writes = perThread (envNum "DB_WRITE_CONNS" 32)
    if threads * (reads + writes) > 512 then failwithf "%d Postgres connections: more than 512 (rule 5)" (threads * (reads + writes))
    let slots = Server.start ip ports threads reads writes url (env "DB_OPTIONS" "")

    let builder = WebApplication.CreateEmptyBuilder(WebApplicationOptions())
    builder.WebHost.UseKestrelCore() |> ignore
    builder.Services.AddSingleton<IConnectionListenerFactory, UringTransportFactory>() |> ignore
    builder.WebHost.ConfigureKestrel(fun o ->
        o.AddServerHeader <- false
        for p in ports do o.Listen(ip, p))
    |> ignore
    let app = builder.Build()
    let server = app.Services.GetRequiredService<IServer>()
    server.StartAsync(Unused(), CancellationToken.None).GetAwaiter().GetResult()
    eprintfn "fsharp-kestrel-uring on %O ports %s: %d io_uring threads, %d fixed-file slots each, postgres %d read + %d write connections per thread, gc server=%b"
        ip (String.Join(",", ports)) threads slots reads writes Runtime.GCSettings.IsServerGC
    Thread.Sleep Timeout.Infinite
    0
