"""Postgres: the asyncpg connections of one worker process, with SPEC.md's statements prepared on each.

asyncpg runs one statement at a time per connection (no pipelining), so a worker keeps a small pool and each
query takes a free connection. The pool is a LIFO stack: under light load the same few connections stay hot.
A connection that drops is replaced in the background while the others carry the load.
"""
import asyncio
import sys
from collections import deque

import asyncpg

POST_SELECT = """SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id"""
FEED_SQL = POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
POST_SQL = POST_SELECT + " WHERE p.id = $1"
INSERT_POST_SQL = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
INSERT_LIKE_SQL = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
HEALTH_SQL = "SELECT 1"

# Statement slots on a Conn, in this order.
FEED, POST, INSERT_POST, INSERT_LIKE, HEALTH = range(5)
_STATEMENTS = (FEED_SQL, POST_SQL, INSERT_POST_SQL, INSERT_LIKE_SQL, HEALTH_SQL)

# Session settings: timestamptz columns come back as text in UTC ('2025-12-31 23:59:19.409123+00').
SESSION = {"TimeZone": "UTC", "DateStyle": "ISO", "application_name": "feed-python"}


def iso_millis(s):
    """timestamptz as Postgres writes it in UTC -> '2025-12-31T23:59:19.409Z', truncated to milliseconds.

    Postgres drops trailing zeros of the fraction ('...:19.4+00') and the whole fraction when it is zero
    ('...:19+00'); the common case is all six digits.
    """
    n = len(s)
    if n == 29:
        return s[:10] + "T" + s[11:23] + "Z"
    if n == 22:
        return s[:10] + "T" + s[11:19] + ".000Z"
    return s[:10] + "T" + (s[11:n - 3] + "00")[:12] + "Z"


class Conn:
    """One connection: its asyncpg protocol and prepared statements (and their states), by FEED, POST, ..."""
    __slots__ = ("pg", "proto", "stmts", "states", "dead")


async def _open(dsn):
    # Plain TCP unless the URL asks for TLS: asyncpg's default ("prefer") would otherwise try TLS first.
    ssl = None if "sslmode=" in dsn else False
    pg = await asyncpg.connect(dsn, ssl=ssl, server_settings=SESSION, statement_cache_size=0)
    try:
        # Text format for timestamptz only (every other column stays binary), converted in iso_millis.
        await pg.set_type_codec("timestamptz", schema="pg_catalog", encoder=str, decoder=iso_millis, format="text")
        c = Conn()
        c.pg = pg
        c.proto = pg._protocol
        # Named statements, parsed and planned once per connection. (With statement_cache_size=0 asyncpg's
        # prepare() defaults to the unnamed statement, which it re-sends with every execution, so Postgres
        # would parse and plan every query.) Keep the PreparedStatement objects alive: dropping one closes the
        # statement on the server.
        c.stmts = [await pg.prepare(sql, name=f"feed_s{i}") for i, sql in enumerate(_STATEMENTS)]
        c.states = [stmt._state for stmt in c.stmts]
        # asyncpg prepares with Parse/Describe/Flush, no Sync, and Postgres opens an implicit transaction at the
        # Parse. Left open, the first statement run on this connection would get that transaction's start time:
        # an INSERT minutes later would be stamped created_at = now() of when the connection was set up.
        await pg.execute("SELECT 1")
        c.dead = False
        return c
    except BaseException:
        pg.terminate()
        raise


class Pool:
    """Connections that are free, and queries waiting for one (FIFO, each with a deadline)."""

    def __init__(self, dsn, size, log):
        self.dsn = dsn
        self.size = size
        self.log = log
        self.free = []
        self.pending = deque()  # (deadline, which, args, callback)
        self.loop = asyncio.get_running_loop()

    async def start(self, wait_s):
        """Opens `size` connections (retrying in the background); returns once all are open or after wait_s."""
        for _ in range(self.size):
            self.loop.create_task(self._connect())
        deadline = self.loop.time() + wait_s
        while len(self.free) < self.size and self.loop.time() < deadline:
            await asyncio.sleep(0.02)
        self.loop.call_later(1.0, self._expire_pending)

    async def _connect(self):
        backoff = 0.05
        while True:
            try:
                c = await _open(self.dsn)
            except Exception as e:
                self.log(f"postgres connect failed: {e!r}")
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 1.0)
                continue
            c.pg.add_termination_listener(lambda _pg, c=c: self._lost(c))
            self.put(c)
            return

    def _lost(self, c):
        """asyncpg calls this when the connection closes for any reason: replace it."""
        if c.dead:
            return
        c.dead = True
        self.log("postgres connection lost; reconnecting")
        try:
            self.free.remove(c)
        except ValueError:
            pass  # in use: the query running on it fails and the connection is not put back
        self.loop.create_task(self._connect())

    def query(self, which, args, callback, timeout=30.0):
        """Runs prepared statement `which` with `args`, then calls callback(rows, command tag, exception)."""
        if self.free:
            _run(self, self.free.pop(), which, args, callback)
        else:
            self.pending.append((self.loop.time() + timeout, which, args, callback))

    def put(self, c):
        if c.dead:
            return
        if self.pending:
            _, which, args, callback = self.pending.popleft()
            _run(self, c, which, args, callback)
        else:
            self.free.append(c)

    def _expire_pending(self):
        """Once a second: queries that waited past their deadline for a connection fail."""
        now = self.loop.time()
        if self.pending and any(p[0] <= now for p in self.pending):
            expired = [p for p in self.pending if p[0] <= now]
            self.pending = deque(p for p in self.pending if p[0] > now)
            for p in expired:
                p[3](None, None, TimeoutError("timed out waiting for a database connection"))
        self.loop.call_later(1.0, self._expire_pending)


def _run(pool, c, which, args, callback):
    # What PreparedStatement.fetch() runs, minus its four Python wrapper coroutines, and driven without an
    # asyncio Task: the coroutine runs right here until it waits for Postgres (the query is on its way), and a
    # done-callback on the future it waits on resumes it.
    _step(pool, c, c.proto.bind_execute(c.states[which], args, "", 0, True, None), callback)


def _step(pool, c, coro, callback):
    try:
        fut = coro.send(None)
    except StopIteration as done:
        pool.put(c)
        rows, status, _ = done.value
        callback(rows, status, None)
    except BaseException as e:
        # A statement error (e.g. a foreign key violation) leaves the connection usable; a lost connection is
        # marked dead by _lost and put() drops it.
        pool.put(c)
        callback(None, None, e)
    else:
        fut.add_done_callback(lambda _: _step(pool, c, coro, callback))


def log_to_stderr(prefix):
    count = 0

    def log(msg):
        # At most 100 lines, then every 10,000th: a sick database must not flood the journal.
        nonlocal count
        count += 1
        if count <= 100 or count % 10_000 == 0:
            print(f"{prefix} #{count}: {msg}", file=sys.stderr, flush=True)

    return log
