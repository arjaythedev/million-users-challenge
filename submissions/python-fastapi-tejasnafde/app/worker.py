from uvicorn.protocols.http import httptools_impl
from uvicorn_worker import UvicornWorker

from .coalesce import CoalescingCycle

# One write per response (see coalesce.py). Set at import, before any connection is accepted.
httptools_impl.RequestResponseCycle = CoalescingCycle


class Worker(UvicornWorker):
    """UvicornWorker with uvloop + httptools named explicitly (what "auto" picks when
    uvicorn[standard] is installed) and uvicorn's access log off. Subclassing CONFIG_KWARGS is the
    documented way to pass uvicorn settings that gunicorn has no flag for."""

    # proxy_headers: uvicorn wraps the app in ProxyHeadersMiddleware by default, which reads
    # X-Forwarded-For/-Proto on every request (~2% of a worker's CPU). The server faces clients
    # directly (rule 11), so there is no proxy whose headers to trust. server_header: drops the
    # "server: uvicorn" line from every response.
    CONFIG_KWARGS = {"loop": "uvloop", "http": "httptools", "access_log": False,
                     "proxy_headers": False, "server_header": False}
