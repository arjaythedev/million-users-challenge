defmodule Feed.DB do
  @moduledoc false
  # Postgrex pools, one per scheduler (up to the connection budget). A request checks out from the pool of the
  # scheduler it runs on, so 42k checkouts a second don't all queue on one pool process. Every statement is
  # prepared once per connection (cache_statement) and reused.

  def child_specs(url, connections) do
    count = min(System.schedulers_online(), connections)
    names = for i <- 1..count, do: :"feed_db_#{i}"
    :persistent_term.put({__MODULE__, :pools}, List.to_tuple(names))
    opts = conn_opts(url)

    for {name, i} <- Enum.with_index(names) do
      size = div(connections, count) + if(i < rem(connections, count), do: 1, else: 0)
      Supervisor.child_spec({Postgrex, [name: name, pool_size: size] ++ opts}, id: name)
    end
  end

  def query(sql, params, name), do: Postgrex.query(pool(), sql, params, cache_statement: name)
  def query!(sql, params, name), do: Postgrex.query!(pool(), sql, params, cache_statement: name)

  defp pool do
    pools = :persistent_term.get({__MODULE__, :pools})
    elem(pools, rem(:erlang.system_info(:scheduler_id) - 1, tuple_size(pools)))
  end

  # postgres://user:pass@host:5432/db
  defp conn_opts(url) do
    uri = URI.parse(url)
    {user, password} =
      case String.split(uri.userinfo || "", ":", parts: 2) do
        [user, password] -> {URI.decode(user), URI.decode(password)}
        [user] -> {URI.decode(user), nil}
      end

    [
      hostname: uri.host,
      port: uri.port || 5432,
      database: String.trim_leading(uri.path || "", "/"),
      username: user,
      # A checkout waits up to ~2 s under load before DBConnection starts dropping requests (default 100 ms):
      # a slow answer beats an error.
      queue_target: 1_000,
      queue_interval: 5_000
    ] ++ if(password, do: [password: password], else: [])
  end
end
