import { SQL } from "bun";
import { desc, eq, sql } from "drizzle-orm";
import { drizzle } from "drizzle-orm/bun-sql";
import {
  alias,
  bigint,
  bigserial,
  pgTable,
  text,
  timestamp,
} from "drizzle-orm/pg-core";
import { HttpError, logError } from "./errors";
import { admission } from "./admission";

// These definitions map the supplied schema. No migrations or schema writes run at startup.
const users = pgTable("users", {
  id: bigint({ mode: "number" }).primaryKey(),
  username: text().notNull(),
});
const posts = pgTable("posts", {
  id: bigserial({ mode: "number" }).primaryKey(),
  userId: bigint("user_id", { mode: "number" }).notNull(),
  body: text().notNull(),
  createdAt: timestamp("created_at", { withTimezone: true })
    .notNull()
    .defaultNow(),
});
const p = alias(posts, "p");
const u = alias(users, "u");

export function database(url: string, max: number) {
  const client = new SQL(url, {
    max,
    idleTimeout: 300,
    maxLifetime: 0,
    connectionTimeout: 5,
    prepare: true,
    connection: {
      application_name: "bun-elysia-feed",
      statement_timeout: "5000",
    },
  });
  const db = drizzle({ client });
  const work = admission(max);
  async function query<T>(
    signal: AbortSignal,
    execute: () => Promise<T>,
  ): Promise<T> {
    const ticket = work.acquire(signal);
    const result = typeof ticket === "string" ? ticket : await ticket;
    if (result !== "admitted") {
      logError(`database_admission_${result}`, undefined);
      throw new HttpError(500, "internal server error");
    }
    try {
      if (signal.aborted) throw new HttpError(500, "internal server error");
      return await execute();
    } finally {
      work.release();
    }
  }
  const fields = {
    id: p.id,
    body: p.body,
    createdAt: p.createdAt,
    username: u.username,
    likeCount: sql<number>`(SELECT count(*) FROM likes l WHERE l.post_id = ${p.id})::int`,
  };
  const feed = db
    .select(fields)
    .from(p)
    .innerJoin(u, eq(u.id, p.userId))
    .orderBy(desc(p.createdAt), desc(p.id))
    .limit(20)
    .prepare("feed");
  const post = db
    .select(fields)
    .from(p)
    .innerJoin(u, eq(u.id, p.userId))
    .where(eq(p.id, sql.placeholder("id")))
    .prepare("post");
  const insert = db
    .insert(posts)
    .values({
      userId: sql.placeholder("userId"),
      body: sql.placeholder("body"),
    })
    .returning({ id: posts.id, createdAt: posts.createdAt })
    .prepare("insert_post");

  return {
    client,
    capacity: max,
    health: (signal: AbortSignal) =>
      query(signal, async () => await db.execute(sql`SELECT 1`)),
    feed: (signal: AbortSignal) =>
      query(signal, async () => (await feed.execute()).map(postObject)),
    post: (id: number, signal: AbortSignal) =>
      query(signal, async () => {
        const row = (await post.execute({ id }))[0];
        if (!row) throw new HttpError(404, "post not found");
        return postObject(row);
      }),
    create: (
      userId: number,
      body: string,
      username: string,
      signal: AbortSignal,
    ) =>
      query(signal, async () => {
        const row = (await insert.execute({ userId, body }))[0];
        if (!row) throw new Error("INSERT did not return a post");
        return {
          id: row.id,
          body,
          created_at: row.createdAt.toISOString(),
          author: username,
          like_count: 0,
        };
      }),
    like: (userId: number, postId: number, signal: AbortSignal) =>
      query(signal, async () => {
        try {
          const result = await db.execute(
            sql`INSERT INTO likes (user_id, post_id) VALUES (${userId}, ${postId}) ON CONFLICT (user_id, post_id) DO NOTHING`,
          );
          if (
            !("count" in result) ||
            typeof result.count !== "number" ||
            (result.count !== 0 && result.count !== 1)
          ) {
            throw new Error("Driver did not return INSERT row count");
          }
          return result.count === 1;
        } catch (error) {
          // Drizzle wraps driver errors in cause. Match the specific FK, not every constraint failure.
          const cause = error instanceof Error ? error.cause : undefined;
          if (
            cause instanceof SQL.PostgresError &&
            cause.errno === "23503" &&
            cause.constraint === "likes_post_id_fkey"
          ) {
            throw new HttpError(404, "post not found");
          }
          throw error;
        }
      }),
  };
}

function postObject(row: {
  id: number;
  body: string;
  createdAt: Date;
  username: string;
  likeCount: number;
}) {
  return {
    id: row.id,
    body: row.body,
    created_at: row.createdAt.toISOString(),
    author: row.username,
    like_count: row.likeCount,
  };
}

export type Database = ReturnType<typeof database>;
