"""Profiling, as decorators around ports. Off by default; when off it costs nothing (nothing is wrapped).

FEED_PROFILE=latency
    Every port (the use cases, the repository, the codec, the authenticator) is wrapped in a
    timing proxy: the GoF Decorator pattern, applied at the hexagon's seams. Each call's duration
    goes into a log2 histogram (64 buckets, one increment per call, no allocation), labelled with
    the operation's declared semantics (`feed.domain.semantics`), so read and write tails are
    reported separately instead of being averaged together. The use-case timings include the
    port timings nested inside them; the difference is the core's own work.

FEED_PROFILE=cprofile
    Deterministic function-level profiling of the whole worker (cProfile). Heavy: use it to find
    *where* time goes, never to measure *how much* (it inflates every Python call).

Both write to FEED_PROFILE_DIR (default ./profiles), one file per worker, every 10 s, and the
latency report also goes to stderr on SIGUSR1. Combine them as FEED_PROFILE=latency,cprofile.
For sampling with no instrumentation at all, attach py-spy from outside: `py-spy top --pid <worker>`.

The proxies keep the wrapped methods' declarations (`@route`, `@query`, `@command`) via
functools.wraps, so a profiled service still routes and still picks lanes exactly as before.
"""
import asyncio
import cProfile
import functools
import inspect
import os
import signal
import sys
import time

from ..domain.semantics import is_declared, semantics_of

BUCKETS = 64  # bucket b holds durations in [2^(b-1), 2^b) ns


class LatencyRecorder:
    __slots__ = ("histograms",)

    def __init__(self):
        self.histograms: dict[str, list[int]] = {}

    def histogram(self, label: str) -> list[int]:
        return self.histograms.setdefault(label, [0] * BUCKETS)

    def report(self) -> str:
        lines = [f"{'operation':<44} {'calls':>9} {'p50':>9} {'p95':>9} {'p99':>9} {'max':>9}   (µs, bucket upper bounds)"]
        for label, h in sorted(self.histograms.items()):
            n = sum(h)
            if not n:
                continue

            def q(fraction: float) -> float:
                rank, seen = fraction * n, 0
                for b, count in enumerate(h):
                    seen += count
                    if seen >= rank:
                        return (1 << b) / 1000
                return float("inf")

            top = max(b for b, c in enumerate(h) if c)
            lines.append(f"{label:<44} {n:>9} {q(.5):>9.1f} {q(.95):>9.1f} {q(.99):>9.1f} {(1 << top) / 1000:>9.1f}")
        return "\n".join(lines)


def _wrap(fn, label: str, recorder: LatencyRecorder):
    h = recorder.histogram(label)
    clock = time.perf_counter_ns
    if inspect.iscoroutinefunction(fn):
        async def timed(*args, **kwargs):
            t = clock()
            try:
                return await fn(*args, **kwargs)
            finally:
                h[(clock() - t).bit_length()] += 1
    else:
        def timed(*args, **kwargs):
            t = clock()
            try:
                return fn(*args, **kwargs)
            finally:
                h[(clock() - t).bit_length()] += 1
    return functools.wraps(fn)(timed)


class _Proxy:
    """Holds the wrapped methods as instance attributes; everything else is the target's."""

    def __init__(self, target):
        self.__target = target

    def __getattr__(self, name):
        return getattr(self.__target, name)


def profiled(target, port: type, layer: str, recorder: LatencyRecorder):
    """Wrap every public method `port` declares, as implemented by `target`."""
    proxy = _Proxy(target)
    for name, declared in inspect.getmembers(port, callable):
        if name.startswith("_"):
            continue
        label = f"{layer}.{name}"
        if is_declared(declared):  # pure helpers (the codec) carry no semantics: no label
            s = semantics_of(declared)
            label += f" [{s.kind}{', idempotent' if s.kind == 'command' and s.idempotent else ''}]"
        setattr(proxy, name, _wrap(getattr(target, name), label, recorder))
    return proxy


class Profiler:
    """Per-worker profiling session: decorates ports, and flushes reports to disk periodically."""

    def __init__(self, modes: set[str], directory: str, slot: int):
        self.modes = modes
        self.slot = slot
        self.directory = directory
        self.latency = LatencyRecorder() if "latency" in modes else None
        self.cprofile = cProfile.Profile() if "cprofile" in modes else None
        if modes:
            os.makedirs(directory, exist_ok=True)

    def decorate(self, target, port: type, layer: str):
        return profiled(target, port, layer, self.latency) if self.latency else target

    def start(self) -> None:
        if not self.modes:
            return
        loop = asyncio.get_running_loop()
        if self.latency:
            loop.add_signal_handler(signal.SIGUSR1, lambda: print(self._header() + self.latency.report(), file=sys.stderr, flush=True))
        if self.cprofile:
            self.cprofile.enable()
        self._schedule(loop)

    def _header(self) -> str:
        return f"--- worker {self.slot} (pid {os.getpid()}) ---\n"

    def _schedule(self, loop) -> None:
        loop.call_later(10, self._flush, loop)

    def _flush(self, loop) -> None:
        base = os.path.join(self.directory, f"worker-{self.slot}")
        if self.latency:
            with open(base + ".latency.txt", "w") as f:
                f.write(self._header() + self.latency.report() + "\n")
        if self.cprofile:
            self.cprofile.dump_stats(base + ".prof")  # disables the profiler as a side effect
            self.cprofile.enable()
        self._schedule(loop)
