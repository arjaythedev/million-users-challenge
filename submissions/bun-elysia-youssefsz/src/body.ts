import { HttpError } from "./errors";

export async function jsonBody(
  request: Request,
  timeoutMs = 5000,
): Promise<unknown> {
  const reader = request.body?.getReader();
  if (!reader) throw new HttpError(400, "malformed JSON body");
  const deadline = performance.now() + timeoutMs;
  const buffer = new Uint8Array(16 * 1024);
  let length = 0;
  let expired = false;
  const timer = setTimeout(() => {
    expired = true;
    void reader.cancel().catch(() => {});
  }, timeoutMs);
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (expired || performance.now() >= deadline || request.signal.aborted)
        throw new Error("Body deadline exceeded");
      if (done) break;
      if (length + value.length > buffer.length)
        throw new Error("Body limit exceeded");
      buffer.set(value, length);
      length += value.length;
    }
    return JSON.parse(
      new TextDecoder("utf-8", { fatal: true }).decode(
        buffer.subarray(0, length),
      ),
    );
  } catch {
    await reader.cancel().catch(() => {});
    throw new HttpError(400, "malformed JSON body");
  } finally {
    clearTimeout(timer);
    reader.releaseLock();
  }
}
