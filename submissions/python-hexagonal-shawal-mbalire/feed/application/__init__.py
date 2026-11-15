"""The application layer: use cases, the ports they need, and the API contract they speak.

     driving side                        the hexagon                        driven side
  ------------------           ------------------------------         -------------------------
  servers: raw | uvicorn  -->  Router  (built from @route)            PostRepository: libpq |
           | granian                     |                              asyncpg | pgjson*
  apps:    raw | bare ASGI                v                     -->   JsonCodec: cjson |
           | FastAPI | RSGI    FeedService (use cases,                 handwritten | orjson |
                                @query / @command)                      pydantic | pgjson*
                                         |                            Authenticator: HS256
                                         v
                               contract: Reply(status, bytes)        * breaks rule 3; experiment only

  profiling: a decorator that can wrap any port above, on either side, without either side knowing.

The use cases answer in the API's own terms, a `Reply(status, body)`, because the spec defines the
API as exact bytes and status codes. Every HTTP adapter therefore produces byte-identical output
by construction instead of re-implementing the error table, key order and timestamp format with
its own framework's serializer. An HTTP adapter's whole job is: match, call, write.

Imports allowed here: `feed.domain` and the standard library. Nothing else (enforced by tests).
"""
