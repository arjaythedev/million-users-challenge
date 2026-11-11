// Drives the TypeScript 7.1 type checker as the computation engine. Each call
// writes one virtual module `export type Out = App<"<input>">` and reads back the
// string literal the checker deduced for `Out`. All application logic (HTTP
// parsing, routing, JWT/HMAC verification, JSON validation, choosing the SQL,
// interpreting rows, building the response) lives in the types; this is the FFI.
import { API, SymbolFlags } from "typescript/sync";
import { serverFS } from "typescript/fs";
import path from "node:path";

const OPTS = { noLib: true, strict: true, noEmit: true, types: [], skipLibCheck: true, noErrorTruncation: true };

export class Evaluator {
  constructor(typesDir) {
    this.dir = path.resolve(typesDir);
    this.virt = new Map();
    this.seq = 0;
    this.api = new API({
      cwd: this.dir,
      fs: {
        readFile: (f) => (this.virt.has(f) ? this.virt.get(f) : serverFS.useOS),
        fileExists: (f) => (this.virt.has(f) ? true : serverFS.useOS),
        directoryExists: serverFS.useOS, getAccessibleEntries: serverFS.useOS,
        realpath: serverFS.useOS, stat: serverFS.useOS, writeFile: serverFS.noop, removeFile: serverFS.noop,
      },
    });
    this.lib = path.join(this.dir, "minilib.d.ts");
    this.entry = path.join(this.dir, "app.ts");
  }

  // input: a latin1 string. Returns the latin1 string the checker produced for Out.
  eval(input) {
    const esc = JSON.stringify(input).replace(/[\u007f-￿]/g, (c) => "\\u" + c.charCodeAt(0).toString(16).padStart(4, "0"));
    const f = path.join(this.dir, `__req${this.seq++}.ts`);
    this.virt.set(f, `import type { App } from "./app";\nexport type Out = App<${esc}>;\n`);
    try {
      const prog = this.api.createProgram([this.lib, this.entry, f], OPTS);
      try {
        const ch = prog.getProject().checker;
        const sf = prog.getSourceFile(f);
        const sym = ch.resolveName("Out", SymbolFlags.TypeAlias, { document: f, position: sf.end });
        const ty = ch.getDeclaredTypeOfSymbol(sym);
        if (typeof ty.value === "string") return ty.value;
        return "\u0001" + ch.typeToString(ty); // sentinel: not a string literal
      } finally { prog.dispose(); }
    } finally { this.virt.delete(f); }
  }
}
