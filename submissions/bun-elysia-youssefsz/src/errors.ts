export class HttpError extends Error {
  constructor(
    readonly status: 400 | 401 | 404 | 500,
    message: string,
  ) {
    super(message);
    this.name = "HttpError";
  }
}

let lastLogged = -Infinity;
let suppressed = 0;

export function logError(event: string, error: unknown): void {
  const now = performance.now();
  if (now - lastLogged < 1000) {
    suppressed++;
    return;
  }
  lastLogged = now;
  const cause =
    error instanceof Error && error.cause instanceof Error
      ? error.cause
      : error;
  const sqlState =
    cause instanceof Error &&
    "errno" in cause &&
    typeof cause.errno === "string" &&
    /^[0-9A-Z]{5}$/.test(cause.errno)
      ? cause.errno
      : undefined;
  // Avoid driver messages and query parameters, which can contain credentials or post bodies.
  process.stderr.write(
    `${JSON.stringify({ event, pid: process.pid, error: error instanceof Error ? error.name : "UnknownError", sqlState, suppressed })}\n`,
  );
  suppressed = 0;
}
