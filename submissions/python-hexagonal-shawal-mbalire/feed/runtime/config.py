"""Configuration: the runtime contract's environment variables, the adapter choice for every port,
and the capacity model's overrides. Read once, in the master, before any worker exists."""
import math
import os
from dataclasses import dataclass

from ..domain import capacity


def available_cores(proc_cgroup: str = "/proc/self/cgroup", cgroup_root: str = "/sys/fs/cgroup") -> int:
    """Cores this process may actually use: the CPU affinity mask, further capped by a cgroup v2
    CPU quota if one is set. `os.cpu_count()` reports every CPU in the machine, which is wrong for
    the 8- and 4-core runs: twice the loops on half the cores means time-slicing, not throughput."""
    cores = len(os.sched_getaffinity(0))
    quota = _cgroup_cpu_quota(proc_cgroup, cgroup_root)
    if quota is not None:
        cores = min(cores, max(1, math.ceil(quota)))
    return cores


def _cgroup_cpu_quota(proc_cgroup: str, cgroup_root: str) -> float | None:
    """The tightest cgroup v2 CPU quota (in cores) on this process's own cgroup or any ancestor, or
    None. The quota lives in the process's cgroup directory (named in /proc/self/cgroup's `0::`
    line), not at the root, and a parent's limit (a systemd slice's CPUQuota) binds as well."""
    try:
        with open(proc_cgroup) as f:
            path = next((line[3:].strip() for line in f if line.startswith("0::")), None)
    except OSError:
        return None
    if path is None:
        return None
    tightest = None
    parts = [p for p in path.split("/") if p]
    for depth in range(len(parts), -1, -1):
        try:
            with open(os.path.join(cgroup_root, *parts[:depth], "cpu.max")) as f:
                limit, period = f.read().split()
            if limit != "max":
                q = int(limit) / int(period)
                tightest = q if tightest is None else min(tightest, q)
        except (OSError, ValueError):
            pass
    return tightest


@dataclass(frozen=True, slots=True)
class Config:
    database_url: str
    jwt_secret: str
    host: str
    ports: tuple[int, ...]
    server: str         # FEED_SERVER: raw | uvicorn | granian
    app: str            # FEED_APP:    raw | bare | fastapi | rsgi   (default: the server's own best)
    db: str             # FEED_DB:     libpq | asyncpg | pgjson
    codec: str          # FEED_JSON:   cjson | handwritten | orjson | pydantic | pgjson
    profile: frozenset[str]  # FEED_PROFILE: latency, cprofile (comma-separated), default none
    profile_dir: str
    gc: str             # FEED_GC:     survivors | generational
    gzip: str           # FEED_GZIP:   igzip | libdeflate | zlib  ("": the composition root picks)
    allow_noncompliant: bool
    workers: int
    read_lanes: int
    write_lanes: int

    @classmethod
    def from_env(cls, env=os.environ) -> "Config":
        ports = [int(env.get("PORT", "3000"))]
        if env.get("EXTRA_PORT"):
            ports.append(int(env["EXTRA_PORT"]))
        workers = int(env.get("FEED_WORKERS") or capacity.worker_count(available_cores()))
        write_lanes = int(env.get("FEED_WRITE_LANES") or capacity.write_lanes_per_worker(workers))
        read_lanes = int(env.get("FEED_READ_LANES")
                         or capacity.read_lanes_per_worker(workers, write_lanes=write_lanes))
        total = workers * (read_lanes + write_lanes)
        if total > capacity.POSTGRES_CONNECTION_CAP:
            raise ValueError(f"{total} Postgres connections exceed the cap of {capacity.POSTGRES_CONNECTION_CAP}")
        server = env.get("FEED_SERVER", "raw")
        db = env.get("FEED_DB", "libpq")
        return cls(
            database_url=env["DATABASE_URL"],
            jwt_secret=env["JWT_SECRET"],
            host=env.get("HOST", "0.0.0.0"),
            ports=tuple(ports),
            server=server,
            app=env.get("FEED_APP", ""),
            db=db,
            codec=env.get("FEED_JSON", "pgjson" if db == "pgjson" else "cjson"),
            profile=frozenset(m for m in env.get("FEED_PROFILE", "").split(",") if m),
            profile_dir=env.get("FEED_PROFILE_DIR", "profiles"),
            gc=env.get("FEED_GC", "survivors"),
            gzip=env.get("FEED_GZIP", ""),
            allow_noncompliant=env.get("FEED_ALLOW_NONCOMPLIANT") == "1",
            workers=workers,
            read_lanes=read_lanes,
            write_lanes=write_lanes,
        )
