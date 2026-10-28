"""One write per response instead of two.

ASGI sends a response as two messages, http.response.start (status + headers) and
http.response.body, and uvicorn writes each to the socket as it arrives: two send() system calls
and usually two TCP segments for every response. This cycle keeps the status line and headers
and writes them in the same call as the body. Everything else, including every check uvicorn makes
on the headers and the body length, is uvicorn's own code.

uvicorn's httptools protocol creates its cycles through the module-level name
RequestResponseCycle, so worker.py points that name at this subclass before the server starts.
"""
from uvicorn.protocols.http.httptools_impl import RequestResponseCycle


class _HeadCapture:
    """Stands in for the transport while uvicorn serializes the headers: keeps the bytes."""

    __slots__ = ("data",)

    def write(self, data: bytes) -> None:
        self.data = data


class _HeadThenBody:
    """Stands in for the transport while uvicorn writes the body: one write of head + body."""

    __slots__ = ("transport", "head")

    def __init__(self, transport, head: bytes):
        self.transport = transport
        self.head = head

    def write(self, data: bytes) -> None:
        head = self.head
        if head:
            self.head = b""
            data = head + data
        self.transport.write(data)

    def close(self) -> None:  # uvicorn closes the connection after a "connection: close" response
        if self.head:  # a HEAD response has no body write: the head must still go out first
            self.transport.write(self.head)
            self.head = b""
        self.transport.close()

    def __getattr__(self, name):
        return getattr(self.transport, name)


class CoalescingCycle(RequestResponseCycle):
    _head = b""  # status line + headers, serialized by uvicorn and not yet written

    async def send(self, message) -> None:
        transport = self.transport
        if not self.response_started and message["type"] == "http.response.start":
            capture = _HeadCapture()
            capture.data = b""
            self.transport = capture
            try:
                await super().send(message)
            finally:
                self.transport = transport
            self._head = capture.data
            return
        head = self._head
        if not head:
            await super().send(message)
            return
        self._head = b""
        proxy = _HeadThenBody(transport, head)
        self.transport = proxy
        try:
            await super().send(message)
        finally:
            self.transport = transport
        # uvicorn writes nothing for a HEAD request or an empty final body: send the head alone.
        if proxy.head and not self.disconnected:
            transport.write(proxy.head)
