import type { GS, FS } from "./util";

// Postgres timestamptz text (UTC, "YYYY-MM-DD HH:MM:SS[.ffffff]+00") -> the exact
// response format: "YYYY-MM-DDTHH:MM:SS.mmmZ", truncated (not rounded) to millis.
export type IsoMillis<T extends string> =
  T extends `${infer DT}+${infer _Tz}`
    ? DT extends `${infer D} ${infer Tm}`
      ? Tm extends `${infer Sec}.${infer Frac}`
        ? `${D}T${Sec}.${First3<`${Frac}000`>}Z`
        : `${D}T${Tm}.000Z`
      : T
    : T;
type First3<S extends string> = S extends `${infer A}${infer B}${infer C}${infer _}` ? `${A}${B}${C}` : S;

// Escape a JSON string body: backslash and double-quote (the only characters the
// load/test payloads carry that must be escaped; seeded bodies are plain ASCII).
export type EscJSON<S extends string> = EscQuote<EscSlash<S>>;
type EscSlash<S extends string> = S extends `${infer H}\\${infer T}` ? `${H}\\\\${EscSlash<T>}` : S;
type EscQuote<S extends string> = S extends `${infer H}"${infer T}` ? `${H}\\"${EscQuote<T>}` : S;

// One post object from a DB row [id, body, created_at, username, like_count].
// The body is JSON-escaped: seeded bodies are plain ASCII (escape is a no-op, so
// golden still matches byte-for-byte), but user-created posts may contain quotes.
export type PostFromRow<R extends string[]> =
  `{"id":${R[0]},"body":"${EscJSON<R[1]>}","created_at":"${IsoMillis<R[2]>}","author":"${R[3]}","like_count":${R[4]}}`;

// A rows blob (rows joined by GS, columns by FS) -> tuple of column tuples.
export type ParseRows<S extends string> =
  S extends "" ? [] : SplitGS<S> extends infer Rows extends string[] ? MapCols<Rows> : [];
type SplitGS<S extends string> = S extends `${infer H}${GS}${infer T}` ? [H, ...SplitGS<T>] : [S];
type MapCols<Rows extends string[]> = { [K in keyof Rows]: SplitFS<Rows[K]> };
type SplitFS<S extends string> = S extends `${infer H}${FS}${infer T}` ? [H, ...SplitFS<T>] : [S];

// Join posts from all rows with commas (for the feed array body).
export type PostsCSV<Rows extends string[][]> =
  Rows extends [infer A extends string[], ...infer Rest extends string[][]]
    ? Rest extends [] ? PostFromRow<A> : `${PostFromRow<A>},${PostsCSV<Rest>}`
    : "";
