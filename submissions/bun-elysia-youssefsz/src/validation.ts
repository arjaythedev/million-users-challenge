import { HttpError } from "./errors";

export function positiveId(value: string): number {
  const id = Number(value);
  if (!/^\d+$/.test(value) || !Number.isSafeInteger(id) || id <= 0) {
    throw new HttpError(400, "invalid post id");
  }
  return id;
}

export function postBody(input: unknown): string {
  if (
    input === null ||
    typeof input !== "object" ||
    !("body" in input) ||
    typeof input.body !== "string"
  ) {
    throw new HttpError(400, "body is required");
  }
  const body = input.body.trim();
  if (body.length === 0) throw new HttpError(400, "body is required");
  // PostgreSQL char_length counts Unicode code points, rather than UTF-16 code units.
  if (body.length > 500) {
    let length = 0;
    for (const _character of body) {
      if (++length > 500)
        throw new HttpError(400, "body must be at most 500 characters");
    }
  }
  return body;
}
