"""JsonCodec adapter for the `pgjson` repository: posts arrive from Postgres already as JSON text.

The repository returns each post as the text of `row_to_json(...)` (compact, keys in column
order); this codec only wraps and joins those strings. Anything that did not come from Postgres
(a freshly created post, likes, health) is encoded by the hand-written codec.

NOT ALLOWED IN THE BENCHMARK (rule 3: "building the JSON in SQL" moves work into Postgres). It
exists to measure what that move is worth; the composition root refuses it unless
FEED_ALLOW_NONCOMPLIANT=1.
"""
from .handwritten import HandwrittenJson


class PgJson(HandwrittenJson):
    __slots__ = ()

    def post(self, view) -> bytes:
        if isinstance(view, str):
            return b'{"post":' + view.encode() + b"}"
        return super().post(view)

    def feed(self, views) -> bytes:
        return b'{"posts":[' + ",".join(views).encode() + b"]}"
