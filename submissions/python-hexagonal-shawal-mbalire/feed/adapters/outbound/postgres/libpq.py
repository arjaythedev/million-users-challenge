"""PostRepository adapter: libpq (the C client library) in pipeline mode, driven by the event loop.

Model
-----
A lane is one Postgres connection in pipeline mode: many statements in flight, answered strictly in
order, so FIFO order *is* the correlation and no request ids are needed. Reads and writes ride
different lanes (see `feed.domain.capacity`, section 3), and each kind has its own batching:

Reads: one event-loop pass, one lane, one Sync
----------------------------------------------
Nothing is sent when a read is submitted. One flush is scheduled per event-loop iteration
(`call_soon`), so the reads that handlers issue during an iteration all go to one lane and leave
under one Sync in one write(2). Postgres sends everything up to a Sync in one flush, so a batch is
one write and one wake-up on each side, however large; spreading the same reads over every lane
would cost one of each per lane. A batch is never waited for: when idle, a read still leaves within
its own iteration. A read is answered as soon as its rows arrive.

Writes: group commit
--------------------
A write lane has at most one transaction in flight. Writes that arrive meanwhile queue, and the next
transaction takes them all: one Sync, one commit, one WAL flush. A write is answered only when its
Sync has been reached, which is after the commit (rule 8), never at its own result: until then the
transaction can still fail.

When a statement fails
----------------------
A statement that fails (a like on a missing post) aborts its whole batch. Postgres rolled back the
writes before it and skipped those after it, so nothing of the batch happened. Every write of a
failed batch runs again in a transaction of its own, each with its own answer, at the cost of one
retry. Reads have nothing to roll back: the failed one gets its error and only those skipped behind
it are sent again.

Failure and recovery
--------------------
A lane whose connection breaks fails the requests it holds (the fate of a write in flight is
unknown, so replaying it could double it), then reconnects in the background with capped exponential
backoff. Connecting blocks, so it runs on a thread of its own, which needs nothing of the loop but
`call_soon_threadsafe`. While a lane is down the repository leaves it out of the rotation, so later
requests go to healthy lanes.
"""
import asyncio
import threading
import time
from collections import deque
from collections.abc import Callable
from functools import partial
from operator import attrgetter

from ....application.ports import REPOSITORY_OPERATIONS, PostRepository
from ....domain.errors import PostNotFound
from ....domain.model import PostView
from ....domain.semantics import COMMAND, QUERY, semantics_of
from . import sql
from ._pq import ABORTED, SYNC, Conn, PqError

RECONNECT_FIRST_DELAY = 0.05  # seconds; doubles per failed attempt
RECONNECT_MAX_DELAY = 2.0
MAX_BATCH = 64  # statements under one Sync: enough to matter, and a bound on one failure's retries

_depth = attrgetter("depth")

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


def reconnect_in_background(loop, open_conn: Callable[[], Conn], attach: Callable[[Conn], None]) -> None:
    """Keeps trying `open_conn` off the loop, with backoff; `attach` runs on the loop when it worked."""
    def work() -> None:
        delay = RECONNECT_FIRST_DELAY
        while True:
            time.sleep(delay)
            try:
                conn = open_conn()
            except PqError:
                delay = min(delay * 2, RECONNECT_MAX_DELAY)
                continue
            try:
                loop.call_soon_threadsafe(partial(attach, conn))
            except RuntimeError:  # the loop has closed: nobody is left to want it
                conn.close()
            return
    threading.Thread(target=work, name="lane-reconnect", daemon=True).start()


