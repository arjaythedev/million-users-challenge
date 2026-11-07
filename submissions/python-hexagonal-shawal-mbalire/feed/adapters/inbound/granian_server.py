"""Driving adapter (server): granian (Rust, hyper), embedded in this worker's event loop.

Granian normally forks its own workers; here its embedded server runs inside the worker the
runtime already forked, so process topology, database lanes and supervision stay the runtime's,
whatever the HTTP server. One embedded server per port. Granian binds its own sockets
(SO_REUSEPORT on Linux), so every worker accepts on both ports, as with the other servers.

The HTTP parsing and connection handling run on a Rust thread; Python only sees each request
once it is fully parsed. `header_read_timeout` is raised well above the contract's 75 s so idle
keep-alive connections are never cut early.
"""
import asyncio

from granian.constants import HTTPModes, Interfaces
from granian.http import HTTP1Settings
from granian.log import LogLevels
from granian.server.embed import Server

from .policy import LISTEN_BACKLOG, Listen


async def serve(app, listen: Listen) -> None:
    interface = Interfaces.RSGI if hasattr(app, "__rsgi__") else Interfaces.ASGINL
    servers = [
        Server(
            app,
            address=listen.host,
            port=port,
            interface=interface,
            http=HTTPModes.http1,
            websockets=False,
            backlog=LISTEN_BACKLOG,
            backpressure=LISTEN_BACKLOG,
            http1_settings=HTTP1Settings(header_read_timeout=300_000, keep_alive=True),
            log_level=LogLevels.warning,
            log_access=False,
        )
        for port in listen.ports
    ]
    await asyncio.gather(*(s.serve() for s in servers))
