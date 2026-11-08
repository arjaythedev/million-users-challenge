module Program

open System
open System.Net
open System.Threading

let env name fallback =
    match Environment.GetEnvironmentVariable name with
    | null
    | "" -> fallback
    | v -> v

let envNum name (fallback: int) = int (env name (string fallback))

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
    // Postgres connections in total, reads and writes apart, split evenly over the threads: 192 + 64 (4 cores: 48 + 16
    // per thread), so with at most PG_DEPTH queries on a connection a burst spreads over several of them
    let perThread total = max 1 ((total + threads - 1) / threads)
    let reads = perThread (envNum "DB_READ_CONNS" 192)
    let writes = perThread (envNum "DB_WRITE_CONNS" 64)
    if threads * (reads + writes) > 512 then failwithf "%d Postgres connections: more than 512 (rule 5)" (threads * (reads + writes))
    let slots = Server.start ip ports threads reads writes url (env "DB_OPTIONS" "")

    eprintfn "fsharp-uring on %O ports %s: %d io_uring threads, %d fixed-file slots each, postgres %d read + %d write connections per thread, gc server=%b"
        ip (String.Join(",", ports)) threads slots reads writes Runtime.GCSettings.IsServerGC
    Thread.Sleep Timeout.Infinite
    0
