// Bound running queries to the configured pool size and HTTP waiting by count and age.
// A timed-out waiter never starts SQL.
export function admission(
  capacity: number,
  maximumWaiting = capacity * 16,
  waitMs = 250,
) {
  let active = 0;
  const pending = new Set<() => boolean>();
  return {
    acquire(signal: AbortSignal): boolean | Promise<boolean> {
      if (signal.aborted) return false;
      if (active < capacity) {
        active++;
        return true;
      }
      if (pending.size >= maximumWaiting) return false;
      return new Promise((resolve) => {
        const deadline = performance.now() + waitMs;
        const cleanup = () => {
          clearTimeout(timer);
          signal.removeEventListener("abort", expire);
        };
        const grant = () => {
          const valid = !signal.aborted && performance.now() < deadline;
          cleanup();
          resolve(valid);
          return valid;
        };
        const expire = () => {
          pending.delete(grant);
          cleanup();
          resolve(false);
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
