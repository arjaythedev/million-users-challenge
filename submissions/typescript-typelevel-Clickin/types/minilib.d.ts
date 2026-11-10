// Just enough of the standard library for noLib: the checker needs these global
// names to exist for arrays, tuples and template-literal types. No runtime.
interface Array<T> { length: number; [n: number]: T; }
interface ReadonlyArray<T> { readonly length: number; readonly [n: number]: T; }
interface Boolean {}
interface CallableFunction {}
interface Function {}
interface IArguments {}
interface NewableFunction {}
interface Number {}
interface Object {}
interface RegExp {}
interface String {}
