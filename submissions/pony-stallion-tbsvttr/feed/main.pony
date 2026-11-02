"""
The feed API in Pony: stallion for HTTP (one actor per connection), the pure-Pony postgres driver for the
database, and the standard library for JSON, HS256 and base64url.
"""
use "cli"
use "net"
use "time"

class val Context
  let db: Database
  let secret: String
  let started: I64

  new val create(db': Database, secret': String, started': I64) =>
    db = db'
    secret = secret'
    started = started'

actor Main
  new create(env: Env) =>
    let vars = EnvVars(env.vars)
    let host = vars.get_or_else("HOST", "127.0.0.1")
    let ports = [vars.get_or_else("PORT", "3000"); vars.get_or_else("EXTRA_PORT", "")]
    // Postgres connections in total; one query runs on each at a time
    let connections = try vars.get_or_else("DB_POOL", "256").usize()? else 256 end
    try
      let secret = vars("JWT_SECRET")?
      let db = Database(TCPConnectAuth(env.root), vars("DATABASE_URL")?, connections)?
      let ctx = Context(db, secret, Time.seconds())
      for port in ports.values() do
        if port != "" then
          Listener(TCPListenAuth(env.root), host, port, ctx, env.out, env.err)
        end
      end
      env.out.print("feed: " + connections.string() + " Postgres connections")
    else
      env.err.print("DATABASE_URL (postgres://user:pass@host:port/db) and JWT_SECRET are required")
      env.exitcode(1)
    end
