"""PostRepository adapter: libpq lanes, but Postgres renders each post as JSON (`row_to_json`).

NOT ALLOWED IN THE BENCHMARK. Rule 3 forbids "building the JSON in SQL": it moves CPU from the
app box (which is being measured) to the database box (which is declared never the bottleneck).
It is here as a controlled experiment: same lanes, same pipelining, only the location of the
serialization work differs, so `FEED_DB=pgjson` vs `FEED_DB=libpq` isolates its cost. The
composition root refuses it unless FEED_ALLOW_NONCOMPLIANT=1, and it must be paired with the
`pgjson` codec, which passes the JSON text through.

`row_to_json` (unlike `json_build_object`) emits compact JSON with keys in column order, and
`to_char(... MS)` truncates to milliseconds, so the bytes match the other adapters exactly.
"""
from .libpq import STATEMENTS, LibpqPostRepository

_AS_JSON = """SELECT row_to_json(t)::text FROM (
  SELECT p.id, p.body,
         to_char(p.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS created_at,
         u.username AS author,
         (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
    FROM posts p JOIN users u ON u.id = p.user_id
   {where}) t"""


class PgJsonPostRepository(LibpqPostRepository):
    COMPLIANT = False
    STATEMENTS = {
        **STATEMENTS,
        "feed": _AS_JSON.format(where="ORDER BY p.created_at DESC, p.id DESC LIMIT $1"),
        "post": _AS_JSON.format(where="WHERE p.id = $1"),
    }
    __slots__ = ()

    async def newest(self, limit: int) -> list[str]:
        return [r[0] for r in await self._lane["newest"]().submit("feed", (limit,))]

    async def by_id(self, post_id: int) -> str | None:
        rows = await self._lane["by_id"]().submit("post", (post_id,))
        return rows[0][0] if rows else None