class Lane:
    """One connection's lifecycle: flushing, reading, breaking and reconnecting. Subclasses decide
    when statements are sent and when their futures resolve."""
    __slots__ = ("_loop", "_open", "_conn", "_fd", "_flush_scheduled", "_writing", "_broken",
                 "on_change", "on_flush")

    def __init__(self, loop: asyncio.AbstractEventLoop, open_conn: Callable[[], Conn]):
        self._loop = loop
        self._open = open_conn
        self._flush_scheduled = False
        self.on_change: Callable[[], None] | None = None  # told when the lane goes down or comes back
        self.on_flush: Callable[[], None] | None = None   # told when this pass's batch has left
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

    def _schedule_flush(self) -> None:
        if not self._flush_scheduled:
            self._flush_scheduled = True
            self._loop.call_soon(self._flush)

    def _flush(self) -> None:
        self._flush_scheduled = False
        if self.on_flush is not None:
            self.on_flush()
        if self._broken is not None:
            return
        try:
            self._send_batch()
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
            self._on_replies(replies)
        except PqError as exc:  # also a re-send that found the connection gone
            self._fail(exc)

    def _fail(self, exc: BaseException) -> None:
        """Fail every request the broken connection holds, loudly, then start reconnecting."""
        self._broken = exc
        self._loop.remove_reader(self._fd)
        if self._writing:
            self._loop.remove_writer(self._fd)
        self._fail_held(exc)
        self._conn.close()
        reconnect_in_background(self._loop, self._open, self._reconnected)
        if self.on_change is not None:
            self.on_change()

    def _reconnected(self, conn: Conn) -> None:
        self._attach(conn)
        if self.on_change is not None:
            self.on_change()

    def _send_batch(self) -> None:
        raise NotImplementedError

    def _on_replies(self, replies: list) -> None:
        raise NotImplementedError

    def _fail_held(self, exc: BaseException) -> None:
        raise NotImplementedError


def _resolve(future: asyncio.Future, reply) -> None:
    if future.done():  # the awaiting request was cancelled (its client went away)
        return
    if isinstance(reply, PqError):
        future.set_exception(reply)
    else:
        future.set_result(reply)


class ReadLane(Lane):
    __slots__ = ("_waiting", "batch_size")

    def __init__(self, loop, open_conn):
        self._waiting: deque[tuple] = deque()  # (future, statement, params) per statement sent
        self.batch_size = 0                    # statements sent since the last Sync
        super().__init__(loop, open_conn)

    @property
    def depth(self) -> int:
        return len(self._waiting)

    def submit(self, statement: str, params: tuple) -> asyncio.Future:
        if self._broken is not None:
            raise self._broken
        try:
            self._conn.send(statement, params)
        except PqError as exc:
            self._fail(exc)
            raise
        future = self._loop.create_future()
        self._sent(future, statement, params)
        return future

    def _sent(self, future, statement, params) -> None:
        self._waiting.append((future, statement, params))
        self.batch_size += 1
        self._schedule_flush()

    def _send_batch(self) -> None:
        if self.batch_size:
            self.batch_size = 0
            self._conn.sync()

    def _on_replies(self, replies: list) -> None:
        waiting = self._waiting
        for reply in replies:
            if reply is SYNC:
                continue
            future, statement, params = waiting.popleft()
            if reply is ABORTED:
                self._sent(future, statement, params)  # first, so that a send that fails fails it too
                self._conn.send(statement, params)
            else:
                _resolve(future, reply)

    def _fail_held(self, exc: BaseException) -> None:
        self.batch_size = 0
        while self._waiting:
            _resolve(self._waiting.popleft()[0], exc)


