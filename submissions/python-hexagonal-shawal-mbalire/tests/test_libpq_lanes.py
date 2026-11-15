"""Lanes: batching, group commit, failure and recovery, against a fake connection. No Postgres needed."""
import asyncio
import socket
import unittest

from feed.adapters.outbound.postgres import libpq
from feed.adapters.outbound.postgres._pq import ABORTED, SYNC, PqError

FK_VIOLATION = PqError("23503", "no such post")


class FakeConn:
    """A pipeline as Postgres runs it. Statements answer with their params when the test pokes the
    socket; a params value in `failing` fails, which skips the rest of its batch and rolls it back.
    `committed` is what a real database would hold. `kill()` breaks the connection."""

    def __init__(self):
        self.ours, self.peer = socket.socketpair()
        self.unsynced: list[tuple] = []
        self.batches: list[list[tuple]] = []  # every batch ever sent, for the tests to inspect
        self.unread: list[list[tuple]] = []
        self.failing: set = set()
        self.committed: list[tuple] = []
        self.hold_sync = False
        self.held_syncs = 0
        self.dead = False
        self.closed = False

    def fileno(self):
        return self.ours.fileno()

    def send(self, name, params):
        if self.dead:
            raise PqError("08006", "send on a dead connection")
        self.unsynced.append(params)

    def sync(self):
        self.batches.append(self.unsynced)
        self.unread.append(self.unsynced)
        self.unsynced = []

    def flush(self):
        return 0

    def read(self):
        self.ours.recv(64)
        if self.dead:
            raise PqError("08006", "server closed the connection")
        replies = [SYNC] * self.held_syncs
        self.held_syncs = 0
        for batch in self.unread:
            failed = False
            for params in batch:
                if failed:
                    replies.append(ABORTED)
                elif params in self.failing:
                    replies.append(FK_VIOLATION)
                    failed = True
                else:
                    replies.append(params)
            if not failed:
                self.committed.extend(batch)
            if self.hold_sync:
                self.held_syncs += 1
            else:
                replies.append(SYNC)
        self.unread = []
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


async def one_pass():
    """Lets everything scheduled with call_soon so far run."""
    await asyncio.sleep(0)
    await asyncio.sleep(0)


class LaneTestCase(unittest.TestCase):
    def setUp(self):
        self._saved = libpq.RECONNECT_FIRST_DELAY, libpq.RECONNECT_MAX_DELAY
        libpq.RECONNECT_FIRST_DELAY, libpq.RECONNECT_MAX_DELAY = 0.01, 0.02

    def tearDown(self):
        libpq.RECONNECT_FIRST_DELAY, libpq.RECONNECT_MAX_DELAY = self._saved

    def run_with(self, opener, scenario):
        async def main():
            try:
                await asyncio.wait_for(scenario(), 5)
            finally:
                opener.close_all()
        asyncio.run(main())


class LaneRecoveryTest(LaneTestCase):
    def test_in_flight_requests_fail_then_lane_reconnects(self):
        opener = Opener()

        async def scenario():
            lane = libpq.ReadLane(asyncio.get_running_loop(), opener)
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
            await one_pass()
            opener.conns[1].poke()
            self.assertEqual(await reply, (3,))
        self.run_with(opener, scenario)

    def test_send_on_dead_connection_breaks_the_lane(self):
        opener = Opener()

        async def scenario():
            lane = libpq.ReadLane(asyncio.get_running_loop(), opener)
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
            reads = [libpq.ReadLane(loop, opener) for _ in range(3)]
            writes = [libpq.WriteLane(loop, opener)]
            repo = libpq.LibpqPostRepository(reads, writes)
            opener.refuse = 1_000  # keep the broken lane down for the whole test
            dead = reads[1]
            opener.conns[1].kill()
            await settle(lambda: not dead.healthy)

            def picked():
                rotation = repo._lane["newest"]
                seen = set()
                for lane in reads:  # load each lane in turn: the rotation takes the least loaded
                    rotation.pass_ended()
                    seen.add(id(rotation()))
                    rotation()._waiting.append(None)
                for lane in reads:
                    lane._waiting.clear()
                return seen
            self.assertEqual(picked(), {id(reads[0]), id(reads[2])})
            self.assertIs(repo._lane["create"](), writes[0])

            # Every read lane down: requests fail fast instead of waiting for a reconnect.
            for conn in (opener.conns[0], opener.conns[2]):
                conn.kill()
            await settle(lambda: not any(lane.healthy for lane in reads))
            with self.assertRaises(PqError):
                await repo.newest()

            opener.refuse = 0
            await settle(lambda: all(lane.healthy for lane in reads))
            self.assertEqual(picked(), {id(lane) for lane in reads})
        self.run_with(opener, scenario)


