import { expect, test } from "bun:test";

test("startup errors redact malformed connection strings", async () => {
  const secret = "password-that-must-not-appear";
  const child = Bun.spawn(
    [
      process.execPath,
      "-e",
      'import { configuration } from "./src/config"; configuration(["--workers", "1"]);',
    ],
    {
      cwd: new URL("..", import.meta.url).pathname,
      env: {
        ...process.env,
        DATABASE_URL: `invalid://${secret} bad host`,
        JWT_SECRET: "test-only",
      },
      stdout: "pipe",
      stderr: "pipe",
    },
  );
  const stderr = await new Response(child.stderr).text();
  expect(await child.exited).toBe(1);
  expect(stderr).toContain("DATABASE_URL must be a valid PostgreSQL URL");
  expect(stderr).not.toContain(secret);
});
