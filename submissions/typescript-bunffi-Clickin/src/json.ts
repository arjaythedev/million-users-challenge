// Writes response JSON straight into a preallocated byte buffer: no strings and no objects per response.
const enc = new TextEncoder();
export const bytes = (s: string): Uint8Array => enc.encode(s);

const HEX = bytes("0123456789abcdef");

export class Writer {
  buf: Uint8Array;
  pos = 0;
  constructor(buf: Uint8Array) {
    this.buf = buf;
  }

  // Short constants are copied with a loop: TypedArray.set costs a native call, more than copying ~20 bytes.
  raw(b: Uint8Array): void {
    const n = b.length;
    if (n > 64) {
      this.buf.set(b, this.pos);
    } else {
      const buf = this.buf;
      const p = this.pos;
      for (let i = 0; i < n; i++) buf[p + i] = b[i]!;
    }
    this.pos += n;
  }

  byte(c: number): void {
    this.buf[this.pos++] = c;
  }

  // A non-negative integer below 2^53.
  uint(v: number): void {
    const buf = this.buf;
    if (v < 10) {
      buf[this.pos++] = 48 + v;
      return;
    }
    let digits = 1;
    for (let t = v; t >= 10; t = Math.floor(t / 10)) digits++;
    let i = this.pos + digits - 1;
    for (let t = v; i >= this.pos; i--) {
      const q = Math.floor(t / 10);
      buf[i] = 48 + (t - q * 10);
      t = q;
    }
    this.pos += digits;
  }

  // A JSON string from UTF-8 bytes src[start, end): quotes, backslashes and control characters escaped,
  // everything else (non-ASCII included) copied as is.
  str(src: Uint8Array, start: number, end: number): void {
    const buf = this.buf;
    let p = this.pos;
    buf[p++] = 34;
    for (let i = start; i < end; i++) {
      const c = src[i]!;
      if (c >= 0x20 && c !== 34 && c !== 92) {
        buf[p++] = c;
      } else if (c === 34 || c === 92) {
        buf[p++] = 92;
        buf[p++] = c;
      } else {
        buf[p++] = 92; // \u00XX
        buf[p++] = 117;
        buf[p++] = 48;
        buf[p++] = 48;
        buf[p++] = HEX[c >> 4]!;
        buf[p++] = HEX[c & 15]!;
      }
    }
    buf[p++] = 34;
    this.pos = p;
  }

  // A JSON string from a JavaScript string (error messages, the rare non-hot paths).
  jsStr(s: string): void {
    const b = enc.encode(s);
    this.str(b, 0, b.length);
  }

  // timestamptz in microseconds since 2000-01-01 UTC, as "YYYY-MM-DDTHH:MM:SS.mmmZ" (truncated to ms).
  timestamp(us: number): void {
    const rem = us % 1000;
    let ms = (us - rem) / 1000 - (rem < 0 ? 1 : 0) + 946684800000; // floor, then the Unix epoch
    const msPart = ms % 1000;
    let secs = (ms - msPart) / 1000;
    const days = Math.floor(secs / 86400);
    let sod = secs - days * 86400;
    // civil_from_days (H. Hinnant)
    const z = days + 719468;
    const era = Math.floor(z / 146097);
    const doe = z - era * 146097;
    const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
    let y = yoe + era * 400;
    const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100));
    const mp = Math.floor((5 * doy + 2) / 153);
    const d = doy - Math.floor((153 * mp + 2) / 5) + 1;
    const m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    const hh = Math.floor(sod / 3600);
    sod -= hh * 3600;
    const mi = Math.floor(sod / 60);
    const ss = sod - mi * 60;
    const b = this.buf;
    let p = this.pos;
    b[p++] = 34;
    b[p++] = 48 + Math.floor(y / 1000);
    b[p++] = 48 + (Math.floor(y / 100) % 10);
    b[p++] = 48 + (Math.floor(y / 10) % 10);
    b[p++] = 48 + (y % 10);
    b[p++] = 45;
    b[p++] = 48 + Math.floor(m / 10);
    b[p++] = 48 + (m % 10);
    b[p++] = 45;
    b[p++] = 48 + Math.floor(d / 10);
    b[p++] = 48 + (d % 10);
    b[p++] = 84;
    b[p++] = 48 + Math.floor(hh / 10);
    b[p++] = 48 + (hh % 10);
    b[p++] = 58;
    b[p++] = 48 + Math.floor(mi / 10);
    b[p++] = 48 + (mi % 10);
    b[p++] = 58;
    b[p++] = 48 + Math.floor(ss / 10);
    b[p++] = 48 + (ss % 10);
    b[p++] = 46;
    b[p++] = 48 + Math.floor(msPart / 100);
    b[p++] = 48 + (Math.floor(msPart / 10) % 10);
    b[p++] = 48 + (msPart % 10);
    b[p++] = 90;
    b[p++] = 34;
    this.pos = p;
  }
}