class ReadBatchTest(LaneTestCase):
    def rotation(self, opener, lanes=2):
        loop = asyncio.get_running_loop()
        return libpq.Rotation([libpq.ReadLane(loop, opener) for _ in range(lanes)])

    def test_a_pass_reads_share_one_lane_and_one_sync(self):
        opener = Opener()

        async def scenario():
            rotation = self.rotation(opener)
            futures = [rotation().submit("post", (i,)) for i in range(5)]
            await one_pass()
            used = [conn for conn in opener.conns if conn.batches]
            self.assertEqual(len(used), 1)
            self.assertEqual(used[0].batches, [[(i,) for i in range(5)]])
            used[0].poke()
            self.assertEqual(await asyncio.gather(*futures), [(i,) for i in range(5)])
        self.run_with(opener, scenario)

    def test_a_full_batch_moves_to_the_next_lane(self):
        opener = Opener()

        async def scenario():
            rotation = self.rotation(opener)
            futures = [rotation().submit("post", (i,)) for i in range(libpq.MAX_BATCH + 1)]
            await one_pass()
            self.assertEqual([len(conn.batches[0]) for conn in opener.conns], [libpq.MAX_BATCH, 1])
            for conn in opener.conns:
                conn.poke()
            await asyncio.gather(*futures)
        self.run_with(opener, scenario)

    def test_the_next_pass_takes_the_least_loaded_lane(self):
        opener = Opener()

        async def scenario():
            rotation = self.rotation(opener)
            first = rotation().submit("post", (1,))
            await one_pass()
            second = rotation().submit("post", (2,))
            await one_pass()
            self.assertEqual([conn.batches for conn in opener.conns], [[[(1,)]], [[(2,)]]])
            for conn in opener.conns:
                conn.poke()
            await asyncio.gather(first, second)
        self.run_with(opener, scenario)

    def test_a_failed_read_fails_alone(self):
        opener = Opener()

        async def scenario():
            rotation = self.rotation(opener, lanes=1)
            conn = opener.conns[0]
            conn.failing = {(2,)}
            futures = [rotation().submit("post", (i,)) for i in (1, 2, 3, 4)]
            await one_pass()
            conn.poke()
            self.assertEqual(await futures[0], (1,))
            with self.assertRaises(PqError):
                await futures[1]
            await one_pass()
            self.assertEqual(conn.batches[1], [(3,), (4,)])  # those skipped go again
            conn.poke()
            self.assertEqual(await asyncio.gather(futures[2], futures[3]), [(3,), (4,)])
        self.run_with(opener, scenario)

    def test_a_resend_on_a_dead_connection_fails_the_request(self):
        opener = Opener()

        async def scenario():
            rotation = self.rotation(opener, lanes=1)
            conn = opener.conns[0]
            conn.failing = {(1,)}
            futures = [rotation().submit("post", (i,)) for i in (1, 2)]
            await one_pass()
            real_send = conn.send

            def dies(name, params):
                conn.send = real_send
                raise PqError("08006", "gone during the re-send")
            conn.send = dies
            conn.poke()
            results = await asyncio.gather(*futures, return_exceptions=True)
            self.assertEqual(results[0].args[0], "23503")
            self.assertEqual(results[1].args[0], "08006")
        self.run_with(opener, scenario)


