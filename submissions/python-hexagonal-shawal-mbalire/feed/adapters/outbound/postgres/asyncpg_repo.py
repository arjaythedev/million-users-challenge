"""PostRepository adapter: asyncpg with a connection pool.

The conventional Python choice, kept as the reference implementation that `pg_libpq` must agree
with. asyncpg sends one query per connection at a time (no pipelining across coroutines), so its
throughput is bounded by pool_size / round-trip time per worker; the pool is sized from the same
capacity model; `@query` operations use the read pool and `@command` operations the write pool,
chosen from the port's declared semantics, for the same head-of-line reason as the libpq lanes.
"""
import asyncpg

from ....application.ports import REPOSITORY_OPERATIONS, PostRepository
from ....domain.errors import PostNotFound
from ....domain.model import PostView
from ....domain.semantics import COMMAND, QUERY, semantics_of
from ....domain.timefmt import iso_millis
from . import sql


def _view(r) -> PostView:
    return (r[0], r[1], iso_millis(r[2]), r[3], r[4])


class AsyncpgPostRepository:
    __slots__ = ("_pool",)

    def __init__(self, reads: asyncpg.Pool, writes: asyncpg.Pool):
        pick = {QUERY: reads, COMMAND: writes}
        self._pool = {op: pick[semantics_of(getattr(PostRepository, op)).kind] for op in REPOSITORY_OPERATIONS}

    @classmethod
    async def connect(cls, dsn: str, read_lanes: int, write_lanes: int) -> "AsyncpgPostRepository":
        def pool(size: int):
            # min = max: every connection opens at startup and none is closed when idle, so a burst
            # never waits on a TCP + SCRAM handshake.
            return asyncpg.create_pool(dsn, min_size=size, max_size=size, max_inactive_connection_lifetime=0)
        return cls(await pool(read_lanes), await pool(write_lanes))

    async def newest(self, limit: int) -> list[PostView]:
        return [_view(r) for r in await self._pool["newest"].fetch(sql.FEED, limit)]

    async def by_id(self, post_id: int) -> PostView | None:
        r = await self._pool["by_id"].fetchrow(sql.POST_BY_ID, post_id)
        return None if r is None else _view(r)

    async def create(self, user_id: int, body: str) -> tuple[int, str]:
        post_id, created_at = await self._pool["create"].fetchrow(sql.INSERT_POST, user_id, body)
        return post_id, iso_millis(created_at)

    async def like(self, user_id: int, post_id: int) -> bool:
        try:
            tag = await self._pool["like"].execute(sql.INSERT_LIKE, user_id, post_id)
        except asyncpg.ForeignKeyViolationError:
            raise PostNotFound from None
        return tag == "INSERT 0 1"

    async def ping(self) -> None:
        await self._pool["ping"].fetchval(sql.PING)
