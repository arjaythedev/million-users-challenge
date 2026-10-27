import { afterAll, describe, expect, test } from "bun:test";
import { SignJWT } from "jose";
import { application } from "../src/app";
import { database } from "../src/database";

const url = process.env.TEST_DATABASE_URL;
const integration = url ? describe : describe.skip;

integration("real PostgreSQL integration", () => {
  const db = database(url ?? "postgres://unused/unused", 8);
  const secret = "integration-tests-only";
  const app = application(db, secret);
  const token = (payload: Record<string, unknown>, algorithm = "HS256") =>
    new SignJWT(payload)
      .setProtectedHeader({ alg: algorithm })
      .sign(new TextEncoder().encode(secret));
  const auth = async (id = "1") =>
    `Bearer ${await token({ sub: id, username: "from-token", exp: Math.floor(Date.now() / 1000) + 300 })}`;
  const request = (path: string, options?: RequestInit) =>
    app(new Request(`http://test${path}`, options));
  afterAll(() => db.client.close());

  test("auth precedes malformed JSON and invalid IDs", async () => {
    const response = await request("/posts", {
      method: "POST",
      body: "{invalid",
    });
    expect(response.status).toBe(401);
    expect(await response.json()).toEqual({ error: "missing bearer token" });
    const like = await request("/posts/abc/like", { method: "POST" });
    expect(like.status).toBe(401);
  });

  test("checks every token, expiration and algorithm", async () => {
    for (const payload of [
      { sub: "1", username: "u" },
      { sub: "1", username: "u", exp: 1 },
    ]) {
      const response = await request("/posts", {
        method: "POST",
        headers: { authorization: `Bearer ${await token(payload)}` },
        body: '{"body":"x"}',
      });
      expect(response.status).toBe(401);
    }
    const wrongAlgorithm = await token(
      { sub: "1", username: "u", exp: 9999999999 },
      "HS384",
    );
    expect(
      (
        await request("/posts/1/like", {
          method: "POST",
          headers: { authorization: `Bearer ${wrongAlgorithm}` },
        })
      ).status,
    ).toBe(401);
    const expires = Math.floor(Date.now() / 1000) + 1;
    const authorization = `Bearer ${await token({ sub: "1", username: "u", exp: expires })}`;
    expect(
      (
        await request("/posts/1/like", {
          method: "POST",
          headers: { authorization },
        })
      ).status,
    ).toBeLessThan(300);
    await Bun.sleep(Math.max(0, expires * 1000 - Date.now()) + 20);
    expect(
      (
        await request("/posts/1/like", {
          method: "POST",
          headers: { authorization },
        })
      ).status,
    ).toBe(401);
  });

  test("concurrent duplicate likes have one winner and one persisted row", async () => {
    const authorization = await auth();
    const created = await request("/posts", {
      method: "POST",
      headers: { authorization },
      body: JSON.stringify({ body: "race test 😀" }),
    });
    expect(created.status).toBe(201);
    const result = await created.json();
    const id: unknown = result.post.id;
    if (typeof id !== "number") throw new Error("Missing created post ID");
    const responses = await Promise.all(
      Array.from({ length: 30 }, () =>
        request(`/posts/${id}/like`, {
          method: "POST",
          headers: { authorization },
        }),
      ),
    );
    expect(
      responses.filter((response) => response.status === 201),
    ).toHaveLength(1);
    expect(
      responses.filter((response) => response.status === 200),
    ).toHaveLength(29);
    const rows =
      await db.client`SELECT count(*)::int AS n FROM likes WHERE user_id = 1 AND post_id = ${id}`;
    expect(rows[0].n).toBe(1);
    const read = await request(`/posts/${id}`);
    expect((await read.json()).post.like_count).toBe(1);
  });

  test("500 supplementary Unicode characters persist unchanged", async () => {
    const body = "😀".repeat(500);
    const response = await request("/posts", {
      method: "POST",
      headers: { authorization: await auth() },
      body: JSON.stringify({ body }),
    });
    expect(response.status).toBe(201);
    expect((await response.json()).post.body).toBe(body);
  });

  test("timestamp microseconds truncate and response key order is exact", async () => {
    const rows =
      await db.client`INSERT INTO posts (user_id, body, created_at) VALUES (1, 'microseconds', '2025-01-01T12:34:56.123999Z') RETURNING id`;
    const id = Number(rows[0].id);
    const response = await request(`/posts/${id}`);
    expect(await response.text()).toBe(
      `{"post":{"id":${id},"body":"microseconds","created_at":"2025-01-01T12:34:56.123Z","author":"golden_ember_1","like_count":0}}`,
    );
  });

  test("gzip decompresses to the same fresh response and q=0 is honored", async () => {
    const plain = await request("/feed");
    const compressed = await request("/feed", {
      headers: { "accept-encoding": "gzip" },
    });
    expect(compressed.headers.get("content-encoding")).toBe("gzip");
    expect(
      new TextDecoder().decode(
        Bun.gunzipSync(new Uint8Array(await compressed.arrayBuffer())),
      ),
    ).toBe(await plain.text());
    const refused = await request("/feed", {
      headers: { "accept-encoding": "gzip;q=0, *" },
    });
    expect(refused.headers.get("content-encoding")).toBeNull();
  });

  test("missing users are not mislabeled as missing posts", async () => {
    const response = await request("/posts/1/like", {
      method: "POST",
      headers: { authorization: await auth("999999999") },
    });
    expect(response.status).toBe(500);
    expect(await response.json()).toEqual({ error: "internal server error" });
  });

  test("unreachable database reports degraded health", async () => {
    const broken = database("postgres://test:test@127.0.0.1:1/missing", 1);
    try {
      const response = await application(
        broken,
        secret,
      )(new Request("http://test/health"));
      expect(response.status).toBe(503);
      expect(await response.json()).toEqual({
        status: "degraded",
        db: "unreachable",
        error: "database unavailable",
      });
    } finally {
      await broken.client.close();
    }
  });

  test("a request that expires in admission never writes later", async () => {
    const control = database(url ?? "postgres://unused/unused", 1);
    const authorization = await auth();
    const nonce = crypto.randomUUID();
    const options = (body: string) => ({
      method: "POST",
      headers: { authorization },
      body: JSON.stringify({ body }),
    });
    let running: Promise<Response>[] = [];
    try {
      await control.client.begin(async (tx) => {
        await tx`LOCK TABLE posts IN SHARE MODE`;
        running = Array.from({ length: db.capacity }, () =>
          request("/posts", options(`active-${nonce}`)),
        );
        for (let attempt = 0; ; attempt++) {
          const locks =
            await tx`SELECT count(*)::int AS n FROM pg_locks WHERE relation = 'posts'::regclass AND NOT granted`;
          if (locks[0].n === db.capacity) break;
          if (attempt >= 100)
            throw new Error("Requests did not reach the lock barrier");
          await Bun.sleep(10);
        }
        const queued = await request("/posts", options(`expired-${nonce}`));
        expect(queued.status).toBe(500);
      });
      expect(
        (await Promise.all(running)).every(
          (response) => response.status === 201,
        ),
      ).toBe(true);
      const rows =
        await control.client`SELECT count(*)::int AS n FROM posts WHERE body = ${`expired-${nonce}`}`;
      expect(rows[0].n).toBe(0);
    } finally {
      await control.client.close();
    }
  });

  test("slow uploads do not hold database execution slots", async () => {
    const authorization = await auth();
    const uploads = Array.from({ length: db.capacity }, () => {
      let complete = () => {};
      let readStarted = () => {};
      const ready = new Promise<void>((resolve) => {
        readStarted = resolve;
      });
      const stream = new ReadableStream({
        start(controller) {
          controller.enqueue(new TextEncoder().encode('{"body":"slow upload'));
          complete = () => {
            controller.enqueue(new TextEncoder().encode('"}'));
            controller.close();
          };
        },
        pull() {
          readStarted();
        },
      });
      return {
        ready,
        complete,
        response: request("/posts", {
          method: "POST",
          headers: { authorization },
          body: stream,
        }),
      };
    });
    try {
      await Promise.all(uploads.map((upload) => upload.ready));
      expect((await request("/health")).status).toBe(200);
    } finally {
      for (const upload of uploads) upload.complete();
    }
    expect(
      (await Promise.all(uploads.map((upload) => upload.response))).every(
        (response) => response.status === 201,
      ),
    ).toBe(true);
  });

  test("a recoverable SQL stall does not discard a bounded burst of writes", async () => {
    if (!url) throw new Error("TEST_DATABASE_URL is required");
    const db = database(url, 8);
    const control = database(url, 1);
    const secret = "stall-regression-only";
    const token = await new SignJWT({ sub: "1", username: "from-token" })
      .setProtectedHeader({ alg: "HS256" })
      .setExpirationTime("5m")
      .sign(new TextEncoder().encode(secret));
    const app = application(db, secret);
    const nonce = crypto.randomUUID();
    const write = () =>
      app(
        new Request("http://test/posts", {
          method: "POST",
          headers: { authorization: `Bearer ${token}` },
          body: JSON.stringify({ body: nonce }),
        }),
      );
    let active: Promise<Response>[] = [];
    let queued: Promise<Response>[] = [];
    try {
      await control.client.begin(async (tx) => {
        await tx`LOCK TABLE posts IN SHARE MODE`;
        active = Array.from({ length: db.capacity }, write);
        for (let attempt = 0; ; attempt++) {
          const locks = await tx`SELECT count(*)::int AS n FROM pg_locks
          WHERE relation='posts'::regclass AND NOT granted
          AND database=(SELECT oid FROM pg_database WHERE datname=current_database())`;
          if (locks[0].n === db.capacity) break;
          if (attempt >= 100) throw new Error("Lock barrier was not reached");
          await Bun.sleep(10);
        }
        queued = Array.from({ length: db.capacity * 16 + 1 }, write);
        await Bun.sleep(350);
      });
      const responses = await Promise.all([...active, ...queued]);
      expect(
        responses.filter((response) => response.status === 201).length,
      ).toBe(responses.length);
      const rows =
        await control.client`SELECT count(*)::int AS n FROM posts WHERE body=${nonce}`;
      expect(rows[0].n).toBe(responses.length);
    } finally {
      await Promise.all([db.client.close(), control.client.close()]);
    }
  }, 15_000);
});
