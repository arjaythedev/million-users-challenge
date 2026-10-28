require "rayo"

module Feed
  # App#run still owns forking and supervision. Only worker listener setup differs.
  class App < Rayo::App
    private def worker(host : String, port : Int32, workers : Int32, database : String?, pool : Int32) : NoReturn
      reset_signals
      fds = StaticArray[listen(host, port), listen(host, ENV.fetch("EXTRA_PORT", "3001").to_i)]
      raise "io_uring setup failed" unless Rayo::Uring.setup(fds.to_unsafe, 2, Rayo::Sys.raise_nofile, workers)
      Rayo::Server.init(self, Rayo::Uring.capacity)
      Rayo::Server.database = Rayo::PG::WorkerPool.new(database, pool) if database
      Rayo::Uring.run
    end
  end

  # Rayo discards CommandComplete, so INSERT 0 0 and INSERT 0 1 are otherwise
  # indistinguishable. Preserve the count until the synchronous ReadyForQuery
  # callback. This is worker-local, and no receive dispatch is recursive.
  module CommandComplete
    macro included
      @feed_inserted = false

      private def message(type : UInt8, p : UInt8*, n : Int32) : Nil
        if type == 'C'.ord && n >= 11 && Bytes.new(p, 9) == "INSERT 0 ".to_slice
          @feed_inserted = p[9] == '1'.ord
        end
        Feed.inserted = @feed_inserted if type == 'Z'.ord
        previous_def(type, p, n)
        @feed_inserted = false if type == 'Z'.ord
      end
    end
  end

  class_property inserted = false

  struct Timestamp
    def initialize(@value : Bytes)
    end

    def to_json(j : Rayo::JSON::Writer) : Nil
      out = uninitialized UInt8[24]
      bytes = out.to_slice
      bytes[0, 19].copy_from(@value[0, 19])
      bytes[10] = 'T'.ord.to_u8
      bytes[19] = '.'.ord.to_u8
      3.times do |i|
        c = @value[20 + i]?
        bytes[20 + i] = c && c >= 48 && c <= 57 ? c : 48_u8
      end
      bytes[23] = 'Z'.ord.to_u8
      j.value(bytes)
    end
  end

  STARTED = Time.instant
  KEY     = Rayo::JWT::Key.new(ENV.fetch("JWT_SECRET", "million-users-challenge"))
  CLAIMS  = Bytes.new(16384)
  NAME    = Bytes.new(16384)
  BODY    = Bytes.new(16384)

  def self.error(ctx, code, message)
    ctx.output.clear
    ctx.status code
    ctx.json error: message
  end

  def self.id(value : Bytes) : Int64
    return 0_i64 if value.empty? || value.size > 18
    n = 0_i64
    value.each do |c|
      return 0_i64 unless 48 <= c <= 57
      n = n * 10 + c - 48
    end
    n
  end

  def self.authenticate(ctx) : Int64?
    auth = ctx.header("Authorization")
    unless auth.size >= 7 && auth[0, 7] == "Bearer ".to_slice
      error(ctx, 401, "missing bearer token")
      return nil
    end
    unless claims = KEY.verify(auth[7..], CLAIMS, Time.utc.to_unix)
      error(ctx, 401, "invalid or expired token")
      return nil
    end
    uid = 0_i64
    name = false
    expiry = false
    claims.object do |key, value|
      uid = id(value.string(NAME)) if key.string_equals?("sub") && value.string?
      name = value.string? if key.string_equals?("username")
      expiry = true if key.string_equals?("exp")
    end
    unless expiry
      error(ctx, 401, "invalid or expired token")
      return nil
    end
    if uid <= 0 || !name
      error(ctx, 401, "invalid token payload")
      return nil
    end
    uid
  end

  # Request input stays pinned during a query. Decode the already verified token
  # again for the create response rather than retaining a per-request allocation.
  def self.username(ctx) : Bytes
    token = ctx.header("Authorization")[7..]
    first = token.index('.'.ord.to_u8).not_nil! + 1
    last = token[first..].index('.'.ord.to_u8).not_nil! + first
    n = Rayo::JWT.decode(token[first...last], CLAIMS)
    Rayo::JSON::Reader.new(CLAIMS[0, n])["username"].string(NAME)
  end

  def self.body(ctx) : Bytes?
    unless document = Rayo::JSON::Reader.parse?(ctx.body)
      error(ctx, 400, "malformed JSON body")
      return nil
    end
    body = Bytes.empty
    document.object do |key, value|
      body = value.string(BODY) if key.string_equals?("body") && value.string?
    end if document.object?
    while !body.empty? && {9_u8, 10_u8, 11_u8, 12_u8, 13_u8, 32_u8}.includes?(body[0])
      body = body[1..]
    end
    while !body.empty? && {9_u8, 10_u8, 11_u8, 12_u8, 13_u8, 32_u8}.includes?(body[-1])
      body = body[0...-1]
    end
    if body.empty?
      error(ctx, 400, "body is required")
      return nil
    end
    if body.count { |c| c & 0xc0 != 0x80 } > 500
      error(ctx, 400, "body must be at most 500 characters")
      return nil
    end
    body
  end

  def self.post(row)
    {id: row.i64(0), body: row.str(1), created_at: Timestamp.new(row.str(2)), author: row.str(3), like_count: row.i32(4)}
  end
end

class Rayo::PG::Conn
  include Feed::CommandComplete
end
