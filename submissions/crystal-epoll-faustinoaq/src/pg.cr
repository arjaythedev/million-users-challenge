require "c/netdb"
require "c/netinet/in"
require "c/netinet/tcp"
require "uri"
require "base64"
require "openssl"
require "openssl/hmac"
require "openssl/pkcs5"
require "random/secure"

# A Postgres wire-protocol client written for this server: a few connections per worker,
# each a pipeline. Every request's query goes out as Bind/Execute/Sync on a prepared
# statement (one implicit transaction each, so one failing query never touches another),
# appended to the connection's output during a batch and sent in one write at its end.
# Replies come back in order, so each connection keeps a FIFO of who asked what.
module Pg
  K_FEED   = 1_u8
  K_ONE    = 2_u8
  K_CREATE = 3_u8
  K_LIKE   = 4_u8
  K_HEALTH = 5_u8

  GZIP_MIN = 1024 # smaller bodies go out as they are (the /feed page is ~6 KB)

  POST_SELECT = "SELECT p.id, p.body, p.created_at, u.username, " \
                "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " \
                "FROM posts p JOIN users u ON u.id = p.user_id"

  STATEMENTS = {
    {"f", POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", 0},
    {"o", POST_SELECT + " WHERE p.id = $1", 1},
    {"c", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", 2},
    {"l", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", 2},
    {"h", "SELECT 1", 0},
  }

  struct Query
    property cid : Int32 = 0
    property gen : UInt16 = 0
    property kind : UInt8 = 0
    property gzip : Bool = false
    property pid : Int64 = 0
    property blob : UInt8* = Pointer(UInt8).null # create: trimmed body, then the author
    property blen : Int32 = 0
    property nlen : Int32 = 0
  end

  class Conn
    getter idx : Int32
    getter fd : Int32
    property alive = true
    property inbuf : UInt8*
    property inlen = 0
    property incap = 65536
    property qbuf = Buf.new(65536)   # queries of the current batch
    property sbuf = Buf.new(65536)  # the write in flight
    property soff = 0
    property sending = false
    property q : Query*
    property qhead = 0_u32
    property qtail = 0_u32
    property qcap = 1024_u32
    property rows = 0
    property err = false
    property code = 0_u64 # SQLSTATE, 5 bytes packed
    property inserted = false
    property body = Buf.new(16384) # the reply being assembled
    property msg = Buf.new(256)    # error message, for /health

    def initialize(@idx, @fd)
      @inbuf = LibC.malloc(@incap).as(UInt8*)
      @q = LibC.malloc(@qcap * sizeof(Query)).as(Query*)
    end

    def pending : UInt32
      @qtail &- @qhead
    end

    def push(qq : Query) : Nil
      if pending == @qcap
        nq = LibC.malloc(@qcap * 2 * sizeof(Query)).as(Query*)
        @qcap.times { |i| nq[i] = @q[(@qhead &+ i) & (@qcap &- 1)] }
        LibC.free(@q)
        @q = nq
        @qhead = 0_u32
        @qtail = @qcap
        @qcap &*= 2
      end
      @q[@qtail & (@qcap &- 1)] = qq
      @qtail &+= 1
    end

    def front : Query*
      @q + (@qhead & (@qcap &- 1))
    end

    def pop : Nil
      @qhead &+= 1
    end
  end

  @@conns = Pointer(Conn).null
  @@n = 0
  @@rr = 0
  @@deflate = Pointer(Void).null
  @@zbuf = Buf.new(16)

  # ---- connecting (blocking, before the event loop starts) ----

  def self.connect_all(url : String, n : Int32) : Nil
    u = URI.parse(url)
    host = u.host.presence || "127.0.0.1"
    host = host[1..-2] if host.starts_with?('[')
    port = u.port || 5432
    user = URI.decode(u.user || "postgres")
    pass = URI.decode(u.password || "")
    db = u.path.lchop('/')
    db = user if db.empty?
    @@deflate = LibDeflate.alloc_compressor(6)
    @@zbuf = Buf.new(65536)
    @@conns = LibC.malloc(n * sizeof(Conn)).as(Conn*)
    n.times do |i|
      fd = open_socket(host, port)
      startup(fd, user, pass, db)
      prepare(fd)
      @@conns[i] = Conn.new(i, fd)
      @@n &+= 1
    end
  end

  private def self.open_socket(host : String, port : Int32) : Int32
    hints = LibC::Addrinfo.new
    hints.ai_family = LibC::AF_UNSPEC
    hints.ai_socktype = LibC::SOCK_STREAM
    res = Pointer(LibC::Addrinfo).null
    Sys.die("cannot resolve #{host}") if LibC.getaddrinfo(host, port.to_s, pointerof(hints), pointerof(res)) != 0
    ai = res
    while !ai.null?
      fd = LibC.socket(ai.value.ai_family, LibC::SOCK_STREAM | LibC::SOCK_CLOEXEC, 0)
      if fd >= 0
        if LibC.connect(fd, ai.value.ai_addr, ai.value.ai_addrlen) == 0
          one = 1
          LibC.setsockopt(fd, LibC::IPPROTO_TCP, LibC::TCP_NODELAY, pointerof(one).as(Void*), 4)
          return fd
        end
        LibC.close(fd)
      end
      ai = ai.value.ai_next
    end
    Sys.die("cannot connect to Postgres at #{host}:#{port}")
  end

  private def self.write_all(fd : Int32, b : Bytes) : Nil
    off = 0
    while off < b.size
      r = LibC.write(fd, b.to_unsafe + off, b.size - off)
      Sys.die("Postgres write failed") if r <= 0
      off += r
    end
  end

  private def self.read_full(fd : Int32, n : Int32) : Bytes
    b = Bytes.new(n)
    off = 0
    while off < n
      r = LibC.read(fd, b.to_unsafe + off, n - off)
      Sys.die("Postgres closed the connection") if r <= 0
      off += r
    end
    b
  end

  # One backend message: {type, payload}.
  private def self.read_msg(fd : Int32) : {Char, Bytes}
    h = read_full(fd, 5)
    len = IO::ByteFormat::BigEndian.decode(Int32, h[1, 4])
    {h[0].chr, read_full(fd, len - 4)}
  end

  private def self.msg(type : Char?, & : IO::Memory ->) : Bytes
    body = IO::Memory.new
    yield body
    io = IO::Memory.new
    io.write_byte(type.ord.to_u8) if type
    io.write_bytes(body.size.to_i32 + 4, IO::ByteFormat::BigEndian)
    io.write(body.to_slice)
    io.to_slice
  end

  private def self.cstr(io : IO, s : String) : Nil
    io << s
    io.write_byte(0_u8)
  end

  private def self.fail(payload : Bytes) : NoReturn
    m = String.new(payload).split('\0').select { |f| f.starts_with?('M') }.first?
    Sys.die("Postgres: #{m.try(&.[1..]) || "error"}")
  end

  private def self.startup(fd : Int32, user : String, pass : String, db : String) : Nil
    write_all(fd, msg(nil) { |io|
      io.write_bytes(196608_i32, IO::ByteFormat::BigEndian)
      # session settings: timestamps come back as "YYYY-MM-DD HH:MM:SS.ffffff+00"
      {"user" => user, "database" => db, "client_encoding" => "UTF8", "TimeZone" => "UTC",
       "DateStyle" => "ISO, MDY", "application_name" => "crystal-epoll"}.each { |k, v| cstr(io, k); cstr(io, v) }
      io.write_byte(0_u8)
    })
    nonce = client_first = ""
    salted = Bytes.empty
    auth_msg = ""
    loop do
      t, p = read_msg(fd)
      case t
      when 'R'
        code = IO::ByteFormat::BigEndian.decode(Int32, p[0, 4])
        case code
        when 0 # ok
        when 3 # cleartext
          write_all(fd, msg('p') { |io| cstr(io, pass) })
        when 5 # md5
          salt = p[4, 4]
          inner = OpenSSL::Digest.new("MD5").update(pass + user).final.hexstring
          outer = OpenSSL::Digest.new("MD5").update(inner.to_slice).update(salt).final.hexstring
          write_all(fd, msg('p') { |io| cstr(io, "md5" + outer) })
        when 10 # SASL: SCRAM-SHA-256
          Sys.die("Postgres wants SASL without SCRAM-SHA-256") unless String.new(p[4..]).includes?("SCRAM-SHA-256")
          nonce = Base64.strict_encode(Random::Secure.random_bytes(18))
          client_first = "n=,r=#{nonce}"
          first = "n,,#{client_first}"
          write_all(fd, msg('p') { |io|
            cstr(io, "SCRAM-SHA-256")
            io.write_bytes(first.bytesize.to_i32, IO::ByteFormat::BigEndian)
            io << first
          })
        when 11 # SASL continue: server-first
          server_first = String.new(p[4..])
          f = server_first.split(',').to_h { |kv| {kv[0], kv[2..]} }
          r = f['r']
          Sys.die("SCRAM nonce mismatch") unless r.starts_with?(nonce)
          salted = OpenSSL::PKCS5.pbkdf2_hmac(pass, Base64.decode(f['s']), f['i'].to_i, OpenSSL::Algorithm::SHA256, 32)
          client_key = OpenSSL::HMAC.digest(:sha256, salted, "Client Key")
          stored_key = OpenSSL::Digest.new("SHA256").update(client_key).final
          without_proof = "c=biws,r=#{r}"
          auth_msg = "#{client_first},#{server_first},#{without_proof}"
          sig = OpenSSL::HMAC.digest(:sha256, stored_key, auth_msg)
          proof = Bytes.new(32) { |i| client_key[i] ^ sig[i] }
          final = "#{without_proof},p=#{Base64.strict_encode(proof)}"
          write_all(fd, msg('p') { |io| io << final })
        when 12 # SASL final: check the server's signature
          server_key = OpenSSL::HMAC.digest(:sha256, salted, "Server Key")
          expect = Base64.strict_encode(OpenSSL::HMAC.digest(:sha256, server_key, auth_msg))
          Sys.die("SCRAM server signature mismatch") unless String.new(p[4..]).includes?("v=#{expect}")
        else
          Sys.die("unsupported Postgres auth method #{code}")
        end
      when 'E' then fail(p)
      when 'Z' then break
      end
    end
  end

  private def self.prepare(fd : Int32) : Nil
    io = IO::Memory.new
    STATEMENTS.each do |name, sql, nparams|
      io.write(msg('P') { |m|
        cstr(m, name)
        cstr(m, sql)
        m.write_bytes(nparams.to_i16, IO::ByteFormat::BigEndian)
        nparams.times { m.write_bytes(0_i32, IO::ByteFormat::BigEndian) } # let Postgres infer
      })
    end
    io.write(msg('S') { })
    write_all(fd, io.to_slice)
    loop do
      t, p = read_msg(fd)
      fail(p) if t == 'E'
      break if t == 'Z'
    end
  end

  def self.count : Int32
    @@n
  end

  def self.conn(i : Int32) : Conn
    @@conns[i]
  end

  # ---- queries ----

  # The next live connection, round-robin. nil when none is left.
  private def self.pick : Conn?
    @@n.times do
      c = @@conns[@@rr]
      @@rr &+= 1
      @@rr = 0 if @@rr == @@n
      return c if c.alive
    end
    nil
  end

  @[AlwaysInline]
  private def self.bind(o : Buf, stmt : UInt8) : Int32
    o << 'B'.ord.to_u8
    at = o.len
    o.reserve(4)
    o.len &+= 4
    o << 0_u8 << stmt << 0_u8 # unnamed portal, statement
    o << 0_u8 << 0_u8         # parameter formats: all text
    at
  end

  @[AlwaysInline]
  private def self.be32(p : UInt8*, v : Int32) : Nil
    p[0] = (v >> 24).to_u8!
    p[1] = (v >> 16).to_u8!
    p[2] = (v >> 8).to_u8!
    p[3] = v.to_u8!
  end

  @[AlwaysInline]
  private def self.be16(o : Buf, v : Int32) : Nil
    o << (v >> 8).to_u8! << v.to_u8!
  end

  # Text parameter: length, then bytes.
  @[AlwaysInline]
  private def self.param(o : Buf, p : UInt8*, n : Int32) : Nil
    o.reserve(4)
    be32(o.ptr + o.len, n)
    o.len &+= 4
    o.write(p, n)
  end

  private def self.param(o : Buf, v : Int64) : Nil
    at = o.len
    o.reserve(4)
    o.len &+= 4
    o << v
    be32(o.ptr + at, o.len &- at &- 4)
  end

  # Result formats (all text), Execute (all rows), Sync; then fixes the Bind length.
  @[AlwaysInline]
  private def self.finish(o : Buf, at : Int32) : Nil
    o << 0_u8 << 0_u8
    be32(o.ptr + at, o.len &- at)
    o << "E\0\0\0\x09\0\0\0\0\0S\0\0\0\x04"
  end

  # Queues a query for the client connection now being served (Server.cur_fd). It answers
  # when the reply comes in; the client sends nothing else until then.
  def self.ask(kind : UInt8, pid : Int64 = 0_i64, uid : Int64 = 0_i64, body : UInt8* = Pointer(UInt8).null,
               blen : Int32 = 0, name : UInt8* = Pointer(UInt8).null, nlen : Int32 = 0) : Bool
    c = pick
    return false unless c
    o = c.qbuf
    case kind
    when K_FEED
      finish(o, bind(o, 'f'.ord.to_u8).tap { be16(o, 0) })
    when K_ONE
      at = bind(o, 'o'.ord.to_u8)
      be16(o, 1)
      param(o, pid)
      finish(o, at)
    when K_LIKE
      at = bind(o, 'l'.ord.to_u8)
      be16(o, 2)
      param(o, uid)
      param(o, pid)
      finish(o, at)
    when K_CREATE
      at = bind(o, 'c'.ord.to_u8)
      be16(o, 2)
      param(o, uid)
      param(o, body, blen)
      finish(o, at)
    else
      finish(o, bind(o, 'h'.ord.to_u8).tap { be16(o, 0) })
    end
    qq = Query.new
    fd = Server.cur_fd
    qq.cid = fd
    qq.gen = Server.conn(fd).value.gen
    qq.kind = kind
    qq.gzip = Server.cur_gzip
    qq.pid = pid
    if kind == K_CREATE
      b = LibC.malloc(blen &+ nlen &+ 1).as(UInt8*)
      b.copy_from(body, blen)
      (b + blen).copy_from(name, nlen)
      qq.blob = b
      qq.blen = blen
      qq.nlen = nlen
    end
    c.push(qq)
    Server.conn(fd).value.busy = true
    true
  end

  # End of a batch: each connection's queued queries go out in one send.
  def self.flush : Nil
    @@n.times do |i|
      c = @@conns[i]
      next if c.sending || c.qbuf.len == 0 || !c.alive
      t = c.sbuf
      c.sbuf = c.qbuf
      c.qbuf = t
      c.qbuf.clear
      c.soff = 0
      c.sending = true
      Uring.pg_send(c.idx, c.fd, c.sbuf.ptr, c.sbuf.len)
    end
  end

  def self.on_sent(i : Int32, res : Int32) : Nil
    c = @@conns[i]
    if res <= 0
      lost(c)
      return
    end
    c.soff &+= res
    if c.soff < c.sbuf.len
      Uring.pg_send(c.idx, c.fd, c.sbuf.ptr + c.soff, c.sbuf.len &- c.soff)
    else
      c.sending = false
      c.sbuf.clear
    end
  end

  # The connection to Postgres broke: everyone waiting on it gets a 500.
  def self.lost(c : Conn) : Nil
    return unless c.alive
    c.alive = false
    STDERR.puts "postgres connection #{c.idx} lost"
    while c.pending > 0
      qq = c.front.value
      c.pop
      LibC.free(qq.blob) unless qq.blob.null?
      reply_fixed(qq, Resp::INTERNAL)
    end
  end

  # ---- replies ----

  def self.on_data(i : Int32, p : UInt8*, n : Int32) : Nil
    c = @@conns[i]
    if c.incap &- c.inlen < n
      c.incap = (c.inlen &+ n) &* 2
      c.inbuf = LibC.realloc(c.inbuf, c.incap).as(UInt8*)
    end
    (c.inbuf + c.inlen).copy_from(p, n)
    c.inlen &+= n
    b = c.inbuf
    off = 0
    while c.inlen &- off >= 5
      len = (b[off &+ 1].to_i32 << 24) | (b[off &+ 2].to_i32 << 16) | (b[off &+ 3].to_i32 << 8) | b[off &+ 4].to_i32
      break if c.inlen &- off < len &+ 1
      message(c, b[off], b + (off &+ 5), len &- 4)
      off &+= len &+ 1
    end
    if off > 0
      b.move_from(b + off, c.inlen &- off)
      c.inlen &-= off
    end
  end

  @[AlwaysInline]
  private def self.u32(p : UInt8*) : Int32
    (p[0].to_i32 << 24) | (p[1].to_i32 << 16) | (p[2].to_i32 << 8) | p[3].to_i32
  end

  private def self.message(c : Conn, t : UInt8, p : UInt8*, n : Int32) : Nil
    return if c.pending == 0
    qq = c.front
    case t
    when 'D'.ord then row(c, qq, p)
    when 'C'.ord # "INSERT 0 1": a new like
      c.inserted = n >= 2 && p[n &- 2] == '1'.ord && p[n &- 3] == ' '.ord
    when 'E'.ord
      c.err = true
      c.msg.clear
      j = 0
      while j < n && p[j] != 0
        f = p[j]
        s = j &+ 1
        e = s
        while e < n && p[e] != 0
          e &+= 1
        end
        if f == 'C'.ord && e &- s == 5
          v = 0_u64
          5.times { |k| v = (v << 8) | p[s &+ k] }
          c.code = v
        elsif f == 'M'.ord
          c.msg.write(p + s, e &- s)
        end
        j = e &+ 1
      end
    when 'Z'.ord
      done(c, qq.value)
      c.pop
      c.rows = 0
      c.err = false
      c.inserted = false
      c.body.clear
    end
  end

  # One DataRow: appended to the reply being built.
  private def self.row(c : Conn, qq : Query*, p : UInt8*) : Nil
    o = c.body
    k = qq.value.kind
    p += 2 # column count
    if k == K_CREATE
      # id, created_at: kept as the start of the reply; body and author come from the request
      l = u32(p)
      o << %({"post":{"id":)
      o.write(p + 4, l)
      p += 4 &+ l
      l = u32(p)
      b = qq.value.blob
      o << %(,"body":")
      o.json_escape(b, qq.value.blen)
      o << %(","created_at":")
      timestamp(o, p + 4, l)
      o << %(","author":")
      o.write(b + qq.value.blen, qq.value.nlen)
      o << %(","like_count":0}})
      c.rows &+= 1
      return
    end
    return unless k == K_FEED || k == K_ONE
    o << (c.rows == 0 ? (k == K_FEED ? %({"posts":[) : %({"post":)) : ",")
    c.rows &+= 1
    l = u32(p)
    o << %({"id":)
    o.write(p + 4, l)
    p += 4 &+ l
    l = u32(p)
    o << %(,"body":")
    o.json_escape(p + 4, l)
    p += 4 &+ l
    l = u32(p)
    o << %(","created_at":")
    timestamp(o, p + 4, l)
    p += 4 &+ l
    l = u32(p)
    o << %(","author":")
    o.json_escape(p + 4, l)
    p += 4 &+ l
    l = u32(p)
    o << %(","like_count":)
    o.write(p + 4, l)
    o << '}'.ord.to_u8
  end

  # "2025-12-31 23:59:19.409123+00" -> "2025-12-31T23:59:19.409Z" (UTC, truncated to ms)
  private def self.timestamp(o : Buf, p : UInt8*, n : Int32) : Nil
    if n < 19
      o.write(p, n)
      return
    end
    o.write(p, 10)
    o << 'T'.ord.to_u8
    o.write(p + 11, 8)
    o << '.'.ord.to_u8
    i = 19
    d = 0
    if i < n && p[i] == '.'.ord
      i &+= 1
      while d < 3 && i < n && p[i] >= 0x30 && p[i] <= 0x39
        o << p[i]
        i &+= 1
        d &+= 1
      end
    end
    while d < 3
      o << 0x30_u8
      d &+= 1
    end
    o << 'Z'.ord.to_u8
  end

  SQLSTATE_FK = 0x3233353033_u64 # "23503"

  # ReadyForQuery: the front query's reply is complete.
  private def self.done(c : Conn, qq : Query) : Nil
    LibC.free(qq.blob) unless qq.blob.null?
    if c.err
      if qq.kind == K_LIKE && c.code == SQLSTATE_FK
        reply_fixed(qq, Resp::POST_NOT_FOUND)
      elsif qq.kind == K_HEALTH
        b = c.body
        b.clear
        b << %({"status":"degraded","db":"unreachable","error":")
        b.json_escape(c.msg.ptr, c.msg.len)
        b << %("})
        reply(qq, "503 Service Unavailable", b)
      else
        reply_fixed(qq, Resp::INTERNAL)
      end
      return
    end
    b = c.body
    case qq.kind
    when K_FEED
      b << %({"posts":[) if c.rows == 0
      b << "]}"
      reply(qq, "200 OK", b)
    when K_ONE
      if c.rows == 0
        reply_fixed(qq, Resp::POST_NOT_FOUND)
      else
        b << '}'.ord.to_u8
        reply(qq, "200 OK", b)
      end
    when K_CREATE
      if c.rows == 0
        reply_fixed(qq, Resp::INTERNAL)
      else
        reply(qq, "201 Created", b)
      end
    when K_LIKE
      b << (c.inserted ? %({"liked":true,"already_liked":false,"post_id":) : %({"liked":true,"already_liked":true,"post_id":))
      b << qq.pid << '}'.ord.to_u8
      reply(qq, c.inserted ? "201 Created" : "200 OK", b)
    else
      b << %({"status":"ok","db":"ok","uptime_s":) << Server.uptime << '}'.ord.to_u8
      reply(qq, "200 OK", b)
    end
  end

  private def self.reply_fixed(qq : Query, r : String) : Nil
    o = Server.out
    start = o.len
    o << r
    Server.db_done(qq.cid, qq.gen, start)
  end

  # Writes the HTTP response into the batch arena, gzipped when the client takes it and
  # the body is big enough to be worth it.
  private def self.reply(qq : Query, status : String, b : Buf) : Nil
    o = Server.out
    start = o.len
    if qq.gzip && b.len >= GZIP_MIN
      bound = LibDeflate.gzip_bound(@@deflate, b.len).to_i32
      o << "HTTP/1.1 " << status << Resp::GZIP_HDR
      body = o.len
      o.reserve(bound)
      z = LibDeflate.gzip_compress(@@deflate, b.ptr, b.len, o.ptr + body, bound).to_i32
      if z > 0
        o.len &+= z
        Resp.close(o, body)
        Server.db_done(qq.cid, qq.gen, start)
        return
      end
      o.len = start
    end
    body = Resp.open(o, status)
    o.write(b.ptr, b.len)
    Resp.close(o, body)
    Server.db_done(qq.cid, qq.gen, start)
  end
end

@[Link("deflate")]
lib LibDeflate
  fun alloc_compressor = libdeflate_alloc_compressor(level : LibC::Int) : Void*
  fun gzip_compress = libdeflate_gzip_compress(c : Void*, input : Void*, n : LibC::SizeT, output : Void*, avail : LibC::SizeT) : LibC::SizeT
  fun gzip_bound = libdeflate_gzip_compress_bound(c : Void*, n : LibC::SizeT) : LibC::SizeT
end
