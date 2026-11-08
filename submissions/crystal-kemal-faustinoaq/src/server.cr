require "kemal"
require "pg"
require "jwt"

# The feed API on Kemal + crystal-pg, written the way a Kemal app normally is: routes, a
# DB pool, JSON::Builder, Kemal's gzip handler. start.sh runs one of these processes per
# core, all listening on both ports with SO_REUSEPORT.

SECRET  = ENV["JWT_SECRET"]
STARTED = Time.instant

# Rule 5 allows 512 Postgres connections in total, shared by every process.
WORKERS = ENV.fetch("WORKERS", System.cpu_count.to_s).to_i
POOL    = {500 // {WORKERS, 1}.max, 64}.min

DBC = DB.open(URI.parse(ENV["DATABASE_URL"]).tap { |u|
  u.query = "initial_pool_size=1&max_idle_pool_size=#{POOL}&max_pool_size=#{POOL}&retry_attempts=1&checkout_timeout=10"
}.to_s)

POST_SELECT = <<-SQL
  SELECT p.id, p.body, p.created_at, u.username,
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
    FROM posts p JOIN users u ON u.id = p.user_id
  SQL
FEED_SQL = POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
ONE_SQL  = POST_SELECT + " WHERE p.id = $1"

# created_at: UTC, truncated to milliseconds (%L truncates), Z suffix.
def ts(t : Time) : String
  t.to_utc.to_s("%Y-%m-%dT%H:%M:%S.%LZ")
end

record Post, id : Int64, body : String, created_at : Time, author : String, like_count : Int32 do
  def to_json(json : JSON::Builder)
    json.object do
      json.field "id", id
      json.field "body", body
      json.field "created_at", ts(created_at)
      json.field "author", author
      json.field "like_count", like_count
    end
  end
end

class ApiError < Exception
  getter status : Int32

  def initialize(@status, message)
    super(message)
  end
end

def read_post(rs : DB::ResultSet) : Post
  Post.new(rs.read(Int64), rs.read(String), rs.read(Time), rs.read(String), rs.read(Int32))
end

def json(env, status : Int32, body : String) : String
  env.response.status_code = status
  env.response.content_type = "application/json"
  body
end

def error(env, status : Int32, message : String) : String
  env.set "error", message # Kemal hands every 404 to the `error 404` block below
  json(env, status, {error: message}.to_json)
end

def handle(env, &)
  yield
rescue e : ApiError
  error(env, e.status, e.message.not_nil!)
rescue e
  error(env, 500, "internal server error")
end

def post_id(env) : Int64
  raw = env.params.url["id"]
  id = raw.to_i64? if raw.each_char.all?(&.ascii_number?)
  raise ApiError.new(400, "invalid post id") unless id && id > 0
  id
end

def authenticate(env) : {Int64, String}
  header = env.request.headers["Authorization"]?
  raise ApiError.new(401, "missing bearer token") unless header && header.starts_with?("Bearer ")
  payload = begin
    JWT.decode(header[7..].strip, SECRET, JWT::Algorithm::HS256)[0]
  rescue
    raise ApiError.new(401, "invalid or expired token")
  end
  raise ApiError.new(401, "invalid or expired token") unless payload["exp"]?.try(&.as_i64?)
  sub = payload["sub"]?.try(&.as_s?)
  name = payload["username"]?.try(&.as_s?)
  uid = sub.to_i64? if sub && !sub.empty? && sub.each_char.all?(&.ascii_number?)
  raise ApiError.new(401, "invalid token payload") unless uid && uid > 0 && name
  {uid, name}
end

Kemal.config.logging = false
Kemal.config.powered_by_header = false
Kemal.config.env = "production"
gzip true

get "/health" do |env|
  handle(env) do
    begin
      DBC.scalar("SELECT 1")
      json(env, 200, {status: "ok", db: "ok", uptime_s: (Time.instant - STARTED).total_seconds.to_i}.to_json)
    rescue e
      json(env, 503, {status: "degraded", db: "unreachable", error: e.message.to_s}.to_json)
    end
  end
end

get "/feed" do |env|
  handle(env) do
    posts = [] of Post
    DBC.query(FEED_SQL) { |rs| rs.each { posts << read_post(rs) } }
    json(env, 200, {posts: posts}.to_json)
  end
end

get "/posts/:id" do |env|
  handle(env) do
    id = post_id(env)
    post = DBC.query_one?(ONE_SQL, id) { |rs| read_post(rs) }
    raise ApiError.new(404, "post not found") unless post
    json(env, 200, {post: post}.to_json)
  end
end

post "/posts" do |env|
  handle(env) do
    uid, name = authenticate(env)
    data = begin
      JSON.parse(env.request.body.try(&.gets_to_end) || "")
    rescue JSON::ParseException
      raise ApiError.new(400, "malformed JSON body")
    end
    body = data.as_h?.try(&.["body"]?).try(&.as_s?).try(&.strip)
    raise ApiError.new(400, "body is required") if body.nil? || body.empty?
    raise ApiError.new(400, "body must be at most 500 characters") if body.size > 500
    id, created_at = DBC.query_one(
      "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", uid, body, as: {Int64, Time})
    json(env, 201, {post: Post.new(id, body, created_at, name, 0)}.to_json)
  end
end

post "/posts/:id/like" do |env|
  handle(env) do
    uid, _ = authenticate(env)
    id = post_id(env)
    begin
      res = DBC.exec("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", uid, id)
    rescue e : PQ::PQError
      raise ApiError.new(404, "post not found") if e.field_message(:code) == "23503"
      raise e
    end
    fresh = res.rows_affected > 0
    json(env, fresh ? 201 : 200, {liked: true, already_liked: !fresh, post_id: id}.to_json)
  end
end

error 404 do |env|
  env.response.content_type = "application/json"
  {error: env.get?("error").try(&.as(String)) || "not found"}.to_json
end

host = ENV.fetch("HOST", "0.0.0.0")
Kemal.run do |config|
  server = config.server.not_nil!
  {ENV.fetch("PORT", "3000"), ENV.fetch("EXTRA_PORT", "3001")}.each do |port|
    server.bind_tcp(host, port.to_i, reuse_port: true)
  end
end
