"""Postgres with asyncpg: a small connection pool per Gunicorn worker process, opened in the lifespan handler.

Pool size per worker: DB_POOL if set, else max(2, 128 // workers): about 128 connections in total,
well under the challenge's 512. At a million users the app has ~13 queries in flight at a time.
"""
import asyncio
import collections
import os

import asyncpg

DATABASE_URL = os.environ["DATABASE_URL"]
ACQUIRE_TIMEOUT_S = 5.0
FEED_SIZE = 20

POST_SELECT = """SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p
  JOIN users u ON u.id = p.user_id"""
FEED_SQL = POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT $1"
POST_SQL = POST_SELECT + " WHERE p.id = $1"
INSERT_POST_SQL = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
INSERT_LIKE_SQL = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"


def pool_size() -> int:
    if os.environ.get("DB_POOL"):
        return int(os.environ["DB_POOL"])
    # Gunicorn exports nothing about the worker count, so it is recomputed exactly as in
    # gunicorn.conf.py (WEB_CONCURRENCY, else 1.5 x affinity cores).
    workers = int(os.environ.get("WEB_CONCURRENCY") or max(1, len(os.sched_getaffinity(0)) * 3 // 2))
    return max(2, 128 // workers)


def iso_millis_text(s: str) -> str:
    """Postgres text output of a timestamptz in a UTC session -> SPEC.md's format.

    '2026-10-27 12:34:56.123456+00' -> '2026-10-27T12:34:56.123Z'. Postgres drops trailing zeros
    of the fraction ('.5+00') and the whole fraction when it is zero ('12:34:56+00'), so the digits
    are padded; extra digits are cut off, which truncates (SPEC.md), it does not round.
    """
    if len(s) == 29:  # the usual case, all six fraction digits: '2026-10-27 12:34:56.123456+00'
        return s[:10] + "T" + s[11:23] + "Z"
    if s[19] == ".":
        frac = s[20:s.index("+", 20)][:3]
        return f"{s[:10]}T{s[11:19]}.{frac:0<3}Z"
    return f"{s[:10]}T{s[11:19]}.000Z"


async def init_connection(conn: asyncpg.Connection) -> None:
    # Binary timestamptz -> datetime -> string cost ~8% of the server's CPU (py-spy). With the
    # session in UTC (server_settings below), Postgres sends the text form, and slicing it is cheap.
    await conn.set_type_codec("timestamptz", schema="pg_catalog", encoder=str, decoder=iso_millis_text,
                              format="text")


async def connect() -> asyncpg.Connection:
    # TimeZone and DateStyle are session settings sent at connect (README rule 2 allows session SET).
    conn = await asyncpg.connect(DATABASE_URL, server_settings={"TimeZone": "UTC", "DateStyle": "ISO, MDY"})
    await init_connection(conn)
    return conn


class Pool:
    """A fixed set of asyncpg connections for one worker's event loop.

    asyncpg's own Pool costs more per request than this app can afford: each acquire makes a proxy
    object and a future, and each release runs in a new Task (asyncio.shield) and resets the session.
    Here a free connection is a list pop, and release hands the connection straight to the oldest
    waiting request. The app never changes session state, opens transactions or listens, so there
    is nothing to reset. A connection that dies is replaced in the background.
    """

    def __init__(self, conns: list, loop):
        self._free = conns  # LIFO: the most recently used connection is the warmest
        self._waiters = collections.deque()  # FIFO: requests waiting for a connection
        self._loop = loop
        self._closing = False

    def acquire(self, timeout: float | None = None) -> "_Acquire":
        return _Acquire(self, timeout)

    async def _get(self) -> asyncpg.Connection:
        free = self._free
        while free:
            conn = free.pop()
            if not conn.is_closed():
                return conn
            self.release(conn)  # closed while idle (Postgres restarted): schedule a replacement
        waiter = self._loop.create_future()
        self._waiters.append(waiter)
        try:
            return await waiter
        except BaseException:
            # Cancelled or timed out after release() already handed us a connection: pass it on.
            if waiter.done() and not waiter.cancelled():
                self.release(waiter.result())
            raise

    def release(self, conn: asyncpg.Connection) -> None:
        if conn.is_closed():
            if not self._closing:
                self._loop.create_task(self._replace())
            return
        waiters = self._waiters
        while waiters:
            waiter = waiters.popleft()
            if not waiter.done():
                waiter.set_result(conn)
                return
        self._free.append(conn)

    async def _replace(self) -> None:
        delay = 0.1
        while not self._closing:
            try:
                conn = await connect()
            except Exception:
                await asyncio.sleep(delay)
                delay = min(delay * 2, 5.0)
                continue
            self.release(conn)
            return

    async def close(self) -> None:
        self._closing = True
        for conn in self._free:
            await conn.close()


class _Acquire:
    __slots__ = ("pool", "timeout", "conn")

    def __init__(self, pool: Pool, timeout):
        self.pool = pool
        self.timeout = timeout

    async def __aenter__(self) -> asyncpg.Connection:
        if self.timeout is None:
            self.conn = await self.pool._get()
        else:
            self.conn = await asyncio.wait_for(self.pool._get(), self.timeout)
        return self.conn

    async def __aexit__(self, exc_type, exc, tb) -> None:
        conn = self.conn
        if exc_type is not None and issubclass(exc_type, asyncio.CancelledError):
            # A query cancelled mid-flight leaves the protocol busy: drop the connection instead
            # of handing it to the next request. release() then opens a new one.
            conn.terminate()
        self.pool.release(conn)


async def create_pool() -> Pool:
    size = pool_size()
    # Every connection is opened at worker start, so a burst never waits on a TCP + SCRAM handshake.
    conns = await asyncio.gather(*(connect() for _ in range(size)))
    return Pool(list(conns), asyncio.get_running_loop())
