"""Composition root: the only module that knows every adapter exists.

Every port has a registry of adapters, selected by name from the configuration:

    FEED_SERVER  raw | uvicorn | granian                       (driving: who accepts connections)
    FEED_APP     raw | bare | fastapi | rsgi                   (driving: who turns HTTP into calls)
    FEED_DB      libpq | asyncpg | pgjson                      (driven: PostRepository)
    FEED_JSON    cjson | handwritten | orjson | pydantic | pgjson  (driven: JsonCodec)
    FEED_PROFILE latency,cprofile                              (decorators around every port)
    FEED_GC      survivors | generational                      (runtime: cyclic-GC policy)
    FEED_GZIP    libdeflate | zlib                             (transport: response compressor)

Adapters are imported lazily, inside their factory, so a worker running raw sockets over libpq
never imports FastAPI, Pydantic, granian or asyncpg: unused stacks cost no memory and no import
time. Adding an adapter means adding one entry here; nothing in `feed.domain` or
`feed.application` changes, and `tests/test_architecture.py` fails if anything there starts
depending on one.
"""
import asyncio
import importlib
import sys
from typing import NamedTuple

from .application.ports import Authenticator, JsonCodec, PostRepository
from .application.routes import Router
from .application.service import FeedService
from .runtime.config import Config
from .runtime.prefork import listening_socket, supervise


class Choice(NamedTuple):
    module: str
    attr: str
    compliant: bool = True  # False: violates a challenge rule; for experiments only


def load(choice: Choice):
    return getattr(importlib.import_module(choice.module, __package__), choice.attr)


SERVERS = {
    "raw": Choice(".adapters.inbound.raw", "serve"),
    "uvicorn": Choice(".adapters.inbound.uvicorn_server", "serve"),
    "granian": Choice(".adapters.inbound.granian_server", "serve"),
}
APPS = {
    "raw": Choice(".adapters.inbound.raw", "build"),
    "bare": Choice(".adapters.inbound.asgi_bare", "build"),
    "fastapi": Choice(".adapters.inbound.fastapi_app", "build"),
    "rsgi": Choice(".adapters.inbound.rsgi_bare", "build"),
}
# Which apps each server can host, the first being its default.
SERVER_APPS = {"raw": ("raw",), "uvicorn": ("bare", "fastapi"), "granian": ("rsgi", "bare", "fastapi")}
REPOSITORIES = {
    "libpq": Choice(".adapters.outbound.postgres.libpq", "LibpqPostRepository"),
    "asyncpg": Choice(".adapters.outbound.postgres.asyncpg_repo", "AsyncpgPostRepository"),
    "pgjson": Choice(".adapters.outbound.postgres.pgjson", "PgJsonPostRepository", compliant=False),
}
CODECS = {
    "cjson": Choice(".adapters.outbound.json.cjson", "CJson"),
    "handwritten": Choice(".adapters.outbound.json.handwritten", "HandwrittenJson"),
    "orjson": Choice(".adapters.outbound.json.orjson_codec", "OrJson"),
    "pydantic": Choice(".adapters.outbound.json.pydantic_codec", "PydanticJson"),
    "pgjson": Choice(".adapters.outbound.json.pgjson", "PgJson", compliant=False),
}
GC_POLICIES = {
    "survivors": Choice(".runtime.memory", "survivors"),
    "generational": Choice(".runtime.memory", "generational"),
}

COMPRESSORS = {
    "libdeflate": Choice(".adapters.inbound._deflate", "gzip"),
    "zlib": Choice(".adapters.inbound.policy", "zlib_gzip"),
}


