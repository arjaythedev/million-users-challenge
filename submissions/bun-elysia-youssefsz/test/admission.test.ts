import { expect, test } from "bun:test";
import { admission } from "../src/admission";

test("queued work is FIFO and bounded", async () => {
  const work = admission(1, 2, 1000);
  const signal = new AbortController().signal;
  expect(work.acquire(signal)).toBe(true);
  const first = work.acquire(signal);
  const second = work.acquire(signal);
  expect(work.acquire(signal)).toBe(false);
  work.release();
  expect(await first).toBe(true);
  work.release();
  expect(await second).toBe(true);
  work.release();
  expect(work.acquire(signal)).toBe(true);
  work.release();
});

test("expired and aborted waiters never receive an execution slot", async () => {
  const work = admission(1, 2, 10);
  const signal = new AbortController().signal;
  expect(work.acquire(signal)).toBe(true);
  const expired = work.acquire(signal);
  expect(await expired).toBe(false);
  const aborted = new AbortController();
  const waiting = work.acquire(aborted.signal);
  aborted.abort();
  expect(await waiting).toBe(false);
  expect(work.acquire(aborted.signal)).toBe(false);
  work.release();
  expect(work.acquire(signal)).toBe(true);
  work.release();
});

test("a delayed timer cannot grant work past its deadline", async () => {
  const work = admission(1, 1, 5);
  const signal = new AbortController().signal;
  expect(work.acquire(signal)).toBe(true);
  const waiting = work.acquire(signal);
  const until = performance.now() + 20;
  while (performance.now() < until) {
    /* Deliberately prevent the timeout callback from running. */
  }
  work.release();
  expect(await waiting).toBe(false);
  expect(work.acquire(signal)).toBe(true);
  work.release();
});
