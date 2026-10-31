defmodule Feed.DB do
  @feed_sql "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT $1"
  @post_sql "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1"
  @insert_post_sql "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
  @like_sql "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
  def pool() do
    s =
      try do
        :persistent_term.get(:feed_pg_shards)
      rescue
        _ -> 1
      catch
        _, _ -> 1
      end
    i = Integer.mod(:erlang.system_info(:scheduler_id) - 1, s) + 1
    Module.concat(Feed, "PG#{i}")
  end
  def feed(limit \\ 20) do
    Postgrex.query(pool(), @feed_sql, [limit], timeout: 5000, pool_timeout: 5000, cache_statement: "feed")
  end
  def get_post(id) do
    Postgrex.query(pool(), @post_sql, [id], timeout: 5000, pool_timeout: 5000, cache_statement: "post")
  end
  def insert_post(uid, body) do
    Postgrex.query(pool(), @insert_post_sql, [uid, body], timeout: 5000, pool_timeout: 5000, cache_statement: "mkpost")
  end
  def like(uid, pid) do
    Postgrex.query(pool(), @like_sql, [uid, pid], timeout: 5000, pool_timeout: 5000, cache_statement: "like")
  end
  def ping() do
    Postgrex.query(pool(), "SELECT 1", [], timeout: 2000, pool_timeout: 2000, cache_statement: "health")
  end
end
