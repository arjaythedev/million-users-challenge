"""Process model. `python -m feed` is a master that forks one worker per CPU it may use. Each worker is a
single-threaded uvloop event loop with its own SO_REUSEPORT listeners on both ports (the kernel spreads new
connections over the workers) and its own Postgres connections, so workers share nothing.

A worker that dies is not restarted (the challenge's rules): its connections are gone and the kernel sends new
ones to the workers that are left. The master exits when no worker is left.
"""
import gc
import os
import signal
import socket
import sys
import time

# Imported before fork, so every worker shares these pages with the master.
import asyncio
import uvloop

from . import app


def cpus():
    """CPUs this process may use: the affinity mask (taskset, systemd AllowedCPUs=/CPUAffinity=), capped by a
    cgroup v2 CPU quota (systemd CPUQuota=, docker --cpus) anywhere up the cgroup tree."""
    n = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or 1)
    try:
        with open("/proc/self/cgroup") as f:
            path = next(line.split(":", 2)[2].strip() for line in f if line.startswith("0::"))
    except (OSError, StopIteration):
        return n
    while True:
        try:
            with open(f"/sys/fs/cgroup{path}/cpu.max") as f:
                quota, period = f.read().split()
            if quota != "max":
                n = min(n, max(1, -(-int(quota) // int(period))))
        except (OSError, ValueError):
            pass
        if path in ("/", ""):
            return n
        path = os.path.dirname(path)


def env_int(key, default):
    v = os.environ.get(key, "")
    return int(v) if v.strip() else default


class Config:
    def __init__(self):
        self.database_url = os.environ["DATABASE_URL"]
        if not os.environ.get("JWT_SECRET"):
            raise SystemExit("JWT_SECRET must be set")
        host = os.environ.get("HOST", "127.0.0.1")
        ports = [p for p in (os.environ.get("PORT", "3000"), os.environ.get("EXTRA_PORT", "")) if p.strip()]
        self.addrs = [socket.getaddrinfo(host, int(p), type=socket.SOCK_STREAM, flags=socket.AI_PASSIVE)[0]
                      for p in ports]
        self.workers = env_int("WORKERS", cpus())
        # Postgres connections in total (rule 5 allows 512), split evenly over the workers.
        self.db_per_worker = max(2, env_int("DB_CONNS", 384) // self.workers)
        if self.workers * self.db_per_worker > 512:
            raise SystemExit("more than 512 Postgres connections: lower DB_CONNS or WORKERS")
        self.gzip_level = env_int("GZIP_LEVEL", 0)   # ISA-L levels 0-3
        self.gzip_min = env_int("GZIP_MIN", 1024)    # smaller bodies go out uncompressed
        self.started = time.time()

    def make_sockets(self):
        out = []
        for family, type_, proto, _, sockaddr in self.addrs:
            s = socket.socket(family, type_, proto)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            s.bind(sockaddr)
            s.listen(65535)  # the kernel caps it at net.core.somaxconn
            s.setblocking(False)
            out.append(s)
        return out


def _die_with_parent():
    if sys.platform.startswith("linux"):
        import ctypes
        PR_SET_PDEATHSIG = 1
        ctypes.CDLL(None, use_errno=True).prctl(PR_SET_PDEATHSIG, signal.SIGTERM)


def gc_tick(loop, every):
    """Collect what is young and freeze it: long-lived objects (open connections) move to the permanent
    generation, so no collection ever walks the hundreds of thousands of them again. Objects that die later are
    still freed by reference counting."""
    gc.collect()
    gc.freeze()
    loop.call_later(every, gc_tick, loop, every)


def worker(index, cfg):
    _die_with_parent()
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    loop = uvloop.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(app.start(cfg, index))
    gc.disable()
    loop.call_soon(gc_tick, loop, 1.0)
    loop.run_forever()


def main():
    cfg = Config()
    print(f"feed: {cfg.workers} workers, {cfg.db_per_worker} Postgres connections each, listening on "
          f"{[a[4] for a in cfg.addrs]}, gzip level {cfg.gzip_level} for bodies >= {cfg.gzip_min} B, "
          f"python {sys.version.split()[0]}", file=sys.stderr, flush=True)
    gc.collect()
    gc.freeze()  # before fork: keeps the shared pages shared
    children = {}
    for i in range(cfg.workers):
        pid = os.fork()
        if pid == 0:
            code = 0
            try:
                worker(i, cfg)
            except BaseException:
                import traceback
                traceback.print_exc()
                code = 1
            finally:
                os._exit(code)
        children[pid] = i

    def stop(signum, frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        while children:
            pid, status = os.wait()
            if pid in children:
                i = children.pop(pid)
                print(f"feed: worker {i} (pid {pid}) exited with status {status}; not restarted, "
                      f"{len(children)} left", file=sys.stderr, flush=True)
        raise SystemExit(1)
    finally:
        for pid in children:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
