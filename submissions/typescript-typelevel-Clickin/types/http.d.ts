import type { Lower } from "./util";

// A parsed request: method, path (request target), the Authorization header value
// (or never if absent), and the body. Returns false when the request line is not
// the expected HTTP/1.1 shape.
export type ParseReq<Raw extends string> =
  Raw extends `${infer M} ${infer P} HTTP/1.1\r\n${infer Rest}`
    ? SplitHead<Rest> extends [infer H extends string, infer B extends string]
      ? { method: M; path: P; auth: FindHeader<`\r\n${H}`, "authorization">; body: B }
      : false
    : false;

type SplitHead<S extends string> =
  S extends `${infer H}\r\n\r\n${infer B}` ? [H, B] : [S extends `\r\n${infer _}` ? "" : S, ""];

// Find a header by (lowercased) name in headers prefixed with "\r\n". Value is
// trimmed of one leading space. Absent -> never.
type FindHeader<H extends string, Name extends string> =
  H extends `\r\n${infer Line}\r\n${infer Rest}`
    ? [MatchHeader<Line, Name>] extends [never] ? FindHeader<`\r\n${Rest}`, Name> : MatchHeader<Line, Name>
    : H extends `\r\n${infer Line}` ? MatchHeader<Line, Name> : never;

type MatchHeader<Line extends string, Name extends string> =
  Line extends `${infer K}:${infer V}` ? (Lower<K> extends Name ? Deblank<V> : never) : never;

type Deblank<S extends string> = S extends ` ${infer R}` ? R : S;
