import { jwtVerify } from "jose";
import { HttpError } from "./errors";

export function authenticator(secret: string) {
  const key = new TextEncoder().encode(secret);
  return async (request: Request) => {
    const header = request.headers.get("authorization");
    if (!header?.startsWith("Bearer "))
      throw new HttpError(401, "missing bearer token");
    let payload;
    try {
      ({ payload } = await jwtVerify(header.slice(7), key, {
        algorithms: ["HS256"],
        requiredClaims: ["exp"],
      }));
    } catch {
      throw new HttpError(401, "invalid or expired token");
    }
    const id = typeof payload.sub === "string" ? Number(payload.sub) : NaN;
    if (
      typeof payload.sub !== "string" ||
      !/^\d+$/.test(payload.sub) ||
      !Number.isSafeInteger(id) ||
      id <= 0 ||
      typeof payload.username !== "string"
    ) {
      throw new HttpError(401, "invalid token payload");
    }
    return { id, username: payload.username };
  };
}
