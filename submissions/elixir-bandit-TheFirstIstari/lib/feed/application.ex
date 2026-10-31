defmodule Feed.Application do
  use Application
  require Logger
  def start(_type, _args) do
    db_url = System.fetch_env!("DATABASE_URL")
    secret = System.fetch_env!("JWT_SECRET")
    host = System.get_env("HOST", "0.0.0.0")
    port = System.get_env("PORT", "3000") |> String.to_integer()
    extra_port = System.get_env("EXTRA_PORT", "") |> parse_extra()
    :persistent_term.put(:feed_jwt_secret, secret)
    :persistent_term.put(:feed_started, System.monotonic_time(:second))
    # Affinity/cgroup-aware core count (not logical processors).
    cores = System.schedulers_online()
    acceptors = max(2 * cores, 4)
    sockets = min(cores, 16)
    # Per-acceptor cap scaled so acceptors * num_connections >= 600k headroom
    # per port (500k/port needed for 1M over 2 ports). Floor 32768 keeps 16-core
    # behavior (32 acceptors -> 18750 -> 32768); 4-core scales to 75000.
    num_connections = max(32768, div(600_000 + acceptors - 1, acceptors))
    reuseport = sockets > 1
    pg_opts = pg_opts(db_url, cores)
    pool_size = Keyword.fetch!(pg_opts, :pool_size)
    {shards, pg_children} = pg_shards(pg_opts)
    :persistent_term.put(:feed_pg_shards, shards)
    ip = parse_ip(host)
    Logger.info("[cores] schedulers_online=#{cores} acceptors=#{acceptors} sockets=#{sockets} pool=#{pool_size} reuseport=#{reuseport} num_connections=#{num_connections}")
    bandit_base = [
      plug: Feed.Router,
      ip: ip,
      thousand_island_options: [
        num_acceptors: acceptors,
        num_connections: num_connections,
        num_listen_sockets: sockets,
        read_timeout: 120_000,
        shutdown_timeout: 5000,
        transport_options: [reuseport: reuseport, backlog: 65535, recbuf: 4096, sndbuf: 4096, buffer: 4096, keepalive: true]
      ],
      http_1_options: [
        clear_process_dict: true,
        gc_every_n_keepalive_requests: 1,
        max_request_line_length: 4096,
        max_header_length: 8192,
        max_header_count: 25
      ],
      http_options: [compress: false, log_protocol_errors: false, log_client_closures: false],
      http_2_options: [enabled: false],
      startup_log: false
    ]
    children =
      pg_children ++
        [Supervisor.child_spec({Bandit, Keyword.put(bandit_base, :port, port)}, id: :bandit_main)] ++
        (case extra_port do
           nil -> []
           ep -> [Supervisor.child_spec({Bandit, Keyword.put(bandit_base, :port, ep)}, id: :bandit_extra)]
         end)
    opts = [strategy: :one_for_one, name: Feed.Supervisor]
    {:ok, sup} = Supervisor.start_link(children, opts)
    Logger.info("[pool] shards=#{shards} total=#{pool_size} prepare=named queue_target=1000 queue_interval=5000 timeout=5000")
    Logger.info("[http] compress=false")
    warmup()
    {:ok, sup}
  end
  defp warmup do
    try do
      case Feed.DB.ping() do
        {:ok, _} ->
          case Feed.DB.feed(1) do
            {:ok, _} -> Logger.info("[warmup] statement cache warmed")
            {:error, e} -> Logger.info("[warmup] feed(1) miss: #{inspect(e)}")
          end
        {:error, e} -> Logger.info("[warmup] ping miss: #{inspect(e)}")
      end
    rescue
      e -> Logger.info("[warmup] skipped: #{inspect(e)}")
    catch
      _, e -> Logger.info("[warmup] skipped: #{inspect(e)}")
    end
  end
  defp parse_extra(""), do: nil
  defp parse_extra(nil), do: nil
  defp parse_extra(s), do: String.to_integer(s)
  defp parse_ip(host) do
    case :inet.parse_address(String.to_charlist(host)) do
      {:ok, ip} -> ip
      _ -> {0, 0, 0, 0}
    end
  end
  defp pg_shards(pg_opts) do
    total = Keyword.fetch!(pg_opts, :pool_size)
    s = min(System.schedulers_online(), 8)
    base = div(total, s)
    rem = rem(total, s)
    children =
      for i <- 1..s do
        size = if i <= rem, do: base + 1, else: base
        name = Module.concat(Feed, "PG#{i}")
        Supervisor.child_spec({Postgrex, pg_opts |> Keyword.put(:name, name) |> Keyword.put(:pool_size, size)}, id: {:pg, i})
      end
    {s, children}
  end
  defp pg_opts(url, cores) do
    uri = URI.parse(url)
    userinfo = (uri.userinfo || "") |> String.split(":", parts: 2)
    user = Enum.at(userinfo, 0, "postgres") |> URI.decode()
    pass = Enum.at(userinfo, 1, "") |> URI.decode()
    db = (uri.path || "/feed") |> String.trim_leading("/")
    host = uri.host || "localhost"
    port = uri.port || 5432
    default_pool = cores * 14
    pool =
      case System.get_env("DB_POOL") do
        nil -> default_pool
        "" -> default_pool
        s -> case Integer.parse(s) do
          {n, ""} when n > 0 -> n
          _ -> default_pool
        end
      end
    pool = min(pool, 512)
    [hostname: host, port: port, database: db, username: user, password: pass,
     pool_size: pool, prepare: :named, queue_target: 1000, queue_interval: 5000,
     timeout: 5000, connect_timeout: 5000]
  end
end
