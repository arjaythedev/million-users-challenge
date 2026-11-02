import { describe, expect, test } from "bun:test";
import { acceptsGzip } from "../src/app";
import { positiveId, postBody } from "../src/validation";

describe("request boundaries", () => {
  test("IDs accept decimal positive safe integers", () => {
    expect(positiveId("001")).toBe(1);
    for (const id of ["0", "-1", "1.5", "abc", "", "1e2", "9007199254740992"]) {
      expect(() => positiveId(id)).toThrow("invalid post id");
    }
  });
  test("post length follows PostgreSQL Unicode character semantics", () => {
    expect(postBody({ body: `  ${"😀".repeat(500)}  ` })).toBe(
      "😀".repeat(500),
    );
    expect(() => postBody({ body: "😀".repeat(501) })).toThrow(
      "body must be at most 500 characters",
    );
    expect(() => postBody({ body: " \n\t " })).toThrow("body is required");
  });
  test("gzip honors explicit refusals and wildcard negotiation", () => {
    for (const header of ["gzip", "br, GZIP;q=0.5", "*"])
      expect(acceptsGzip(header)).toBe(true);
    for (const header of [
      null,
      "br",
      "gzip;q=0",
      "gzip;q=0, *;q=1",
      "gzip;q=2",
    ])
      expect(acceptsGzip(header)).toBe(false);
  });
});
