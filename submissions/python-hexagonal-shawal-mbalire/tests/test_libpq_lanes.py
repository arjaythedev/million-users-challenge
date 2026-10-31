"""Lane failure and recovery, against a fake connection: no Postgres needed."""
import asyncio
import socket
import unittest

from feed.adapters.outbound.postgres import libpq
from feed.adapters.outbound.postgres._pq import PqError


class FakeConn:
    """Answers every query with its params once the test pokes the socket; `kill()` breaks it."""

    def __init__(self):
        self.ours, self.peer = socket.socketpair()
        self.sent: list[tuple] = []
        self.dead = False
        self.closed = False

    def fileno(self):
        return self.ours.fileno()

    def send(self, name, params):
        if self.dead:
            raise PqError("08006", "send on a dead connection")
        self.sent.append(params)

    def flush(self):
        return 0

    def read(self):
        self.ours.recv(64)
        if self.dead:
            raise PqError("08006", "server closed the connection")
        replies, self.sent = self.sent, []
        return replies

    def poke(self):
        self.peer.send(b"x")

    def kill(self):
        self.dead = True
        self.poke()

    def close(self):
        if not self.closed:
            self.closed = True
            self.ours.close()
            self.peer.close()


class Opener:
    """Hands out fresh FakeConns; fails `refuse` times first, as a restarting server would."""

    def __init__(self, refuse=0):
        self.refuse = refuse
        self.conns: list[FakeConn] = []

    def __call__(self):
        if self.refuse:
            self.refuse -= 1
            raise PqError("08006", "connection refused")
        self.conns.append(FakeConn())
        return self.conns[-1]

    def close_all(self):
        for conn in self.conns:
            conn.close()


async def settle(predicate, timeout=2.0):
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        if asyncio.get_running_loop().time() > deadline:
            raise AssertionError("condition not reached")
        await asyncio.sleep(0.005)


class LaneRecoveryTest(unittest.TestCase):
    def setUp(self):
        self._saved = libpq.RECONNECT_FIRST_DELAY, libpq.RECONNECT_MAX_DELAY
        libpq.RECONNECT_FIRST_DELAY, libpq.RECONNECT_MAX_DELAY = 0.01, 0.02

    def tearDown(self):
        libpq.RECONNECT_FIRST_DELAY, libpq.RECONNECT_MAX_DELAY = self._saved

    def run_with(self, opener, scenario):
        async def main():
            try:
                await scenario()
            finally:
                opener.close_all()
        asyncio.run(main())

    def test_in_flight_requests_fail_then_lane_reconnects(self):
        opener = Opener()

        async def scenario():
            lane = libpq.Lane(asyncio.get_running_loop(), opener)
            first = opener.conns[0]
            in_flight = lane.submit("post", (1,))
            opener.refuse = 2  # the server is still restarting for the first attempts
            first.kill()
            with self.assertRaises(PqError):
                await in_flight
            self.assertFalse(lane.healthy)
            self.assertTrue(first.closed)
            with self.assertRaises(PqError):
                lane.submit("post", (2,))
            await settle(lambda: lane.healthy)
            self.assertEqual(len(opener.conns), 2)
            reply = lane.submit("post", (3,))
            opener.conns[1].poke()
            self.assertEqual(await reply, (3,))
        self.run_with(opener, scenario)

    def test_send_on_dead_connection_breaks_the_lane(self):
        opener = Opener()

        async def scenario():
            lane = libpq.Lane(asyncio.get_running_loop(), opener)
            opener.conns[0].dead = True
            with self.assertRaises(PqError):
                lane.submit("post", (1,))
            self.assertFalse(lane.healthy)
            await settle(lambda: lane.healthy)
        self.run_with(opener, scenario)

    def test_repository_routes_around_a_broken_lane(self):
        opener = Opener()

        async def scenario():
            loop = asyncio.get_running_loop()
            reads = [libpq.Lane(loop, opener) for _ in range(3)]
            writes = [libpq.Lane(loop, opener)]
            repo = libpq.LibpqPostRepository(reads, writes)
            opener.refuse = 1_000  # keep the broken lane down for the whole test
            dead = reads[1]
            dead_conn = opener.conns[1]
            dead_conn.kill()
            await settle(lambda: not dead.healthy)
            picked = {id(repo._lane["newest"]()) for _ in range(30)}
            self.assertEqual(picked, {id(reads[0]), id(reads[2])})
            self.assertIs(repo._lane["create"](), writes[0])

            # Every read lane down: requests fail fast instead of waiting for a reconnect.
            for conn in (opener.conns[0], opener.conns[2]):
                conn.kill()
            await settle(lambda: not any(lane.healthy for lane in reads))
            with self.assertRaises(PqError):
                await repo.newest(10)

            opener.refuse = 0
            await settle(lambda: all(lane.healthy for lane in reads))
            picked = {id(repo._lane["by_id"]()) for _ in range(30)}
            self.assertEqual(picked, {id(lane) for lane in reads})
        self.run_with(opener, scenario)


if __name__ == "__main__":
    unittest.main()
