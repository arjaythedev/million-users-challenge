"""Driving adapter (server): uvicorn, hosting any ASGI app (bare or FastAPI) on this worker's sockets."""
import uvicorn

from .policy import KEEP_ALIVE_S, LISTEN_BACKLOG, Listen


async def serve(app, listen: Listen) -> None:
    config = uvicorn.Config(
        app,
        loop="none",          # the runtime already installed uvloop
        http="httptools",
        lifespan="off",       # the composition root owns startup
        access_log=False,
        log_level="warning",
        timeout_keep_alive=KEEP_ALIVE_S,
        backlog=LISTEN_BACKLOG,
    )
    await uvicorn.Server(config).serve(sockets=[listen.bind(port) for port in listen.ports])
