import type { US, RS, IsPosInt, Trim, LongerThan } from "./util";
import type { ParseReq } from "./http";
import type { ParseRows, PostFromRow, PostsCSV, IsoMillis, EscJSON } from "./format";
import type { Auth } from "./jwt";
import type { After, ReadStr, HasStr, Unescape } from "./json";

// The single entry point the harness evaluates: App<"<input>">. First field picks
// the phase: "R" = route a request, "F" = format a reply from DB rows. The route
// phase also receives the signing input, already padded to SHA blocks by the
// harness and transported base64url (decoded here before HMAC).

// Phase-1 input is `R <US> now <US> rawRequest <US> signingTail`. The signing tail
// is the HMAC signing input already SHA-padded by the harness; it is the LAST
// field so its arbitrary bytes (which may include a US) don't need escaping, and
// the request (which carries no US) is captured non-greedily before it.
// Phase-1 input: `R <US> now <US> rawRequest <US> headerJson <US> payloadJson <US> signingTail`.
// headerJson/payloadJson are the harness's base64url-decode of the token's first
// two segments (encoding transport, not logic). signingTail is the SHA-padded
// signing input and goes last, since its bytes may include a US.
export type App<Input extends string> =
  Input extends `R${US}${infer Now}${US}${infer Raw}${US}${infer Hdr}${US}${infer Pl}${US}${infer Tail}`
    ? Route<Now, Raw, Hdr, Pl, Tail>
  : Input extends `F${RS}${infer Rest}` ? Format<Rest>
  : Err500;

type Direct<Status extends string, Body extends string> = `D${RS}${Status}${RS}${Body}`;
type Query<Act extends string, Ctx extends string, Params extends string[]> =
  `Q${RS}${Act}${RS}${Ctx}${JoinParams<Params>}`;
type JoinParams<P extends string[]> =
  P extends [infer A extends string, ...infer R extends string[]] ? `${RS}${A}${JoinParams<R>}` : "";
type Reply<Status extends string, Body extends string> = `${Status}${RS}${Body}`;

type Err500 = Direct<"500", `{"error":"internal server error"}`>;
type NotFound = Direct<"404", `{"error":"not found"}`>;
type E401<M extends string> = Direct<"401", `{"error":"${M}"}`>;
type E400<M extends string> = Direct<"400", `{"error":"${M}"}`>;

// 500-element tuple for the "<= 500 characters" check.
type Build<N extends number, Acc extends 0[] = []> = Acc["length"] extends N ? Acc : Build<N, [...Acc, 0]>;
type N500 = Build<500>;

// ============================ phase 1: routing ============================
type Route<Now extends string, Raw extends string, Hdr extends string, Pl extends string, Tail extends string> =
  ParseReq<Raw> extends infer Q
    ? Q extends { method: infer M extends string; path: infer P extends string; auth: infer A; body: infer B extends string }
      ? RouteMP<M, P, A, B, Now, Tail, Hdr, Pl>
      : Err500
    : Err500;

type RouteMP<M extends string, P extends string, A, B extends string, Now extends string, Tail extends string, Hdr extends string, Pl extends string> =
  P extends "/health" ? Query<"5", "H", []>
  : P extends "/feed" ? Query<"1", "L", []>
  : P extends "/posts" ? (M extends "POST" ? GuardRun<Auth<A, Tail, Now, Hdr, Pl>, { k: "create"; b: B }> : NotFound)
  : P extends `/posts/${infer Rest}`
    ? Rest extends `${infer Id}/like`
      ? (M extends "POST" ? GuardRun<Auth<A, Tail, Now, Hdr, Pl>, { k: "like"; id: Id }> : NotFound)
      : (M extends "GET" ? GetPost<Rest> : NotFound)
  : NotFound;

type GetPost<Id extends string> =
  IsPosInt<Id> extends true ? Query<"2", "G", [Id]> : E400<"invalid post id">;

// Apply an auth result: errors map to SPEC messages; success dispatches the route.
type GuardRun<R, Spec> =
  R extends { err: "missing" } ? E401<"missing bearer token">
  : R extends { err: "invalid" } ? E401<"invalid or expired token">
  : R extends { err: "payload" } ? E401<"invalid token payload">
  : R extends { ok: true; sub: infer Sub extends string; name: infer Name extends string }
    ? Spec extends { k: "create"; b: infer B extends string } ? Create<B, Sub, Name>
    : Spec extends { k: "like"; id: infer Id extends string } ? Like<Id, Sub>
    : Err500
  : Err500;

// POST /posts: validate the JSON body, then INSERT (statement 3).
type Create<B extends string, Sub extends string, Name extends string> =
  Trim<B> extends `{${infer _M}}`
    ? After<B, "body"> extends infer BA
      ? [BA] extends [never] ? E400<"body is required">
        : BA extends string ? HasStr<BA> extends true ? ValidateBody<Unescape<ReadStr<BA>>, Sub, Name> : E400<"body is required">
        : E400<"body is required">
      : E400<"body is required">
    : E400<"malformed JSON body">;

type ValidateBody<Raw extends string, Sub extends string, Name extends string> =
  Trim<Raw> extends "" ? E400<"body is required">
  : LongerThan<Trim<Raw>, N500> extends true ? E400<"body must be at most 500 characters">
  : Query<"3", `C${US}${Name}${US}${EscJSON<Trim<Raw>>}`, [Sub, Trim<Raw>]>;

// POST /posts/:id/like: auth already passed; validate id, then INSERT (statement 4).
type Like<Id extends string, Sub extends string> =
  IsPosInt<Id> extends true ? Query<"4", `K${US}${Id}`, [Sub, Id]> : E400<"invalid post id">;

// ============================ phase 2: formatting =========================
type Format<Rest extends string> =
  Rest extends `${infer Ctx}${RS}${infer Cmd}${RS}${infer Code}${RS}${infer Rows}`
    ? Fmt<Ctx, Cmd, Code, Rows>
    : Reply<"500", `{"error":"internal server error"}`>;

type Fmt<Ctx extends string, Cmd extends string, Code extends string, RowsBlob extends string> =
  Ctx extends "H"
    ? Code extends "" ? Reply<"200", `{"status":"ok","db":"ok","uptime_s":\u0003}`>
                      : Reply<"503", `{"status":"degraded","db":"unreachable","error":"\u0004"}`>
  : Ctx extends "L"
    ? Reply<"200", `{"posts":[${PostsCSV<ParseRows<RowsBlob>>}]}`>
  : Ctx extends "G"
    ? ParseRows<RowsBlob> extends [infer R extends string[], ...infer _] ? Reply<"200", `{"post":${PostFromRow<R>}}`>
      : Reply<"404", `{"error":"post not found"}`>
  : Ctx extends `C${US}${infer Name}${US}${infer Body}`
    ? ParseRows<RowsBlob> extends [infer R extends string[], ...infer _]
      ? Reply<"201", `{"post":{"id":${R[0]},"body":"${Body}","created_at":"${IsoMillis<R[1]>}","author":"${Name}","like_count":0}}`>
      : Reply<"500", `{"error":"internal server error"}`>
  : Ctx extends `K${US}${infer Id}`
    ? Code extends "23503" ? Reply<"404", `{"error":"post not found"}`>
      : Cmd extends `${infer _} 0` ? Reply<"200", `{"liked":true,"already_liked":true,"post_id":${Id}}`>
      : Reply<"201", `{"liked":true,"already_liked":false,"post_id":${Id}}`>
  : Reply<"500", `{"error":"internal server error"}`>;
