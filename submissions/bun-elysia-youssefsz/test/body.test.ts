import { expect, test } from "bun:test";
import { jsonBody } from "../src/body";

test("body reader cancels slow streams instead of retaining them", async () => {
  let cancelled = false;
  const stream = new ReadableStream({
    cancel() {
      cancelled = true;
    },
  });
  const request = new Request("http://test/posts", {
    method: "POST",
    body: stream,
  });
  await expect(jsonBody(request, 10)).rejects.toThrow("malformed JSON body");
  expect(cancelled).toBe(true);
});

test("body reader handles split UTF-8 characters and limits bytes", async () => {
  const bytes = new TextEncoder().encode('{"body":"😀"}');
  const stream = new ReadableStream({
    start(controller) {
      for (const byte of bytes) controller.enqueue(new Uint8Array([byte]));
      controller.close();
    },
  });
  expect(
    await jsonBody(
      new Request("http://test/posts", { method: "POST", body: stream }),
    ),
  ).toEqual({ body: "😀" });
  await expect(
    jsonBody(
      new Request("http://test/posts", {
        method: "POST",
        body: "x".repeat(16385),
      }),
    ),
  ).rejects.toThrow("malformed JSON body");
});
