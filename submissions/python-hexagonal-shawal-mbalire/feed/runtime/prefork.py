"""Process topology: a supervising master and one forked worker per core.

The master never runs an event loop and never touches the network or the database; it forks the
workers and keeps that number alive. Each worker builds everything it uses (event loop, database
lanes, listening sockets) *after* the fork, so nothing with a file descriptor or a thread is ever
shared across a fork.

Every worker binds its own listening socket on each port with SO_REUSEPORT, and the kernel spreads
incoming connections across those sockets by hashing the 4-tuple. That is load balancing with no
balancer process and no shared accept queue (see `feed.domain.capacity`, section 2).
"""
import os
import signal
import socket
import sys
import time
import traceback
from collections.abc import Callable


def listening_socket(host: str, port: int, backlog: int) -> socket.socket:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind((host, port))
    sock.listen(backlog)
    sock.setblocking(False)
    return sock


def _child(worker_main: Callable[[int], None], slot: int) -> None:
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.signal(signal.SIGUSR1, signal.SIG_IGN)  # the profiler installs a handler if it is on
    code = 0
    try:
        worker_main(slot)
    except BaseException:
        traceback.print_exc()
        code = 1
    finally:
        sys.stderr.flush()
        os._exit(code)


def supervise(workers: int, worker_main: Callable[[int], None]) -> None:
    """Fork `workers` children and replace any that die, until SIGTERM/SIGINT."""
    children: dict[int, int] = {}  # pid -> slot
    stopping = False

    def spawn(slot: int) -> None:
        pid = os.fork()
        if pid == 0:
            _child(worker_main, slot)
        children[pid] = slot

    def stop(signum, frame) -> None:
        nonlocal stopping
        stopping = True
        for pid in children:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def forward(signum, frame) -> None:
        for pid in children:
            try:
                os.kill(pid, signum)
            except ProcessLookupError:
                pass

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGUSR1, forward)  # `kill -USR1 <master>`: every worker prints its report
    for slot in range(workers):
        spawn(slot)

    while children:
        try:
            pid, status = os.wait()
        except ChildProcessError:
            break
        slot = children.pop(pid, None)
        if slot is None or stopping:
            continue
        print(f"worker {slot} (pid {pid}) exited with status {status}; restarting", file=sys.stderr, flush=True)
        time.sleep(0.5)  # a worker that cannot start (database down) must not become a fork loop
        if not stopping:  # a SIGTERM during the pause must not leave an orphan nobody signals
            spawn(slot)