def resolve(cfg: Config) -> dict[str, Choice]:
    """Validate the combination before forking anything, so a typo fails once, loudly."""
    def pick(registry, name, what):
        if name not in registry:
            sys.exit(f"{what}={name!r}: choose one of {sorted(registry)}")
        return registry[name]

    app = cfg.app or SERVER_APPS.get(cfg.server, ("",))[0]
    chosen = {
        "server": pick(SERVERS, cfg.server, "FEED_SERVER"),
        "app": pick(APPS, app, "FEED_APP"),
        "db": pick(REPOSITORIES, cfg.db, "FEED_DB"),
        "codec": pick(CODECS, cfg.codec, "FEED_JSON"),
        "gc": pick(GC_POLICIES, cfg.gc, "FEED_GC"),
        "gzip": pick(COMPRESSORS, cfg.gzip, "FEED_GZIP"),
    }
    if app not in SERVER_APPS[cfg.server]:
        sys.exit(f"FEED_SERVER={cfg.server} hosts FEED_APP in {SERVER_APPS[cfg.server]}, not {app!r}")
    if (cfg.db == "pgjson") != (cfg.codec == "pgjson"):
        sys.exit("FEED_DB=pgjson and FEED_JSON=pgjson only work together")
    unrules = [k for k, c in chosen.items() if not c.compliant]
    if unrules and not cfg.allow_noncompliant:
        sys.exit(f"{', '.join(unrules)} adapter(s) break challenge rule 3 (JSON built in SQL); "
                 "set FEED_ALLOW_NONCOMPLIANT=1 to run the experiment anyway")
    unknown = cfg.profile - {"latency", "cprofile"}
    if unknown:
        sys.exit(f"FEED_PROFILE: unknown mode(s) {sorted(unknown)}")
    return chosen


async def run_worker(cfg: Config, chosen: dict[str, Choice], slot: int) -> None:
    from .adapters.inbound.policy import LISTEN_BACKLOG, Listen, use_gzip
    from .adapters.outbound.jwt_hs256 import Hs256Authenticator
    from .adapters.profiling import Profiler

    profiler = Profiler(cfg.profile, cfg.profile_dir, slot)
    serve, build_app = load(chosen["server"]), load(chosen["app"])
    repository = await load(chosen["db"]).connect(cfg.database_url, cfg.read_lanes, cfg.write_lanes)
    service = FeedService(
        profiler.decorate(repository, PostRepository, "db"),
        profiler.decorate(Hs256Authenticator(cfg.jwt_secret), Authenticator, "auth"),
        profiler.decorate(load(chosen["codec"])(), JsonCodec, "json"),
    )
    router = Router(profiler.decorate(service, FeedService, "use_case"))
    app = build_app(router)
    use_gzip(load(chosen["gzip"]))
    # Everything allocated so far lives for the whole process: the policy freezes it, and
    # decides what happens to what survives later (see feed/runtime/memory.py).
    load(chosen["gc"])()
    profiler.start()
    # Sockets are bound only now, after the database lanes are up: a worker never accepts a
    # connection it cannot serve.
    await serve(app, Listen(cfg.host, cfg.ports, lambda port: listening_socket(cfg.host, port, LISTEN_BACKLOG)))


def worker_main(cfg: Config, chosen: dict[str, Choice], slot: int) -> None:
    import uvloop

    loop = uvloop.new_event_loop()
    asyncio.set_event_loop(loop)
    # Eager tasks run a handler synchronously up to its first real suspension (the database), which
    # skips a full scheduling round trip through the loop's ready queue for every request.
    loop.set_task_factory(asyncio.eager_task_factory)
    loop.run_until_complete(run_worker(cfg, chosen, slot))


def main() -> None:
    cfg = Config.from_env()
    chosen = resolve(cfg)
    print(f"feed: {cfg.workers} workers x ({cfg.read_lanes} read + {cfg.write_lanes} write lanes); "
          f"server={cfg.server} app={cfg.app or SERVER_APPS[cfg.server][0]} db={cfg.db} json={cfg.codec} "
          f"profile={','.join(sorted(cfg.profile)) or 'off'} gc={cfg.gc} gzip={cfg.gzip}; listening on {cfg.host}:{','.join(map(str, cfg.ports))}",
          file=sys.stderr, flush=True)
    supervise(cfg.workers, lambda slot: worker_main(cfg, chosen, slot))
