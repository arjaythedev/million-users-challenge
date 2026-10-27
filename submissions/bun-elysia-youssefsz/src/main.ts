import { configuration } from "./config";
import { logError } from "./errors";

const workerMode = process.argv[2] === "--worker";
const args = process.argv.slice(workerMode ? 3 : 2);
const config = configuration(args);

if (workerMode || config.workers === 1) {
  const { database } = await import("./database");
  const { application } = await import("./app");
  const db = database(config.databaseUrl, config.poolPerWorker);
  const fetch = application(db, config.jwtSecret, config.gzipLevel);
  const servers = config.ports.map((port) =>
    Bun.serve({
      hostname: config.host,
      port,
      reusePort: config.workers > 1,
      idleTimeout: 120,
      maxRequestBodySize: 16 * 1024,
      development: false,
      fetch,
    }),
  );
  process.stderr.write(
    `${JSON.stringify({ event: "listening", pid: process.pid, ports: config.ports, pool: config.poolPerWorker })}\n`,
  );
  let stopping = false;
  const stop = async () => {
    if (stopping) return;
    stopping = true;
    // Cover bounded body reading, queue waiting, statement execution and connection cleanup.
    const deadline = setTimeout(() => process.exit(1), 20_000);
    deadline.unref();
    await Promise.all(servers.map((server) => server.stop()));
    await db.client.close({ timeout: 5 });
    clearTimeout(deadline);
  };
  process.on(
    "SIGTERM",
    () =>
      void stop().catch((error) => {
        logError("shutdown_failed", error);
        process.exit(1);
      }),
  );
  process.on(
    "SIGINT",
    () =>
      void stop().catch((error) => {
        logError("shutdown_failed", error);
        process.exit(1);
      }),
  );
} else {
  const children = Array.from({ length: config.workers }, () =>
    Bun.spawn({
      cmd: [process.execPath, import.meta.path, "--worker", ...args],
      stdin: "ignore",
      stdout: "inherit",
      stderr: "inherit",
    }),
  );
  let stopping = false;
  function stop() {
    if (stopping) return;
    stopping = true;
    for (const child of children) child.kill("SIGTERM");
  }
  process.on("SIGTERM", stop);
  process.on("SIGINT", stop);
  const codes = await Promise.all(
    children.map(async (child) => {
      const code = await child.exited;
      if (!stopping) {
        process.stderr.write(
          `${JSON.stringify({ event: "worker_exited", pid: child.pid, code })}\n`,
        );
        stop();
        return 1;
      }
      return code;
    }),
  );
  process.exit(codes.some((code) => code !== 0) ? 1 : 0);
}
