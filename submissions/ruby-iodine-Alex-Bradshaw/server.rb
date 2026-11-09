# frozen_string_literal: true

# Million Users Challenge: a Ruby entry.
#
# Iodine's C reactor thread owns every socket (accept, HTTP parsing, keep-alive, writes) outside the GVL, so a
# million idle keep-alive connections cost C memory, not Ruby objects. Each worker process runs a small pool of
# Ruby threads that only see live requests, through Iodine's NeoRack API (no Rack env Hash per request).
# Every Ruby thread has its own Postgres connection with the spec's queries prepared once.

require "etc"
require "json"
require "openssl"
require "iodine"
require "pg"

module App
  DATABASE_URL = ENV.fetch("DATABASE_URL")
  JWT_SECRET   = ENV.fetch("JWT_SECRET").b.freeze
  HOST         = ENV.fetch("HOST", "0.0.0.0")
  PORTS        = [ENV.fetch("PORT", "3000"), ENV["EXTRA_PORT"]].compact.reject(&:empty?).uniq.freeze
  WORKERS      = Integer(ENV["WORKERS"] || Etc.nprocessors) # nprocessors follows the CPU affinity mask
  THREADS      = Integer(ENV["THREADS"] || 6)
  STARTED_AT   = Process.clock_gettime(Process::CLOCK_MONOTONIC)

  POST_SELECT = <<~SQL
    SELECT p.id, p.body, p.created_at, u.username,
           (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
      FROM posts p JOIN users u ON u.id = p.user_id
  SQL
  STATEMENTS = {
    "health" => "SELECT 1",
    "feed"   => "#{POST_SELECT} ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
    "post"   => "#{POST_SELECT} WHERE p.id = $1",
    "create" => "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
    "like"   => "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
  }.freeze

  CONTENT_TYPE = "content-type"
  JSON_TYPE    = " application/json"

  NOT_FOUND       = '{"error":"not found"}'
  INVALID_ID      = '{"error":"invalid post id"}'
  POST_NOT_FOUND  = '{"error":"post not found"}'
  MISSING_TOKEN   = '{"error":"missing bearer token"}'
  INVALID_TOKEN   = '{"error":"invalid or expired token"}'
  INVALID_PAYLOAD = '{"error":"invalid token payload"}'
  MALFORMED_JSON  = '{"error":"malformed JSON body"}'
  BODY_REQUIRED   = '{"error":"body is required"}'
  BODY_TOO_LONG   = '{"error":"body must be at most 500 characters"}'
  INTERNAL_ERROR  = '{"error":"internal server error"}'

  POSITIVE_INT  = /\A[1-9][0-9]{0,17}\z/
  NEEDS_ESCAPE  = /["\\\x00-\x1f]/
  BEARER        = "Bearer "
  HS256_HEADER  = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"

  class Halt < StandardError
    attr_reader :status, :body
    def initialize(status, body)
      @status = status
      @body = body
      super()
    end

    def backtrace = nil
  end

  module_function

  def on_http(e)
    path = e.path
    case path
    when "/feed"   then reply(e, 200, feed)
    when "/health" then health(e)
    when "/posts"
      e.method == "POST" ? create_post(e) : reply(e, 404, NOT_FOUND)
    else
      if path.start_with?("/posts/")
        rest = path.byteslice(7, path.bytesize)
        if rest.end_with?("/like")
          e.method == "POST" ? like(e, rest.byteslice(0, rest.bytesize - 5)) : reply(e, 404, NOT_FOUND)
        elsif rest.include?("/")
          reply(e, 404, NOT_FOUND)
        else
          get_post(e, rest)
        end
      else
        reply(e, 404, NOT_FOUND)
      end
    end
  rescue Halt => h
    reply(e, h.status, h.body)
  rescue PG::ConnectionBad, PG::UnableToSend, PG::AdminShutdown => err
    drop_db
    warn "db connection lost: #{err.class}: #{err.message}"
    reply(e, 500, INTERNAL_ERROR)
  rescue StandardError => err
    warn "500: #{err.class}: #{err.message}\n  #{err.backtrace&.first(5)&.join("\n  ")}"
    reply(e, 500, INTERNAL_ERROR)
  end

  def reply(e, status, body)
    e.status = status
    e.write_header(CONTENT_TYPE, JSON_TYPE)
    e.finish(body)
  end

  def health(e)
    db.exec_prepared("health") { |r| r.getvalue(0, 0) }
    uptime = (Process.clock_gettime(Process::CLOCK_MONOTONIC) - STARTED_AT).to_i
    reply(e, 200, %({"status":"ok","db":"ok","uptime_s":#{uptime}}))
  rescue PG::Error => err
    drop_db
    reply(e, 503, %({"status":"degraded","db":"unreachable","error":#{json_str(err.message.strip)}}))
  end

  def feed
    db.exec_prepared("feed") do |r|
      out = +'{"posts":['
      r.ntuples.times do |i|
        out << "," if i > 0
        post_json(out, r, i)
      end
      out << "]}"
    end
  end

  def get_post(e, raw_id)
    halt(400, INVALID_ID) unless POSITIVE_INT.match?(raw_id)
    body = db.exec_prepared("post", [raw_id]) do |r|
      next nil if r.ntuples.zero?
      post_json(+'{"post":', r, 0) << "}"
    end
    body ? reply(e, 200, body) : reply(e, 404, POST_NOT_FOUND)
  end

  def create_post(e)
    user_id, username = authenticate(e)
    raw = e.read
    data = begin
      JSON.parse(raw || "")
    rescue JSON::ParserError
      halt(400, MALFORMED_JSON)
    end
    text = data.is_a?(Hash) ? data["body"] : nil
    halt(400, BODY_REQUIRED) unless text.is_a?(String)
    text = text.strip
    halt(400, BODY_REQUIRED) if text.empty?
    halt(400, BODY_TOO_LONG) if text.length > 500

    id, created_at = db.exec_prepared("create", [user_id, text]) { |r| [r.getvalue(0, 0), r.getvalue(0, 1)] }
    reply(e, 201, %({"post":{"id":#{id},"body":#{json_str(text)},"created_at":"#{timestamp(created_at)}","author":#{json_str(username)},"like_count":0}}))
  end

  def like(e, raw_id)
    user_id, = authenticate(e) # auth first, then the id (SPEC)
    halt(400, INVALID_ID) unless POSITIVE_INT.match?(raw_id)
    inserted = begin
      db.exec_prepared("like", [user_id, raw_id]) { |r| r.cmd_tuples }
    rescue PG::ForeignKeyViolation
      halt(404, POST_NOT_FOUND)
    end
    if inserted == 1
      reply(e, 201, %({"liked":true,"already_liked":false,"post_id":#{raw_id}}))
    else
      reply(e, 200, %({"liked":true,"already_liked":true,"post_id":#{raw_id}}))
    end
  end

  def post_json(out, r, i)
    out << '{"id":' << r.getvalue(i, 0) <<
      ',"body":' << json_str(r.getvalue(i, 1)) <<
      ',"created_at":"' << timestamp(r.getvalue(i, 2)) <<
      '","author":' << json_str(r.getvalue(i, 3)) <<
      ',"like_count":' << r.getvalue(i, 4) << "}"
  end

  def json_str(s)
    NEEDS_ESCAPE.match?(s) ? JSON.generate(s) : %("#{s}")
  end

  def timestamp(s)
    frac = if s.getbyte(19) == 46 # "."
      tz = s.index("+", 20) || s.index("-", 20) || s.bytesize
      digits = s.byteslice(20, tz - 20)
      digits.bytesize >= 3 ? digits.byteslice(0, 3) : digits.ljust(3, "0")
    else
      "000"
    end
    "#{s.byteslice(0, 10)}T#{s.byteslice(11, 8)}.#{frac}Z"
  end

  def authenticate(e)
    header = e["authorization"]
    halt(401, MISSING_TOKEN) unless header&.start_with?(BEARER)
    token = header.byteslice(7, header.bytesize)
    h64, p64, s64, extra = token.split(".", 4)
    halt(401, INVALID_TOKEN) if s64.nil? || extra || s64.empty?

    signature = b64url_decode(s64)
    expected = OpenSSL::HMAC.digest("SHA256", JWT_SECRET, "#{h64}.#{p64}")
    halt(401, INVALID_TOKEN) unless signature && signature.bytesize == 32 &&
                                    OpenSSL.fixed_length_secure_compare(signature, expected)

    unless h64 == HS256_HEADER
      jose = parse_segment(h64)
      halt(401, INVALID_TOKEN) unless jose.is_a?(Hash) && jose["alg"] == "HS256"
    end

    claims = parse_segment(p64)
    halt(401, INVALID_TOKEN) unless claims.is_a?(Hash)
    exp = claims["exp"]
    halt(401, INVALID_TOKEN) unless exp.is_a?(Numeric) && exp > Time.now.to_i

    sub = claims["sub"]
    username = claims["username"]
    halt(401, INVALID_PAYLOAD) unless sub.is_a?(String) && POSITIVE_INT.match?(sub) && username.is_a?(String)
    [sub, username]
  end

  def parse_segment(b64)
    raw = b64url_decode(b64) or halt(401, INVALID_TOKEN)
    JSON.parse(raw)
  rescue JSON::ParserError, EncodingError
    halt(401, INVALID_TOKEN)
  end

  def b64url_decode(s)
    return nil if s.nil? || s.match?(/[^A-Za-z0-9_-]/)
    s = s.tr("-_", "+/")
    pad = (-s.bytesize) % 4
    return nil if pad == 3
    (s + ("=" * pad)).unpack1("m0")
  rescue ArgumentError
    nil
  end

  def halt(status, body) = raise(Halt.new(status, body))

  def db
    Thread.current[:pg] ||= connect
  end

  def connect
    conn = PG.connect(DATABASE_URL)
    conn.exec("SET TimeZone = 'UTC'")
    STATEMENTS.each { |name, sql| conn.prepare(name, sql) }
    conn
  end

  def drop_db
    if (conn = Thread.current[:pg])
      Thread.current[:pg] = nil
      conn.close rescue nil
    end
  end
end

Iodine.workers = App::WORKERS
Iodine.threads = App::THREADS
App::PORTS.each do |port|
  Iodine.listen(
    url: "http://#{App::HOST}:#{port}",
    handler: App,
    timeout: 120,
    max_body_size: 64 * 1024,
    log: false,
  )
end
warn "ruby-iodine: #{RUBY_DESCRIPTION} yjit=#{defined?(RubyVM::YJIT) && RubyVM::YJIT.enabled?}, " \
     "#{App::WORKERS} workers x #{App::THREADS} threads, ports #{App::PORTS.join(",")} " \
     "(up to #{App::WORKERS * App::THREADS} Postgres connections)"
Iodine.start
