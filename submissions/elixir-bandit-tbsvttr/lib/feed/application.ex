defmodule Feed.Application do
  @moduledoc false
  # One BEAM node: one scheduler per CPU it may run on, one Erlang process per connection (Bandit), and the
  # Postgres pools from Feed.DB. Both ports are served by the same node.
  use Application
  require Logger

  @impl true
  def start(_type, _args) do
    :persistent_term.put({Feed.JWT, :key}, env!("JWT_SECRET"))
    :persistent_term.put(:feed_started_ms, System.monotonic_time(:millisecond))
    {:ok, ip} = :inet.parse_address(String.to_charlist(System.get_env("HOST", "127.0.0.1")))
    ports =
      [System.get_env("PORT", "3000"), System.get_env("EXTRA_PORT")]
      |> Enum.reject(&(&1 in [nil, ""]))
      |> Enum.map(&String.to_integer/1)
    # Postgres connections in total (DB_POOL), split over the pools.
    db_connections = String.to_integer(System.get_env("DB_POOL", "256"))

    servers = for port <- ports, do: Supervisor.child_spec({Bandit, bandit_opts(ip, port)}, id: {Bandit, port})
    Logger.info("feed: #{System.schedulers_online()} schedulers, ports #{Enum.join(ports, ", ")}, " <>
                  "#{db_connections} Postgres connections")
    Supervisor.start_link(Feed.DB.child_specs(env!("DATABASE_URL"), db_connections) ++ servers,
      strategy: :one_for_one, name: Feed.Supervisor)
  end

  defp bandit_opts(ip, port) do
    [
      plug: Feed.Router,
      ip: ip,
      port: port,
      startup_log: false,
      # gzip is done in Feed.Router, only for bodies of 1 KB or more and at level 1
      http_options: [compress: false, log_protocol_errors: false],
      # Collect the connection process after every request, so an idle connection doesn't keep its last
      # response alive until its next request
      http_1_options: [gc_every_n_keepalive_requests: 1],
      thousand_island_options: [
        # Users idle up to ~40 s between requests; the spec asks for at least 75 s (the default is 60 s)
        read_timeout: 120_000,
        num_connections: :infinity,
        # The socket backend: under mload, inet_drv kept ~128 KB of binaries alive per open connection (README).
        # buffer: the receive buffer per socket, 128 KB by default; requests here are under 1 KB.
        transport_options: [inet_backend: :socket, backlog: 65_535, buffer: 4096]
      ]
    ]
  end

  defp env!(name), do: System.get_env(name) || raise("#{name} is required")
end
