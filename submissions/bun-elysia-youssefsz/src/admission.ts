// Share one request budget across both listeners and the database queue. Idle sockets do not count.
export const MAX_IN_FLIGHT_REQUESTS = 2048;
// Leave room for execution and serialization within the service's one-second tail-latency target.
export const DATABASE_QUEUE_WAIT_MS = 500;
export type AdmissionResult = "admitted" | "full" | "expired" | "aborted";

// Pool size limits SQL concurrency; the request budget limits queued HTTP work independently.
// A timed-out waiter never starts SQL.
export function admission(
  capacity: number,
  maximumWaiting = MAX_IN_FLIGHT_REQUESTS - capacity,
  waitMs = DATABASE_QUEUE_WAIT_MS,
) {
  let active = 0;
  const pending = new Set<() => boolean>();
  return {
    acquire(signal: AbortSignal): AdmissionResult | Promise<AdmissionResult> {
      if (signal.aborted) return "aborted";
      if (active < capacity) {
        active++;
        return "admitted";
      }
      if (pending.size >= maximumWaiting) return "full";
      return new Promise<AdmissionResult>((resolve) => {
        const deadline = performance.now() + waitMs;
        const cleanup = () => {
          clearTimeout(timer);
          signal.removeEventListener("abort", expire);
        };
        const grant = () => {
          const valid = !signal.aborted && performance.now() < deadline;
          cleanup();
          resolve(valid ? "admitted" : signal.aborted ? "aborted" : "expired");
          return valid;
        };
        const expire = () => {
          pending.delete(grant);
          cleanup();
          resolve(signal.aborted ? "aborted" : "expired");
        };
        const timer = setTimeout(expire, waitMs);
        pending.add(grant);
        signal.addEventListener("abort", expire, { once: true });
      });
    },
    release() {
      for (const next of pending) {
        pending.delete(next);
        if (next()) return;
      }
      active--;
    },
  };
}
