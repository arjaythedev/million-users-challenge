// An io_uring used only to batch system calls: epoll still says which client sockets are ready, but their recv()s
// and send()s are queued here and submitted together, so a pass enters the kernel once for all its reads and once
// for all its responses instead of once per request and once per response (and makes no FFI call per request).
// Every operation carries MSG_DONTWAIT, so it completes inside io_uring_enter and its result (bytes, -EAGAIN, an
// error) is handled exactly as the result of recv()/send() was. The same scheme as c-epoll-unorig-username.
//
// The rings are set up with raw system calls (no liburing) and read and written through typed arrays over the mapped
// memory. With no io_uring (old kernel, a seccomp profile that blocks it) `available` stays false and the caller
// uses plain recv()/send().
import { dlopen, FFIType, toArrayBuffer } from "bun:ffi";
import { alloc, errno } from "./ffi";

const { i32, i64, ptr: P, usize } = FFIType;
const libc = dlopen("libc.so.6", {
  syscall: { args: [i64, i64, i64, i64, i64, i64, i64], returns: i32 },
  mmap: { args: [P, usize, i32, i32, i32, i64], returns: P },
});
const SYS_io_uring_setup = 425; // the same number on x86_64 and aarch64
const SYS_io_uring_enter = 426;
const IORING_SETUP_SUBMIT_ALL = 1 << 7;
const IORING_ENTER_GETEVENTS = 1;
const IORING_OFF_SQ_RING = 0;
const IORING_OFF_CQ_RING = 0x8000000;
const IORING_OFF_SQES = 0x10000000;
const PROT_RW = 3, MAP_SHARED_POPULATE = 0x01 | 0x8000;
export const OP_SEND = 26, OP_RECV = 27;
const MSG_DONTWAIT = 0x40, MSG_NOSIGNAL = 0x4000;

export const ENTRIES = 4096;
export let available = false;
let ringFd = -1;
let sqTail: Uint32Array, sqMask = 0;
let cqHead: Uint32Array, cqTail: Uint32Array, cqMask = 0;
let sqes: Uint32Array; // 16 u32 per 64-byte entry
let cqes: Int32Array; // 4 i32 per 16-byte entry: user_data lo, hi, res, flags
let queued = 0; // entries written since the last submit

// Results of the last submit, indexed by user_data (the op's index in its batch).
export const res = new Int32Array(ENTRIES);

function view(base: number, off: number, len: number): ArrayBuffer {
  return toArrayBuffer(base, off, len) as ArrayBuffer;
}

export function setup(): boolean {
  if (process.env.NO_URING) return false;
  const params = alloc(120);
  const p32 = new Uint32Array(params.u8.buffer);
  p32[2] = IORING_SETUP_SUBMIT_ALL; // a failed operation does not hold back the ones queued after it (5.18+)
  let fd = libc.symbols.syscall(SYS_io_uring_setup, ENTRIES, params.addr, 0, 0, 0, 0);
  if (fd < 0) {
    p32.fill(0);
    fd = libc.symbols.syscall(SYS_io_uring_setup, ENTRIES, params.addr, 0, 0, 0, 0);
  }
  if (fd < 0) {
    console.error(`io_uring_setup failed (errno ${errno()}): plain recv/send`);
    return false;
  }
  const sqEntries = p32[0]!, cqEntries = p32[1]!;
  // struct io_sqring_offsets at 40: head, tail, ring_mask, ring_entries, flags, dropped, array
  const sqOff = (i: number) => p32[10 + i]!;
  // struct io_cqring_offsets at 80: head, tail, ring_mask, ring_entries, overflow, cqes
  const cqOff = (i: number) => p32[20 + i]!;
  const sqSize = sqOff(6) + sqEntries * 4;
  const cqSize = cqOff(5) + cqEntries * 16;
  const sqBase = Number(libc.symbols.mmap(0, sqSize, PROT_RW, MAP_SHARED_POPULATE, fd, IORING_OFF_SQ_RING));
  const cqBase = Number(libc.symbols.mmap(0, cqSize, PROT_RW, MAP_SHARED_POPULATE, fd, IORING_OFF_CQ_RING));
  const sqeBase = Number(libc.symbols.mmap(0, sqEntries * 64, PROT_RW, MAP_SHARED_POPULATE, fd, IORING_OFF_SQES));
  const bad = (a: number) => !(a > 0) || a >= 2 ** 63; // NULL or MAP_FAILED
  if (bad(sqBase) || bad(cqBase) || bad(sqeBase)) {
    console.error("io_uring mmap failed: plain recv/send");
    return false;
  }
  sqTail = new Uint32Array(view(sqBase, sqOff(1), 4));
  sqMask = new Uint32Array(view(sqBase, sqOff(2), 4))[0]!;
  const array = new Uint32Array(view(sqBase, sqOff(6), sqEntries * 4));
  for (let i = 0; i < sqEntries; i++) array[i] = i; // entry i always sits in slot i
  cqHead = new Uint32Array(view(cqBase, cqOff(0), 4));
  cqTail = new Uint32Array(view(cqBase, cqOff(1), 4));
  cqMask = new Uint32Array(view(cqBase, cqOff(2), 4))[0]!;
  cqes = new Int32Array(view(cqBase, cqOff(5), cqEntries * 16));
  sqes = new Uint32Array(view(sqeBase, 0, sqEntries * 64));
  ringFd = fd;
  available = true;
  return true;
}

// Queues a recv or send of len bytes at addr on fd; user_data is its index in the batch (0, 1, 2, ...).
export function queue(op: number, fd: number, addr: number, len: number): void {
  const tail = sqTail[0]!;
  const b = ((tail & sqMask) >>> 0) * 16;
  const s = sqes;
  s[b] = op; // opcode, flags 0, ioprio 0
  s[b + 1] = fd;
  s[b + 2] = 0; s[b + 3] = 0; // off
  s[b + 4] = addr >>> 0; s[b + 5] = Math.floor(addr / 4294967296); // addr
  s[b + 6] = len;
  s[b + 7] = op === OP_SEND ? MSG_DONTWAIT | MSG_NOSIGNAL : MSG_DONTWAIT;
  s[b + 8] = queued; s[b + 9] = 0; // user_data
  s[b + 10] = 0; s[b + 11] = 0; s[b + 12] = 0; s[b + 13] = 0; s[b + 14] = 0; s[b + 15] = 0;
  Atomics.store(sqTail, 0, (tail + 1) >>> 0);
  queued++;
}

export function pending(): number {
  return queued;
}

// Submits everything queued and waits for all of it (nothing blocks: MSG_DONTWAIT); results land in `res`.
// Returns how many operations there were.
export function submitAndWait(): number {
  const n = queued;
  if (n === 0) return 0;
  queued = 0;
  let toSubmit = n, got = 0;
  while (got < n) {
    const r = libc.symbols.syscall(SYS_io_uring_enter, ringFd, toSubmit, n - got, IORING_ENTER_GETEVENTS, 0, 0);
    if (r < 0) {
      const e = errno();
      if (e === 4 || e === 11 || e === 16) continue; // EINTR, EAGAIN, EBUSY: try again
      throw new Error(`io_uring_enter: errno ${e}`);
    }
    toSubmit -= Math.min(r, toSubmit);
    let head = cqHead[0]!;
    const tail = Atomics.load(cqTail, 0);
    while (head !== tail) {
      const c = ((head & cqMask) >>> 0) * 4;
      res[cqes[c]!] = cqes[c + 2]!;
      head = (head + 1) >>> 0;
      got++;
    }
    Atomics.store(cqHead, 0, head);
  }
  return n;
}