class WriteLane(Lane):
    __slots__ = ("_queue", "_retry", "_in_flight", "_results")

    def __init__(self, loop, open_conn):
        self._queue: list[tuple] = []          # (future, statement, params) not sent yet
        self._retry: list[tuple] = []          # the writes of a failed batch, to run one by one
        self._in_flight: deque[list] = deque() # the transactions sent, each a list of writes
        self._results: list = []               # the replies of the oldest transaction, so far
        super().__init__(loop, open_conn)

    @property
    def depth(self) -> int:
        return len(self._queue) + len(self._retry) + sum(map(len, self._in_flight))

    @property
    def batch_size(self) -> int:
        return len(self._queue)

    def submit(self, statement: str, params: tuple) -> asyncio.Future:
        if self._broken is not None:
            raise self._broken
        future = self._loop.create_future()
        self._queue.append((future, statement, params))
        self._schedule_flush()
        return future

    def _send_batch(self) -> None:
        if self._in_flight:
            return  # the next transaction leaves when this one has committed
        if self._retry:
            batches = [[write] for write in self._retry]
            self._retry = []
        elif self._queue:
            batches = [self._queue]
            self._queue = []
        else:
            return
        for batch in batches:
            self._in_flight.append(batch)
            for _, statement, params in batch:
                self._conn.send(statement, params)
            self._conn.sync()

    def _on_replies(self, replies: list) -> None:
        for reply in replies:
            if reply is SYNC:
                self._transaction_ended()
            else:
                self._results.append(reply)
        if not self._in_flight and (self._queue or self._retry):
            self._schedule_flush()

    def _transaction_ended(self) -> None:
        batch = self._in_flight.popleft()
        results, self._results = self._results, []
        if len(results) > len(batch):  # an error with no statement to blame: the commit itself failed
            failure = results[-1]
            for future, _, _ in batch:
                _resolve(future, failure)
        elif len(batch) > 1 and any(r is ABORTED or isinstance(r, PqError) for r in results):
            self._retry.extend(batch)  # the whole batch rolled back: nothing has happened yet
        else:
            for (future, _, _), result in zip(batch, results):
                _resolve(future, result)

    def _fail_held(self, exc: BaseException) -> None:
        for batch in self._in_flight:
            for future, _, _ in batch:
                _resolve(future, exc)
        for future, _, _ in self._queue + self._retry:
            _resolve(future, exc)
        self._in_flight.clear()
        self._queue, self._retry, self._results = [], [], []


class Rotation:
    """Which lane of a kind takes the next statement: the least loaded healthy one at the start of
    an event-loop pass, and the same one for the rest of the pass, so that the pass's statements
    make one batch. A full batch moves on to the next least loaded. If every lane is down the
    rotation keeps them all, so requests fail fast with the connection error instead of queueing
    behind a reconnect. Lane health is handled when it changes, not on every query."""
    __slots__ = ("_lanes", "_up", "_current")

    def __init__(self, lanes: list[Lane]):
        self._lanes = lanes
        self._current: Lane | None = None
        self.lanes_changed()
        for lane in lanes:
            lane.on_change = self.lanes_changed
            lane.on_flush = self.pass_ended

    def lanes_changed(self) -> None:
        self._up = [lane for lane in self._lanes if lane.healthy] or self._lanes
        self._current = None

    def pass_ended(self) -> None:
        self._current = None

    def __call__(self) -> Lane:
        lane = self._current
        if lane is None or lane.batch_size >= MAX_BATCH:
            lane = self._current = min(self._up, key=_depth)
        return lane


class LibpqPostRepository:
    """Each operation's lanes are chosen once, at construction, from the port's declared semantics:
    `@query` operations ride the read lanes, `@command` operations the write lanes. The adapter
    holds no list of its own about which statements write."""

    STATEMENTS = STATEMENTS
    __slots__ = ("_lane",)

    def __init__(self, read_lanes: list[ReadLane], write_lanes: list[WriteLane]):
        rotations = {QUERY: Rotation(read_lanes), COMMAND: Rotation(write_lanes)}
        self._lane: dict[str, Rotation] = {
            op: rotations[semantics_of(getattr(PostRepository, op)).kind] for op in REPOSITORY_OPERATIONS
        }

    @classmethod
    async def connect(cls, dsn: str, read_lanes: int, write_lanes: int):
        loop = asyncio.get_running_loop()
        open_conn = opener(dsn, cls.STATEMENTS)
        return cls([ReadLane(loop, open_conn) for _ in range(read_lanes)],
                   [WriteLane(loop, open_conn) for _ in range(write_lanes)])

    async def newest(self) -> list[PostView]:
        return await self._lane["newest"]().submit("feed", ())

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
