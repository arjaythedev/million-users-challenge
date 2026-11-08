import { availableParallelism } from "node:os";
import { readFileSync } from "node:fs";

function cpuBudget(): number {
  let count = availableParallelism();
  if (process.platform === "linux") {
    try {
      const [quota, period] = readFileSync("/sys/fs/cgroup/cpu.max", "utf8")
        .trim()
        .split(/\s+/);
      if (quota !== "max" && Number(quota) > 0 && Number(period) > 0) {
        count = Math.min(
          count,
          Math.max(1, Math.ceil(Number(quota) / Number(period))),
        );
      }
    } catch (error) {
      if (!(
        error instanceof Error &&
        "code" in error &&
        error.code === "ENOENT"
      ))
        throw error;
    }
  }
  return Math.min(count, 32);
}

function required(name: string): string {
  const value = process.env[name];
  if (!value) throw new Error(`${name} is required`);
  return value;
}

function integer(raw: string, name: string, max: number): number {
  const value = Number(raw);
  if (
    !/^\d+$/.test(raw) ||
    !Number.isSafeInteger(value) ||
    value < 1 ||
    value > max
  ) {
    throw new Error(`Invalid ${name}`);
  }
  return value;
}

export function configuration(args: string[] = []) {
  const options = new Map<string, string>();
  for (let i = 0; i < args.length; i += 2) {
    const key = args[i];
    const value = args[i + 1];
    if (
      !key ||
      !value ||
      !["--workers", "--pool", "--gzip-level"].includes(key)
    ) {
      throw new Error("Expected --workers N, --pool N, or --gzip-level N");
    }
    options.set(key, value);
  }
  const workers = integer(
    options.get("--workers") ?? String(cpuBudget()),
    "workers",
    32,
  );
  if (process.platform !== "linux" && workers > 1) {
    throw new Error(
      "Multiple workers require Linux SO_REUSEPORT. Use --workers 1 on macOS.",
    );
  }
  const totalPool = integer(
    options.get("--pool") ?? String(Math.min(256, workers * 16)),
    "pool",
    512,
  );
  if (totalPool < workers)
    throw new Error("Pool must allow at least one connection per worker");
  const gzipRaw = options.get("--gzip-level") ?? "1";
  const gzipLevel = ([0, 1, 2, 3, 4, 5, 6, 7, 8, 9] as const).find(
    (level) => String(level) === gzipRaw,
  );
  if (gzipLevel === undefined)
    throw new Error("gzip level must be 0 through 9");
  const databaseUrl = required("DATABASE_URL");
  let protocol: string;
  try {
    protocol = new URL(databaseUrl).protocol;
  } catch {
    throw new Error("DATABASE_URL must be a valid PostgreSQL URL");
  }
  if (!["postgres:", "postgresql:"].includes(protocol)) {
    throw new Error("DATABASE_URL must be a PostgreSQL URL");
  }
  const port = integer(process.env.PORT ?? "3000", "PORT", 65535);
  const extraPort = integer(
    process.env.EXTRA_PORT ?? "3001",
    "EXTRA_PORT",
    65535,
  );
  if (port === extraPort) throw new Error("PORT and EXTRA_PORT must differ");
  return {
    databaseUrl,
    jwtSecret: required("JWT_SECRET"),
    host: process.env.HOST ?? "127.0.0.1",
    ports: [port, extraPort],
    workers,
    poolPerWorker: Math.floor(totalPool / workers),
    gzipLevel,
  };
}

export type Configuration = ReturnType<typeof configuration>;