class GroupCommitTest(LaneTestCase):
    def lane(self, opener):
        return libpq.WriteLane(asyncio.get_running_loop(), opener)

    def test_writes_wait_for_the_transaction_in_flight_then_share_the_next(self):
        opener = Opener()

        async def scenario():
            lane = self.lane(opener)
            conn = opener.conns[0]
            first = lane.submit("insert_like", (1,))
            await one_pass()
            others = [lane.submit("insert_like", (i,)) for i in (2, 3, 4)]
            await one_pass()
            self.assertEqual(conn.batches, [[(1,)]])  # still in flight: nothing else leaves
            conn.poke()
            self.assertEqual(await first, (1,))
            await settle(lambda: len(conn.batches) == 2)
            self.assertEqual(conn.batches[1], [(2,), (3,), (4,)])
            conn.poke()
            self.assertEqual(await asyncio.gather(*others), [(2,), (3,), (4,)])
        self.run_with(opener, scenario)

    def test_no_write_is_answered_before_its_sync(self):
        """Rule 8: until the Sync the transaction has not committed, whatever its results say."""
        opener = Opener()

        async def scenario():
            lane = self.lane(opener)
            conn = opener.conns[0]
            conn.hold_sync = True
            future = lane.submit("insert_like", (1,))
            await one_pass()
            conn.poke()
            await settle(lambda: not conn.unread)
            await asyncio.sleep(0.01)
            self.assertFalse(future.done())
            conn.poke()  # the Sync arrives
            self.assertEqual(await future, (1,))
        self.run_with(opener, scenario)

    def test_a_failed_batch_runs_again_one_write_per_transaction(self):
        opener = Opener()

        async def scenario():
            lane = self.lane(opener)
            conn = opener.conns[0]
            conn.failing = {(2,)}
            futures = [lane.submit("insert_like", (i,)) for i in (1, 2, 3)]
            await one_pass()
            conn.poke()
            await settle(lambda: len(conn.batches) == 4)
            self.assertEqual(conn.batches[1:], [[(1,)], [(2,)], [(3,)]])
            self.assertEqual(conn.committed, [])  # the first attempt rolled back
            self.assertFalse(any(f.done() for f in futures))
            conn.poke()
            results = await asyncio.gather(*futures, return_exceptions=True)
            self.assertEqual(results[0], (1,))
            self.assertEqual(results[1].args[0], "23503")
            self.assertEqual(results[2], (3,))
            self.assertEqual(conn.committed, [(1,), (3,)])  # each exactly once
        self.run_with(opener, scenario)

    def test_a_failed_commit_fails_the_whole_batch(self):
        opener = Opener()

        async def scenario():
            lane = self.lane(opener)
            conn = opener.conns[0]
            futures = [lane.submit("insert_like", (i,)) for i in (1, 2)]
            await one_pass()
            conn.read = lambda: (conn.ours.recv(64), [(1,), (2,), FK_VIOLATION, SYNC])[1]
            conn.poke()
            results = await asyncio.gather(*futures, return_exceptions=True)
            self.assertTrue(all(isinstance(r, PqError) for r in results))
        self.run_with(opener, scenario)

    def test_a_lost_connection_fails_everything_held_and_recovers(self):
        opener = Opener()

        async def scenario():
            lane = self.lane(opener)
            conn = opener.conns[0]
            in_flight = lane.submit("insert_like", (1,))
            await one_pass()
            queued = lane.submit("insert_like", (2,))
            conn.kill()
            results = await asyncio.gather(in_flight, queued, return_exceptions=True)
            self.assertTrue(all(isinstance(r, PqError) for r in results))
            self.assertEqual(lane.depth, 0)
            await settle(lambda: lane.healthy)
            again = lane.submit("insert_like", (3,))
            await one_pass()
            opener.conns[1].poke()
            self.assertEqual(await again, (3,))
        self.run_with(opener, scenario)


if __name__ == "__main__":
    unittest.main()
