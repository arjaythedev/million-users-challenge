import { expect, test } from "bun:test";
import { admission } from "../src/admission";

test("queued work is FIFO and bounded", async () => {
  const work = admission(1, 2, 1000);
  const signal = new AbortController().signal;
  expect(work.acquire(signal)).toBe("admitted");
  const first = work.acquire(signal);
  const second = work.acquire(signal);
  expect(work.acquire(signal)).toBe("full");
  work.release();
  expect(await first).toBe("admitted");
  work.release();
  expect(await second).toBe("admitted");
  work.release();
  expect(work.acquire(signal)).toBe("admitted");
  work.release();
});

test("expired and aborted waiters never receive an execution slot", async () => {
  const work = admission(1, 2, 10);
  const signal = new AbortController().signal;
  expect(work.acquire(signal)).toBe("admitted");
  const expired = work.acquire(signal);
  expect(await expired).toBe("expired");
  const aborted = new AbortController();
  const waiting = work.acquire(aborted.signal);
  aborted.abort();
  expect(await waiting).toBe("aborted");
  expect(work.acquire(aborted.signal)).toBe("aborted");
  work.release();
  expect(work.acquire(signal)).toBe("admitted");
  work.release();
});

test("a delayed timer cannot grant work past its deadline", async () => {
  const work = admission(1, 1, 5);
  const signal = new AbortController().signal;
  expect(work.acquire(signal)).toBe("admitted");
  const waiting = work.acquire(signal);
  const until = performance.now() + 20;
  while (performance.now() < until) {
    /* Deliberately prevent the timeout callback from running. */
  }
  work.release();
  expect(await waiting).toBe("expired");
  expect(work.acquire(signal)).toBe("admitted");
  work.release();
});

test("a small SQL pool retains the bounded HTTP burst budget", async () => {
  const work = admission(1);
  expect(work.acquire(new AbortController().signal)).toBe("admitted");
  const queued = Array.from({ length: 32 }, () =>
    work.acquire(new AbortController().signal),
  );
  for (const waiting of queued) {
    work.release();
    expect(await waiting).toBe("admitted");
  }
  work.release();
});
