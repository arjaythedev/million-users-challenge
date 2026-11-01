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

Failure and recovery
--------------------
A lane whose connection breaks fails the requests already in flight on it (their replies are lost
with the socket; replaying them could double a write), then reconnects in the background with
capped exponential backoff. Connecting blocks, so it runs on the loop's executor. While a lane is
down the repository leaves it out of the rotation, so later requests go to healthy lanes: one
dropped connection costs one lane's in-flight requests, not a 1/N share of every request after it.
"""
import asyncio
from collections import deque
from collections.abc import Callable
from itertools import cycle

from ....application.ports import REPOSITORY_OPERATIONS, PostRepository
from ....domain.errors import PostNotFound
from ....domain.model import PostView
from ....domain.semantics import COMMAND, QUERY, semantics_of
from . import sql
from ._pq import Conn, PqError

RECONNECT_FIRST_DELAY = 0.05  # seconds; doubles per failed attempt
RECONNECT_MAX_DELAY = 2.0

STATEMENTS = {
    "feed": sql.FEED,
    "post": sql.POST_BY_ID,
    "insert_post": sql.INSERT_POST,
    "insert_like": sql.INSERT_LIKE,
    "ping": sql.PING,
}


def opener(dsn: str, statements: dict[str, str]) -> Callable[[], Conn]:
    """A blocking factory for ready lanes' connections: used at startup and on every reconnect."""
    def open_conn() -> Conn:
        conn = Conn(dsn)
        # Session-level settings (allowed by rule 2): the C decoder relies on UTC ISO timestamps.
        conn.execute("SET TimeZone = 'UTC'")
        conn.execute("SET DateStyle = 'ISO'")
        for name, text in statements.items():
            conn.prepare(name, text)
        conn.enter_pipeline()
        return conn
    return open_conn


class Lane:
    __slots__ = ("_loop", "_open", "_conn", "_fd", "_waiters", "_flush_scheduled", "_writing", "_broken",
                 "_recovery", "on_change")

    def __init__(self, loop: asyncio.AbstractEventLoop, open_conn: Callable[[], Conn]):
        self._loop = loop
        self._open = open_conn
        self._waiters: deque[asyncio.Future] = deque()
        self._flush_scheduled = False
        self._recovery: asyncio.Task | None = None
        self.on_change: Callable[[], None] | None = None  # told when the lane goes down or comes back
        self._attach(open_conn())

    def _attach(self, conn: Conn) -> None:
        self._conn = conn
        self._fd = conn.fileno()
        self._writing = False
        self._broken: BaseException | None = None
        self._loop.add_reader(self._fd, self._on_readable)

    @property
    def healthy(self) -> bool:
        return self._broken is None

    def submit(self, statement: str, params: tuple) -> asyncio.Future:
        if self._broken is not None:
            raise self._broken
        try:
            self._conn.send(statement, params)
        except PqError as exc:
            self._fail(exc)
            raise
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
        """Fail every request waiting on the broken connection, loudly, then start reconnecting."""
        self._broken = exc
        self._loop.remove_reader(self._fd)
        if self._writing:
            self._loop.remove_writer(self._fd)
        while self._waiters:
            future = self._waiters.popleft()
            if not future.done():
                future.set_exception(exc)
        self._conn.close()
        self._recovery = self._loop.create_task(self._reconnect())
        if self.on_change is not None:
            self.on_change()

    async def _reconnect(self) -> None:
        delay = RECONNECT_FIRST_DELAY
        while True:
            await asyncio.sleep(delay)
            try:
                conn = await self._loop.run_in_executor(None, self._open)
            except PqError:
                delay = min(delay * 2, RECONNECT_MAX_DELAY)
                continue
            self._attach(conn)
            self._recovery = None
            if self.on_change is not None:
                self.on_change()
            return

    @property
    def depth(self) -> int:
        return len(self._waiters)


class LibpqPostRepository:
    """Each operation's lane is chosen once, at construction, from the port's declared semantics:
    `@query` operations ride the read lanes, `@command` operations the write lanes. The adapter
    holds no list of its own about which statements write."""

    STATEMENTS = STATEMENTS
    __slots__ = ("_lane", "_lanes")

    def __init__(self, read_lanes: list[Lane], write_lanes: list[Lane]):
        self._lanes = {QUERY: read_lanes, COMMAND: write_lanes}
        self._lane: dict[str, Callable[[], Lane]] = {}
        self._rotate()
        for lane in read_lanes + write_lanes:
            lane.on_change = self._rotate

    def _rotate(self) -> None:
        """(Re)build each operation's picker over the healthy lanes of its kind.

        Round-robin, not least-loaded: lanes see statistically identical traffic, and a cycle()
        step is one C call where a min() over depths is a Python-level scan per query. Lane health
        is handled here, when it changes, not on every query. If every lane of a kind is down the
        rotation keeps them all, so requests fail fast with the connection error instead of
        queueing behind a reconnect."""
        pick = {}
        for kind, lanes in self._lanes.items():
            healthy = [lane for lane in lanes if lane.healthy]
            pick[kind] = cycle(healthy or lanes).__next__
        for op in REPOSITORY_OPERATIONS:
            self._lane[op] = pick[semantics_of(getattr(PostRepository, op)).kind]

    @classmethod
    async def connect(cls, dsn: str, read_lanes: int, write_lanes: int):
        loop = asyncio.get_running_loop()
        open_conn = opener(dsn, cls.STATEMENTS)
        return cls([Lane(loop, open_conn) for _ in range(read_lanes)],
                   [Lane(loop, open_conn) for _ in range(write_lanes)])

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
