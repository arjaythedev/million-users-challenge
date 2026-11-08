import type { Digit } from "./util";

// Minimal JSON field reads over a flat object text. Good enough for the JWT
// payload ({"sub":"..","username":"..","iat":N,"exp":N}) and the request body
// ({"body":".."}); not a general JSON parser.

// Text after the first occurrence of `"<Key>":` (whitespace-tolerant), or never.
export type After<J extends string, Key extends string> =
  J extends `${infer _}"${Key}"${infer R}` ? Colon<R> : never;
type Colon<R extends string> = Trim2<R> extends `:${infer V}` ? Trim2<V> : never;
type WS2 = " " | "\t" | "\n" | "\r";
type Trim2<S extends string> = S extends `${WS2}${infer R}` ? Trim2<R> : S;

// A string value immediately at the cursor -> its contents (no escapes), else a
// marker. Handles the common \" and \\ escapes so bodies round-trip.
export type ReadStr<R extends string> =
  R extends `"${infer Rest}` ? Scan<Rest, ""> : "\u0005notstr";
type Scan<S extends string, Acc extends string> =
  S extends `\\${infer E}${infer T}` ? Scan<T, `${Acc}\\${E}`>
  : S extends `"${infer _}` ? Acc
  : S extends `${infer C}${infer T}` ? Scan<T, `${Acc}${C}`>
  : Acc;

export type HasStr<R extends string> = R extends `"${infer _}` ? true : false;

// Leading decimal digits at the cursor.
export type ReadNum<R extends string> =
  R extends `${infer D extends Digit}${infer T}` ? `${D}${ReadNum<T>}` : "";

// Unescape \" \\ \/ \n \r \t \b \f in a JSON string's contents (for the POST body).
export type Unescape<S extends string> =
  S extends `${infer H}\\${infer E}${infer T}`
    ? `${H}${E extends "n" ? "\n" : E extends "t" ? "\t" : E extends "r" ? "\r" : E extends "b" ? "\b" : E extends "f" ? "\f" : E}${Unescape<T>}`
    : S;
