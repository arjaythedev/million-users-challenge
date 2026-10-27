use pg = "postgres"
use "collections"
use "net"
use "time"

primitive Statements
  """
  SPEC.md's queries, prepared once per Postgres connection.
  """
  fun post_select(): String =>
    "SELECT p.id, p.body, p.created_at, u.username, " +
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " +
    "FROM posts p JOIN users u ON u.id = p.user_id"

  fun apply(): Array[(String, String)] val =>
    let select = post_select()
    recover val
      [ ("health", "SELECT 1")
        ("feed", select + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
        ("post", select + " WHERE p.id = $1")
        ("insert_post", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at")
        ("insert_like",
          "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING")
      ]
    end

class val Database
  """
  The Postgres connections, one `DbSlot` each.
  """
  let _slots: Array[DbSlot] val
  let _first: DbSlot

  new val create(auth: TCPConnectAuth, url: String, connections: USize) ? =>
    (let user, let password, let host, let port, let database) = _PgUrl(url)?
    let server = pg.ServerConnectInfo(auth, host, port where auth_requirement' = pg.AllowAnyAuth)
    let db = pg.DatabaseConnectInfo(user, password, database)
    let first = DbSlot(server, db)
    _first = first
    _slots = recover val
      let slots = Array[DbSlot](connections.max(1))
      slots.push(first)
      for _ in Range(1, connections) do slots.push(DbSlot(server, db)) end
      slots
    end

  fun slot(i: USize): DbSlot =>
    try _slots(i % _slots.size())? else _first end

primitive _Connecting
primitive _Ready
primitive _Down
type _SlotState is (_Connecting | _Ready | _Down)

actor DbSlot is (pg.SessionStatusNotify & pg.PrepareReceiver)
  """
  One Postgres connection. It prepares the statements after logging in and holds queries until they are ready.
  When the connection drops it fails what is waiting, and reconnects on the next query at most once a second.
  """
  let _server: pg.ServerConnectInfo
  let _database: pg.DatabaseConnectInfo
  var _session: (pg.Session | None) = None
  var _state: _SlotState = _Connecting
  var _prepared: USize = 0
  var _down_since: U64 = 0
  embed _waiting: Array[(pg.NamedPreparedQuery, pg.ResultReceiver)] = _waiting.create()

  new create(server: pg.ServerConnectInfo, database: pg.DatabaseConnectInfo) =>
    _server = server
    _database = database
    _connect()

  be execute(query: pg.NamedPreparedQuery, receiver: pg.ResultReceiver) =>
    match _state
    | _Ready =>
      match _session
      | let s: pg.Session => s.execute(query, receiver)
      end
    | _Connecting =>
      if _waiting.size() < 100_000 then _waiting.push((query, receiver)) else _fail(query, receiver) end
    | _Down =>
      if (Time.millis() - _down_since) >= 1000 then
        _connect()
        _waiting.push((query, receiver))
      else
        _fail(query, receiver)
      end
    end

  fun ref _connect() =>
    _state = _Connecting
    _prepared = 0
    _session = pg.Session(_server, _database, this)

  fun ref _down() =>
    _state = _Down
    _down_since = Time.millis()
    for (query, receiver) in _waiting.values() do _fail(query, receiver) end
    _waiting.clear()

  fun _fail(query: pg.NamedPreparedQuery, receiver: pg.ResultReceiver) =>
    match _session
    | let s: pg.Session => receiver.pg_query_failed(s, query, pg.SessionClosed)
    end

  fun _is_current(session: pg.Session): Bool =>
    match _session
    | let s: pg.Session => s is session
    else false
    end

  be pg_session_authenticated(session: pg.Session) =>
    if _is_current(session) then
      for (name, sql) in Statements().values() do session.prepare(name, sql, this) end
    end

  be pg_statement_prepared(session: pg.Session, name: String) =>
    if _is_current(session) then
      _prepared = _prepared + 1
      if _prepared == Statements().size() then
        _state = _Ready
        for (query, receiver) in _waiting.values() do session.execute(query, receiver) end
        _waiting.clear()
      end
    end

  be pg_prepare_failed(session: pg.Session, name: String,
    failure: (pg.ErrorResponseMessage | pg.ClientQueryError))
  =>
    if _is_current(session) then session.close() end

  be pg_session_connection_failed(session: pg.Session, reason: pg.ConnectionFailureReason) =>
    if _is_current(session) then _down() end

  be pg_session_shutdown(session: pg.Session) =>
    if _is_current(session) then _down() end

primitive _PgUrl
  """
  postgres://user:password@host:port/database -> (user, password, host, port, database)
  """
  fun apply(url: String): (String, String, String, String, String) ? =>
    let rest: String = url.substring(url.find("://")? + 3)
    let at = try rest.rfind("@")? else -1 end
    let userinfo: String = if at >= 0 then rest.substring(0, at) else "" end
    let location: String = rest.substring(at + 1)
    var user: String = userinfo
    var password: String = ""
    try
      let colon = userinfo.find(":")?
      user = userinfo.substring(0, colon)
      password = userinfo.substring(colon + 1)
    end
    let slash = location.find("/")?
    let hostport: String = location.substring(0, slash)
    var database: String = location.substring(slash + 1)
    try database = database.substring(0, database.find("?")?) end
    var host: String = hostport
    var port: String = "5432"
    try
      let colon = hostport.rfind(":")?
      host = hostport.substring(0, colon)
      port = hostport.substring(colon + 1)
    end
    (user, password, host, port, database)
