"""PostRepository adapter: libpq (the C client library) in pipeline mode, driven by the event loop.

Model
-----
A `Lane` is one Postgres connection with a FIFO of futures. Sending a query appends a future and
queues the bytes in libpq; the reply resolves the oldest future. Because Postgres answers a
pipeline strictly in order, FIFO order *is* the correlation, and no request ids are needed.

Batching without waiting
------------------------
Queries are only buffered when sent. One flush is scheduled per event-loop iteration
(`call_soon`), so every query that request handlers issued during that iteration leaves in a
single write(2), and Postgres reads them in one recv. Under load that coalesces dozens of queries
per syscall; when idle a query still leaves within the same iteration, so batching never adds a
timer's worth of latency. This is Nagle's idea without Nagle's delay.

Reads and writes ride different lanes (see `feed.domain.capacity`, section 3): an INSERT waiting
on its WAL flush must not hold back the reads behind it.
"""
import asyncio
from collections import deque
from itertools import cycle

from ....application.ports import REPOSITORY_OPERATIONS, PostRepository
from ....domain.errors import PostNotFound
from ....domain.model import PostView
from ....domain.semantics import COMMAND, QUERY, semantics_of
from . import sql
from ._pq import Conn, PqError

STATEMENTS = {
    "feed": sql.FEED,
    "post": sql.POST_BY_ID,
    "insert_post": sql.INSERT_POST,
    "insert_like": sql.INSERT_LIKE,
    "ping": sql.PING,
}


class Lane:
    __slots__ = ("_loop", "_conn", "_fd", "_waiters", "_flush_scheduled", "_writing", "_broken")

    def __init__(self, loop: asyncio.AbstractEventLoop, dsn: str, statements: dict[str, str]):
        conn = Conn(dsn)
        # Session-level settings (allowed by rule 2): the C decoder relies on UTC ISO timestamps.
        conn.execute("SET TimeZone = 'UTC'")
        conn.execute("SET DateStyle = 'ISO'")
        for name, text in statements.items():
            conn.prepare(name, text)
        conn.enter_pipeline()
        self._loop = loop
        self._conn = conn
        self._fd = conn.fileno()
        self._waiters: deque[asyncio.Future] = deque()
        self._flush_scheduled = False
        self._writing = False
        self._broken: BaseException | None = None
        loop.add_reader(self._fd, self._on_readable)

    def submit(self, statement: str, params: tuple) -> asyncio.Future:
        if self._broken is not None:
            raise self._broken
        self._conn.send(statement, params)
        future = self._loop.create_future()
        self._waiters.append(future)
        if not self._flush_scheduled:
            self._flush_scheduled = True
            self._loop.call_soon(self._flush)
        return future

    def _flush(self) -> None:
        self._flush_scheduled = False
        if self._broken is not None:
            return
        try:
            pending = self._conn.flush()
        except PqError as exc:
            return self._fail(exc)
        if pending and not self._writing:  # socket buffer full: finish when writable
            self._writing = True
            self._loop.add_writer(self._fd, self._on_writable)

    def _on_writable(self) -> None:
        try:
            pending = self._conn.flush()
        except PqError as exc:
            return self._fail(exc)
        if not pending:
            self._writing = False
            self._loop.remove_writer(self._fd)

    def _on_readable(self) -> None:
        try:
            replies = self._conn.read()
        except PqError as exc:
            return self._fail(exc)
        waiters = self._waiters
        for reply in replies:
            future = waiters.popleft()
            if future.done():  # the awaiting request was cancelled (its client went away)
                continue
            if isinstance(reply, PqError):
                future.set_exception(reply)
            else:
                future.set_result(reply)

    def _fail(self, exc: BaseException) -> None:
        """A broken connection fails every request waiting on it, and every later one, loudly."""
        self._broken = exc
        self._loop.remove_reader(self._fd)
        if self._writing:
            self._loop.remove_writer(self._fd)
        while self._waiters:
            future = self._waiters.popleft()
            if not future.done():
                future.set_exception(exc)
        self._conn.close()

    @property
    def depth(self) -> int:
        return len(self._waiters)


class LibpqPostRepository:
    """Each operation's lane is chosen once, at construction, from the port's declared semantics:
    `@query` operations ride the read lanes, `@command` operations the write lanes. The adapter
    holds no list of its own about which statements write."""

    STATEMENTS = STATEMENTS
    __slots__ = ("_lane",)

    def __init__(self, read_lanes: list[Lane], write_lanes: list[Lane]):
        # Round-robin, not least-loaded: lanes see statistically identical traffic, and a cycle()
        # step is one C call where a min() over depths is a Python-level scan per query.
        pick = {QUERY: cycle(read_lanes).__next__, COMMAND: cycle(write_lanes).__next__}
        self._lane = {op: pick[semantics_of(getattr(PostRepository, op)).kind] for op in REPOSITORY_OPERATIONS}

    @classmethod
    async def connect(cls, dsn: str, read_lanes: int, write_lanes: int):
        loop = asyncio.get_running_loop()
        return cls([Lane(loop, dsn, cls.STATEMENTS) for _ in range(read_lanes)],
                   [Lane(loop, dsn, cls.STATEMENTS) for _ in range(write_lanes)])

    async def newest(self, limit: int) -> list[PostView]:
        return await self._lane["newest"]().submit("feed", (limit,))

    async def by_id(self, post_id: int) -> PostView | None:
        rows = await self._lane["by_id"]().submit("post", (post_id,))
        return rows[0] if rows else None

    async def create(self, user_id: int, body: str) -> tuple[int, str]:
        rows = await self._lane["create"]().submit("insert_post", (user_id, body))
        return rows[0]

    async def like(self, user_id: int, post_id: int) -> bool:
        try:
            inserted = await self._lane["like"]().submit("insert_like", (user_id, post_id))
        except PqError as exc:
            if exc.args[0] == sql.FOREIGN_KEY_VIOLATION:
                raise PostNotFound from None
            raise
        return inserted == 1

    async def ping(self) -> None:
        await self._lane["ping"]().submit("ping", ())
