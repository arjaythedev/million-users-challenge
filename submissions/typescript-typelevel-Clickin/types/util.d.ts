// Generic string/character utilities used across the type-level app.

// Protocol separators shared with the harness (server.mjs).
export type US = "\u001f"; // unit separator  (top-level fields of an input)
export type RS = "\u001e"; // record separator (output fields; phase-2 input fields)
export type GS = "\u001d"; // group separator  (rows)
export type FS = "\u001c"; // field separator  (columns in a row)

export type Digit = "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9";
export type NonZero = "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9";

// All characters are digits (non-empty).
export type AllDigits<S extends string> = S extends "" ? false
  : S extends `${Digit}${infer R}` ? (R extends "" ? true : AllDigits<R>) : false;

// A canonical positive integer string: "1".. (no leading zero, no sign, no dot).
export type IsPosInt<S extends string> =
  S extends `${NonZero}${infer R}` ? (R extends "" ? true : AllDigits<R>) : false;

// Split S on the first occurrence of D -> [before, after] | S has no D -> never match.
export type SplitFirst<S extends string, D extends string> =
  S extends `${infer A}${D}${infer B}` ? [A, B] : [S, never];

export type Replace<S extends string, A extends string, B extends string> =
  S extends `${infer H}${A}${infer T}` ? `${H}${B}${Replace<T, A, B>}` : S;

// Trim JSON whitespace (space, tab, CR, LF) from both ends.
type WS = " " | "\t" | "\n" | "\r";
export type TrimStart<S extends string> = S extends `${WS}${infer R}` ? TrimStart<R> : S;
export type TrimEnd<S extends string> = S extends `${infer R}${WS}` ? TrimEnd<R> : S;
export type Trim<S extends string> = TrimEnd<TrimStart<S>>;

// Length of a string as a unary tuple, capped — used only for "<= 500 chars".
export type ToChars<S extends string, Acc extends 0[] = []> =
  S extends `${infer _C}${infer R}` ? ToChars<R, [...Acc, 0]> : Acc;

// Is the character length of S greater than N (N given as a 0[] of length N)?
export type LongerThan<S extends string, N extends 0[]> = GT<ToChars<S>, N>;
type GT<A extends 0[], B extends 0[]> =
  A extends [0, ...infer AR extends 0[]]
    ? B extends [0, ...infer BR extends 0[]] ? GT<AR, BR> : true
    : false;

// Lowercase an ASCII string (for case-insensitive header matching).
type LC = { A: "a"; B: "b"; C: "c"; D: "d"; E: "e"; F: "f"; G: "g"; H: "h"; I: "i"; J: "j"; K: "k"; L: "l"; M: "m"; N: "n"; O: "o"; P: "p"; Q: "q"; R: "r"; S: "s"; T: "t"; U: "u"; V: "v"; W: "w"; X: "x"; Y: "y"; Z: "z" };
export type Lower<S extends string> =
  S extends `${infer C}${infer R}` ? `${C extends keyof LC ? LC[C] : C}${Lower<R>}` : "";

// Decimal comparison for non-negative integers without leading zeros (exp vs now).
export type Equal<A, B> = [A] extends [B] ? ([B] extends [A] ? true : false) : false;
type DIdx = { "0": []; "1": [0]; "2": [0,0]; "3": [0,0,0]; "4": [0,0,0,0]; "5": [0,0,0,0,0]; "6": [0,0,0,0,0,0]; "7": [0,0,0,0,0,0,0]; "8": [0,0,0,0,0,0,0,0]; "9": [0,0,0,0,0,0,0,0,0] };
type TupLt<A extends 0[], B extends 0[]> =
  A extends [0, ...infer AR extends 0[]]
    ? B extends [0, ...infer BR extends 0[]] ? TupLt<AR, BR> : false
    : B extends [0, ...infer _ extends 0[]] ? true : "eq";
type DigitLt<X extends string, Y extends string> = TupLt<DIdx[X & keyof DIdx], DIdx[Y & keyof DIdx]>;
type LexLt<A extends string, B extends string> =
  A extends `${infer x}${infer ar}`
    ? B extends `${infer y}${infer br}` ? (x extends y ? LexLt<ar, br> : DigitLt<x, y> extends true ? true : false) : false
    : B extends "" ? false : true;
// A < B ?
export type DecLt<A extends string, B extends string> =
  TupLt<ToChars<A>, ToChars<B>> extends infer L
    ? L extends true ? true : L extends false ? false : LexLt<A, B>
    : false;
