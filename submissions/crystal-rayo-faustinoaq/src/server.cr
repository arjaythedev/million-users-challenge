require "./support"
require "./gzip"

POST_SELECT = "SELECT p.id, p.body, p.created_at, u.username, " \
              "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " \
              "FROM posts p JOIN users u ON u.id = p.user_id"
FEED   = Rayo::PG::Statement.new(POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
POST   = Rayo::PG::Statement.new(POST_SELECT + " WHERE p.id = $1")
CREATE = Rayo::PG::Statement.new("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at")
LIKE   = Rayo::PG::Statement.new("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING")
HEALTH = Rayo::PG::Statement.new("SELECT 1")

app = Feed::App.new
app.get "/health" do |ctx|
  ctx.db.query(HEALTH) do |res, ctx|
    if res.error?
      ctx.status 503
      ctx.json status: "degraded", db: "unreachable", error: "database unavailable"
    else
      ctx.json status: "ok", db: "ok", uptime_s: (Time.instant - Feed::STARTED).total_seconds.to_i64
    end
  end
end
app.get "/feed" do |ctx|
  ctx.db.query(FEED) do |res, ctx|
    if res.error?
      Feed.error(ctx, 500, "internal server error")
    else
      ctx.json { |j| j.object { j.field("posts") { j.array(res) { |row| Feed.post(row) } } } }
    end
  end
end
app.get "/posts/:id" do |ctx|
  id = Feed.id(ctx.param("id"))
  if id <= 0
    Feed.error(ctx, 400, "invalid post id")
  else
    ctx.db.query(POST, id) do |res, ctx|
      if res.error?
        Feed.error(ctx, 500, "internal server error")
      elsif row = res.first?
        ctx.json post: Feed.post(row)
      else
        Feed.error(ctx, 404, "post not found")
      end
    end
  end
end
app.post "/posts" do |ctx|
  if uid = Feed.authenticate(ctx)
    if body = Feed.body(ctx)
      ctx.db.query(CREATE, uid, body) do |res, ctx|
        if !res.error? && (row = res.first?)
          ctx.status 201
          ctx.json post: {id: row.i64(0), body: Feed.body(ctx).not_nil!, created_at: Feed::Timestamp.new(row.str(1)), author: Feed.username(ctx), like_count: 0}
        else
          Feed.error(ctx, 500, "internal server error")
        end
      end
    end
  end
end
app.post "/posts/:id/like" do |ctx|
  if uid = Feed.authenticate(ctx)
    id = Feed.id(ctx.param("id"))
    if id <= 0
      Feed.error(ctx, 400, "invalid post id")
    else
      ctx.db.query(LIKE, uid, id) do |res, ctx|
        if res.error?
          Feed.error(ctx, res.sqlstate == "23503".to_slice ? 404 : 500, res.sqlstate == "23503".to_slice ? "post not found" : "internal server error")
        else
          ctx.status(Feed.inserted ? 201 : 200)
          ctx.json liked: true, already_liked: !Feed.inserted, post_id: Feed.id(ctx.param("id"))
        end
      end
    end
  end
end
app.after do |ctx|
  if ctx.status == 500
    if ctx.path == "/health".to_slice
      ctx.output.clear
      ctx.status 503
      ctx.json status: "degraded", db: "unreachable", error: "database unavailable"
    else
      Feed.error(ctx, 500, "internal server error")
    end
  elsif ctx.status == 404 && ctx.output.len == 0
    Feed.error(ctx, 404, "not found")
  end
  Feed::Gzip.apply(ctx)
end

workers = ENV.fetch("WORKERS", Rayo::Sys.ncpu.to_s).to_i
app.run host: ENV.fetch("HOST", "0.0.0.0"), port: ENV.fetch("PORT", "3000").to_i,
  workers: workers, database: ENV["DATABASE_URL"], pool: Math.min(32, Math.max(1, 500 // workers))
