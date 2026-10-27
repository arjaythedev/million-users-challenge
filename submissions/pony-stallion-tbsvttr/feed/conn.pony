use stallion = "stallion"
use pg = "postgres"
use "json"
use "net"
use "time"

primitive _Health
primitive _Feed
primitive _GetPost

class val _InsertPost
  let username: String
  let body: String

  new val create(username': String, body': String) =>
    username = username'
    body = body'

class val _Like
  let post_id: I64

  new val create(post_id': I64) =>
    post_id = post_id'

type _Kind is (_Health | _Feed | _GetPost | _InsertPost | _Like)

class _Pending
  """
  A request waiting for its query: the responder to answer it with, and what to build.
  """
  let query: pg.NamedPreparedQuery
  let responder: stallion.Responder
  let kind: _Kind
  let gzip: Bool

  new create(query': pg.NamedPreparedQuery, responder': stallion.Responder, kind': _Kind, gzip': Bool) =>
    query = query'
    responder = responder'
    kind = kind'
    gzip = gzip'

class val _User
  let id: I64
  let name: String

  new val create(id': I64, name': String) =>
    id = id'
    name = name'

actor Conn is (stallion.HTTPServerActor & pg.ResultReceiver)
  """
  One HTTP connection. A request that needs Postgres sends its query to this connection's `DbSlot` and keeps
  its responder until the result comes back to this actor.
  """
  var _http: stallion.HTTPServer = stallion.HTTPServer.none()
  let _secret: String
  let _started: I64
  let _db: DbSlot
  var _body: String iso = recover String end
  embed _pending: Array[_Pending] = _pending.create()

  new create(auth: TCPServerAuth, fd: U32, config: stallion.ServerConfig, secret: String, started: I64,
    db: DbSlot)
  =>
    _secret = secret
    _started = started
    _db = db
    _http = stallion.HTTPServer(auth, fd, this, config)

  fun ref _http_connection(): stallion.HTTPServer => _http

  fun ref on_request(request': stallion.Request val, responder: stallion.Responder) =>
    _body = recover String end

  fun ref on_body_chunk(data: Array[U8] val) =>
    _body.append(data)

  fun ref on_request_complete(request': stallion.Request val, responder: stallion.Responder) =>
    let body: String val = _body = recover String end
    let path = request'.uri.path
    match request'.method
    | stallion.GET =>
      if path == "/feed" then
        _query(responder, _Feed, "feed", recover val Array[pg.FieldDataTypes] end, _accepts_gzip(request'))
      elseif path.at("/posts/", 0) then
        match _Ids(path.substring(7))
        | let id: I64 => _query(responder, _GetPost, "post", recover val [as pg.FieldDataTypes: id] end)
        else _error(responder, 400, "invalid post id")
        end
      elseif path == "/health" then
        _query(responder, _Health, "health", recover val Array[pg.FieldDataTypes] end)
      else
        _error(responder, 404, "not found")
      end
    | stallion.POST =>
      if path == "/posts" then
        match _authenticate(request')
        | let user: _User => _create_post(responder, user, body)
        | let message: String => _error(responder, 401, message)
        end
      elseif path.at("/posts/", 0) and path.at("/like", -5) then
        // auth first, then the id
        match _authenticate(request')
        | let user: _User =>
          match _Ids(path.substring(7, -5))
          | let id: I64 =>
            _query(responder, _Like(id), "insert_like", recover val [as pg.FieldDataTypes: user.id; id] end)
          else _error(responder, 400, "invalid post id")
          end
        | let message: String => _error(responder, 401, message)
        end
      else
        _error(responder, 404, "not found")
      end
    else
      _error(responder, 404, "not found")
    end

  fun ref _create_post(responder: stallion.Responder, user: _User, raw: String) =>
    match JSONParser.parse(raw)
    | let obj: JSONObject =>
      try
        let text = (obj("body")? as String).clone()
        text.strip()
        let trimmed: String val = consume text
        if trimmed.size() == 0 then
          _error(responder, 400, "body is required")
        // "characters" are code points, like Postgres's char_length() in the CHECK constraint
        elseif trimmed.codepoints() > 500 then
          _error(responder, 400, "body must be at most 500 characters")
        else
          _query(responder, _InsertPost(user.name, trimmed), "insert_post",
            recover val [as pg.FieldDataTypes: user.id; trimmed] end)
        end
      else
        _error(responder, 400, "body is required")
      end
    | let _: JSONParseError => _error(responder, 400, "malformed JSON body")
    else
      _error(responder, 400, "body is required")
    end

  fun _authenticate(request': stallion.Request val): (_User | String) =>
    match request'.headers.get("authorization")
    | let header: String if header.at("Bearer ", 0) =>
      match Jwt.verify(header.substring(7), _secret)
      | let claims: JSONObject =>
        try
          let sub = claims("sub")? as String
          let username = claims("username")? as String
          match _Ids(sub)
          | let id: I64 => _User(id, username)
          else "invalid token payload"
          end
        else
          "invalid token payload"
        end
      else
        "invalid or expired token"
      end
    else
      "missing bearer token"
    end

  fun _accepts_gzip(request': stallion.Request val): Bool =>
    match request'.headers.get("accept-encoding")
    | let v: String => v.contains("gzip")
    else false
    end

  fun ref _query(responder: stallion.Responder, kind: _Kind, name: String,
    params: Array[pg.FieldDataTypes] val, gzip: Bool = false)
  =>
    let query = pg.NamedPreparedQuery(name, params)
    _pending.push(_Pending(query, responder, kind, gzip))
    _db.execute(query, this)

  fun ref _take(query: pg.Query): (_Pending | None) =>
    var i: USize = 0
    while i < _pending.size() do
      try
        if _pending(i)?.query is query then return _pending.delete(i)? end
      end
      i = i + 1
    end
    None

  be pg_query_result(session: pg.Session, result: pg.Result) =>
    match _take(result.query())
    | let p: _Pending => _respond(p, result)
    end

  be pg_query_failed(session: pg.Session, query: pg.Query,
    failure: (pg.ErrorResponseMessage | pg.ClientQueryError))
  =>
    match _take(query)
    | let p: _Pending => _failed(p, failure)
    end

  fun ref _respond(p: _Pending, result: pg.Result) =>
    try
      match p.kind
      | _Feed =>
        let rows = (result as pg.ResultSet).rows()
        let body = recover val
          let out = String(6144)
          out.append("{\"posts\":[")
          var first = true
          for row in rows.values() do
            if not first then out.push(',') end
            first = false
            _Json.post(out, row)?
          end
          out.append("]}")
          out
        end
        // gzip (level 1) for bodies of 1 KB or more when the client asks for it: in practice the feed
        match if p.gzip and (body.size() >= 1024) then _Gzip(body) end
        | let gz: Array[U8] val => p.responder.respond(_Http.gzip_response(200, gz))
        else _json(p.responder, 200, body)
        end
      | _GetPost =>
        let rows = (result as pg.ResultSet).rows()
        if rows.size() == 0 then
          _error(p.responder, 404, "post not found")
        else
          let row = rows(0)?
          let body = recover val
            let out = String(512)
            out.append("{\"post\":")
            _Json.post(out, row)?
            out.push('}')
            out
          end
          _json(p.responder, 200, body)
        end
      | let k: _InsertPost =>
        let fields = (result as pg.ResultSet).rows()(0)?.fields
        let id = fields(0)?.value as I64
        let created = fields(1)?.value as pg.PgTimestamp
        let body = recover val
          let out = String(768)
          out.append("{\"post\":")
          _Json.post_fields(out, id, k.body, created, k.username, 0)
          out.push('}')
          out
        end
        _json(p.responder, 201, body)
      | let k: _Like =>
        let inserted = (result as pg.RowModifying).impacted() == 1
        _json(p.responder, if inserted then 201 else 200 end,
          "{\"liked\":true,\"already_liked\":" + (not inserted).string() + ",\"post_id\":" +
          k.post_id.string() + "}")
      | _Health =>
        _json(p.responder, 200,
          "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":" + (Time.seconds() - _started).string() + "}")
      end
    else
      _error(p.responder, 500, "internal server error")
    end

  fun ref _failed(p: _Pending, failure: (pg.ErrorResponseMessage | pg.ClientQueryError)) =>
    match p.kind
    | _Health =>
      let message: String = match failure
        | let e: pg.ErrorResponseMessage => e.message
        else "no connection to the database"
        end
      let body = recover val
        let out = String(128)
        out.append("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":")
        _Json.string(out, message)
        out.push('}')
        out
      end
      _json(p.responder, 503, body)
    | let _: _Like =>
      match failure
      | let e: pg.ErrorResponseMessage if e.code == "23503" => _error(p.responder, 404, "post not found")
      else _error(p.responder, 500, "internal server error")
      end
    else
      _error(p.responder, 500, "internal server error")
    end

  fun ref _json(responder: stallion.Responder, status: U16, body: String) =>
    responder.respond(_Http.response(status, body))

  fun ref _error(responder: stallion.Responder, status: U16, message: String) =>
    let body = recover val
      let out = String(64)
      out.append("{\"error\":")
      _Json.string(out, message)
      out.push('}')
      out
    end
    _json(responder, status, body)
