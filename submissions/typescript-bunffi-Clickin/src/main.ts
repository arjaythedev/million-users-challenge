// Process model: one worker process per CPU, each running its own synchronous epoll loop (loop.ts) with its own
// SO_REUSEPORT listeners on both ports and its own Postgres connections. The primary only starts the workers and
// restarts any that die; a worker exits when it notices the primary is gone. With WORKERS=1 there is no primary.
import { lookup } from "node:dns/promises";
import * as jwt from "./jwt";
import { start } from "./loop";
import type { PgConfig } from "./pg";

const env = process.env;
const workers = Math.max(1, Number(env.WORKERS) || navigator.hardwareConcurrency || 1);

async function pgConfig(): Promise<PgConfig> {
  const url = new URL(env.DATABASE_URL ?? "postgres://postgres@localhost:5432/postgres");
  const host = url.hostname || "localhost";
  const ip = /^\d+\.\d+\.\d+\.\d+$/.test(host) ? host : (await lookup(host, { family: 4 })).address;
  return {
    ip,
    port: Number(url.port) || 5432,
    user: decodeURIComponent(url.username),
    password: decodeURIComponent(url.password),
    database: decodeURIComponent(url.pathname.slice(1)) || decodeURIComponent(url.username),
  };
}

async function worker(id: number): Promise<never> {
  const key = new TextEncoder().encode(env.JWT_SECRET ?? "");
  jwt.setKey(key);
  jwt.selfTest(key);
  const cfg = await pgConfig();
  const hostEnv = env.HOST ?? "0.0.0.0";
  const ip = hostEnv === "localhost" ? "127.0.0.1" : hostEnv;
  const ports = [Number(env.PORT ?? 3000)];
  if (env.EXTRA_PORT && Number(env.EXTRA_PORT) !== ports[0]) ports.push(Number(env.EXTRA_PORT));
  // Postgres connections, split over the workers: 64 for reads and 16 for writes in total (at most 512 allowed).
  const reads = Math.max(2, Math.round(Number(env.PG_READS ?? 64) / workers));
  const writes = Math.max(1, Math.round(Number(env.PG_WRITES ?? 16) / workers));
  console.log(`worker ${id}: pid ${process.pid}, ports ${ports.join(",")}, Postgres ${reads} read + ${writes} write connections`);
  return start(id, ip, ports, cfg, reads, writes);
}

function primary(): void {
  const children: (ReturnType<typeof Bun.spawn> | null)[] = [];
  const spawn = (id: number) => {
    const child = Bun.spawn([process.execPath, import.meta.path], {
      env: { ...env, WORKER_ID: String(id) },
      stdio: ["ignore", "inherit", "inherit"],
      onExit(_proc, code, signal) {
        if (stopping) return;
        console.error(`worker ${id} exited (code ${code}, signal ${signal}), restarting`);
        setTimeout(() => spawn(id), 500);
      },
    });
    children[id] = child;
  };
  let stopping = false;
  const stop = () => {
    stopping = true;
    for (const c of children) c?.kill("SIGTERM");
    process.exit(0);
  };
  process.on("SIGTERM", stop);
  process.on("SIGINT", stop);
  for (let i = 0; i < workers; i++) spawn(i);
}

if (env.WORKER_ID !== undefined) await worker(Number(env.WORKER_ID));
else if (workers === 1) await worker(0);
else primary();
