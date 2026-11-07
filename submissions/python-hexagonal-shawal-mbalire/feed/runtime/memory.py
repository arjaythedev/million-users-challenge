"""Garbage-collection policies: how the cyclic collector treats what a worker keeps alive.

Refcounting frees almost everything this server allocates; the cyclic collector exists only for
the rare reference cycle. Its cost, though, grows with what *survives*, because every survivor of
a young collection moves to generation 1 and is traversed again by each gen-1 collection. Here the
survivors are mostly the connections themselves: each keep-alive connection leaves five tracked
objects (the protocol, uvloop's transport, a bound method, a list and a weak reference).

Measured locally with one worker and 100,000 idle-ish keep-alive connections (`.handover/many.sh`):
generation 1 held 499,006 objects and one gen-1 collection stopped the worker for 177 ms. That is
linear in connections, so ~440 ms at 250,000 connections per worker (a million users on 4 cores)
and ~1.3 s at 750,000 (three million). Over the same run the collector found only 158 objects of
cyclic garbage in ~400,000 requests: the scans buy almost nothing.

    generational  Freeze the startup heap (`gc.freeze()`), raise the gen-0 threshold, and leave the
                  generations alone. Pauses grow with the number of open connections.
    survivors     The same, and freeze again after every collection. Each young collection still
                  frees the cycles that died young, then its survivors move to the permanent
                  generation instead of piling up in generation 1, so a pause is bounded by the
                  young generation, not the connection count. The cost is that a cycle which dies
                  *after* surviving one young collection is never collected; at the measured rate
                  that is a few KB over a five-minute run. Objects that die by refcount (nearly
                  all) are freed normally, frozen or not.
"""
import gc

YOUNG_THRESHOLD = 50_000  # tracked allocations between young collections


def generational() -> None:
    gc.freeze()
    gc.set_threshold(YOUNG_THRESHOLD, 50, 100)


def survivors() -> None:
    generational()

    def freeze_survivors(phase: str, info: dict) -> None:
        if phase == "stop":
            gc.freeze()

    gc.callbacks.append(freeze_survivors)
