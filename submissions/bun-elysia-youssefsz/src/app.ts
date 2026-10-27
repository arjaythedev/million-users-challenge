import { Elysia } from "elysia";
import { authenticator } from "./auth";
import type { Configuration } from "./config";
import type { Database } from "./database";
import { HttpError, logError } from "./errors";
import { positiveId, postBody } from "./validation";
import { jsonBody } from "./body";
import { MAX_IN_FLIGHT_REQUESTS } from "./admission";

export function application(
  db: Database,
  secret: string,
  gzipLevel: Configuration["gzipLevel"] = 1,
) {
  const authenticate = authenticator(secret);
  let active = 0;

  const app = new Elysia({ precompile: true })
    .onError(({ error, code, set }) => {
      if (error instanceof HttpError) {
        set.status = error.status;
        return { error: error.message };
      }
      if (code === "NOT_FOUND") {
        set.status = 404;
        return { error: "not found" };
      }
      logError("request_failed", error);
      set.status = 500;
      return { error: "internal server error" };
    })
    .mapResponse(({ request, responseValue, set }) => {
      const json = JSON.stringify(responseValue);
      const headers = new Headers({
        "content-type": "application/json; charset=utf-8",
      });
      let body: string | Uint8Array<ArrayBuffer> = json;
      if (json.length >= 1024) {
        headers.set("vary", "Accept-Encoding");
        if (
          gzipLevel > 0 &&
          acceptsGzip(request.headers.get("accept-encoding"))
        ) {
          body = Bun.gzipSync(json, { level: gzipLevel });
          headers.set("content-encoding", "gzip");
        }
      }
      return new Response(body, {
        status: typeof set.status === "number" ? set.status : 200,
        headers,
      });
    })
    .get("/health", async ({ request, set }) => {
      try {
        await db.health(request.signal);
        return {
          status: "ok",
          db: "ok",
          uptime_s: Math.floor(process.uptime()),
        };
      } catch {
        set.status = 503;
        return {
          status: "degraded",
          db: "unreachable",
          error: "database unavailable",
        };
      }
    })
    .get("/feed", async ({ request }) => ({
      posts: await db.feed(request.signal),
    }))
    .get("/posts/:id", async ({ request, params }) => ({
      post: await db.post(positiveId(params.id), request.signal),
    }))
    // Disable automatic parsing so authentication always runs before JSON and parameter validation.
    .post(
      "/posts",
      async ({ request, set }) => {
        const user = await authenticate(request);
        const input = await jsonBody(request);
        const body = postBody(input);
        if (request.signal.aborted)
          throw new HttpError(500, "internal server error");
        const post = await db.create(
          user.id,
          body,
          user.username,
          request.signal,
        );
        set.status = 201;
        return { post };
      },
      { parse: "none" },
    )
    .post(
      "/posts/:id/like",
      async ({ request, params, set }) => {
        const user = await authenticate(request);
        const id = positiveId(params.id);
        if (request.signal.aborted)
          throw new HttpError(500, "internal server error");
        const inserted = await db.like(user.id, id, request.signal);
        set.status = inserted ? 201 : 200;
        return { liked: true, already_liked: !inserted, post_id: id };
      },
      { parse: "none" },
    );

  app.compile();
  return async (request: Request) => {
    if (active >= MAX_IN_FLIGHT_REQUESTS || request.signal.aborted) {
      logError("admission_rejected", undefined);
      return Response.json({ error: "internal server error" }, { status: 500 });
    }
    active++;
    try {
      return await app.handle(request);
    } finally {
      active--;
    }
  };
}

export function acceptsGzip(header: string | null): boolean {
  if (!header) return false;
  let wildcard = false;
  for (const part of header.split(",")) {
    const [rawName, ...parameters] = part.trim().toLowerCase().split(";");
    let quality = 1;
    for (const parameter of parameters) {
      const match = /^\s*q\s*=\s*([0-9.]+)\s*$/.exec(parameter);
      if (match) quality = Number(match[1]);
    }
    if (rawName === "gzip") return quality > 0 && quality <= 1;
    if (rawName === "*") wildcard = quality > 0 && quality <= 1;
  }
  return wildcard;
}
