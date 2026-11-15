"""The raw adapter steps use-case coroutines itself (no Task): replies, ordering, and failures."""
import asyncio
import unittest

from feed.adapters.inbound.raw import HttpProtocol
from feed.application.contract import Reply


class FakeTransport:
    def __init__(self):
        self.out = b""
        self.closed = False

    def write(self, data):
        self.out += data

    def close(self):
        self.closed = True


class Endpoint:
    wants_auth = False

    def __init__(self, invoke):
        self.invoke = invoke


def protocol_for(routes):
    """routes: path -> async fn(raw_id) -> Reply."""
    def match(method, path):
        fn = routes.get(path)
        return None if fn is None else (Endpoint(lambda raw_id, auth, body: fn(raw_id)), None)
    proto = HttpProtocol(match)
    transport = FakeTransport()
    proto.connection_made(transport)
    return proto, transport


def get(path):
    return f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode()


def bodies(raw: bytes) -> list[bytes]:
    return [chunk.split(b"\r\n\r\n", 1)[1] for chunk in raw.split(b"HTTP/1.1 ")[1:]]


class TasklessDriverTest(unittest.TestCase):
    def run_loop(self, scenario):
        asyncio.run(scenario())

    def test_reply_after_the_awaited_future_resolves(self):
        async def scenario():
            loop = asyncio.get_running_loop()
            pending = loop.create_future()

            async def slow(_):
                return Reply(200, await pending)
            proto, transport = protocol_for({"/a": slow})
            proto.data_received(get("/a"))
            self.assertEqual(transport.out, b"")
            pending.set_result(b'{"ok":1}')
            await asyncio.sleep(0)
            self.assertEqual(bodies(transport.out), [b'{"ok":1}'])
        self.run_loop(scenario)

    def test_pipelined_requests_answered_in_order(self):
        async def scenario():
            loop = asyncio.get_running_loop()
            first = loop.create_future()

            async def slow(_):
                return Reply(200, await first)

            async def fast(_):
                await asyncio.sleep(0)  # a bare yield: the driver must go round the loop
                return Reply(200, b"2")
            proto, transport = protocol_for({"/slow": slow, "/fast": fast})
            proto.data_received(get("/slow") + get("/fast"))
            await asyncio.sleep(0)
            self.assertEqual(transport.out, b"")  # /fast waits behind /slow
            first.set_result(b"1")
            for _ in range(3):
                await asyncio.sleep(0)
            self.assertEqual(bodies(transport.out), [b"1", b"2"])
        self.run_loop(scenario)

    def test_failures_become_500_and_the_connection_carries_on(self):
        async def scenario():
            loop = asyncio.get_running_loop()
            cancelled = loop.create_future()

            async def boom(_):
                raise RuntimeError("adapter bug")

            async def waits(_):
                return Reply(200, await cancelled)

            async def ok(_):
                return Reply(200, b"fine")
            proto, transport = protocol_for({"/boom": boom, "/cancel": waits, "/ok": ok})
            proto.data_received(get("/boom") + get("/cancel") + get("/ok"))
            cancelled.cancel()
            for _ in range(3):
                await asyncio.sleep(0)
            statuses = [chunk[:3] for chunk in transport.out.split(b"HTTP/1.1 ")[1:]]
            self.assertEqual(statuses, [b"500", b"500", b"200"])
            self.assertFalse(transport.closed)
        self.run_loop(scenario)


if __name__ == "__main__":
    unittest.main()
