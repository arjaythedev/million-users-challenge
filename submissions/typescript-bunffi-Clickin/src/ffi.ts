// libc through bun:ffi: sockets, epoll and a few helpers, plus ISA-L for gzip.
//
// Every call takes plain numbers. Pointers are addresses of buffers allocated once at startup (see `alloc`), so no
// call converts a TypedArray or allocates. Return types are i32 even for ssize_t/size_t functions: a 64-bit return
// would come back as a BigInt (an allocation per call), and every size we deal with is far below 2 GB.
import { dlopen, FFIType, ptr, read } from "bun:ffi";

const { i32, u32, ptr: P, usize } = FFIType;

const libc = dlopen("libc.so.6", {
  socket: { args: [i32, i32, i32], returns: i32 },
  setsockopt: { args: [i32, i32, i32, P, u32], returns: i32 },
  getsockopt: { args: [i32, i32, i32, P, P], returns: i32 },
  bind: { args: [i32, P, u32], returns: i32 },
  listen: { args: [i32, i32], returns: i32 },
  accept4: { args: [i32, P, P, i32], returns: i32 },
  connect: { args: [i32, P, u32], returns: i32 },
  recv: { args: [i32, P, usize, i32], returns: i32 },
  send: { args: [i32, P, usize, i32], returns: i32 },
  close: { args: [i32], returns: i32 },
  epoll_create1: { args: [i32], returns: i32 },
  epoll_ctl: { args: [i32, i32, i32, P], returns: i32 },
  epoll_wait: { args: [i32, P, i32, i32], returns: i32 },
  fcntl: { args: [i32, i32, i32], returns: i32 },
  usleep: { args: [u32], returns: i32 },
  getppid: { args: [], returns: i32 },
  sched_setscheduler: { args: [i32, i32, P], returns: i32 },
  __errno_location: { args: [], returns: P },
});

export const C = libc.symbols;

// errno lives at a fixed thread-local address; this process runs JavaScript on one thread.
const errnoAddr = C.__errno_location()!;
export function errno(): number {
  return read.i32(errnoAddr, 0);
}

// ISA-L's igzip at level 0, stateless (one whole buffer per call), for gzip responses. The fields of
// struct isal_zstream we touch (igzip_lib.h, the same on x86_64 and aarch64): next_in @0, avail_in @8,
// next_out @16, avail_out @24, total_out @28, end_of_stream @56 (u16), gzip_flag @60 (u16); sizeof is 82,384 in
// 2.31, and isal_deflate_stateless_init sets the rest (level 0, the default Huffman tables).
const isal = dlopen("libisal.so.2", {
  isal_deflate_stateless_init: { args: [P], returns: FFIType.void },
  isal_deflate_stateless: { args: [P], returns: i32 },
});
const ISAL = isal.symbols;
const ZS_SIZE = 128 * 1024;
let zsAddr = 0, zs32: Uint32Array, zs16: Uint16Array;

function setPtr(i: number, addr: number): void {
  zs32[i] = addr >>> 0;
  zs32[i + 1] = Math.floor(addr / 4294967296);
}
// gzip src[0, len) into dst[0, cap). Returns the gzip size, or 0 when it didn't fit.
export function gzip(src: number, len: number, dst: number, cap: number): number {
  if (zsAddr === 0) {
    const m = alloc(ZS_SIZE);
    zsAddr = m.addr;
    zs32 = new Uint32Array(m.u8.buffer);
    zs16 = new Uint16Array(m.u8.buffer);
  }
  ISAL.isal_deflate_stateless_init(zsAddr);
  setPtr(0, src);
  zs32[2] = len;
  setPtr(4, dst);
  zs32[6] = cap;
  zs16[28] = 1; // end_of_stream
  zs16[30] = 1; // gzip_flag = IGZIP_GZIP
  if (ISAL.isal_deflate_stateless(zsAddr) !== 0 || zs32[2] !== 0) return 0;
  return zs32[7]!;
}

// A buffer whose address is fixed for its lifetime (large ArrayBuffers are never moved by the GC).
export interface Mem {
  u8: Uint8Array;
  addr: number;
}
export function alloc(size: number): Mem {
  const u8 = new Uint8Array(new ArrayBuffer(Math.max(size, 4096)));
  return { u8, addr: ptr(u8) };
}

// Linux constants (the same on x86_64 and aarch64).
export const AF_INET = 2;
export const SOCK_STREAM = 1;
export const SOCK_NONBLOCK = 0o4000;
export const SOCK_CLOEXEC = 0o2000000;
export const SOL_SOCKET = 1;
export const SO_REUSEADDR = 2;
export const SO_ERROR = 4;
export const SO_REUSEPORT = 15;
export const SO_RCVTIMEO = 20;
export const SO_SNDTIMEO = 21;
export const IPPROTO_TCP = 6;
export const TCP_NODELAY = 1;
export const TCP_DEFER_ACCEPT = 9;
export const SCHED_IDLE = 5;
export const EPOLLIN = 0x1;
export const EPOLLOUT = 0x4;
export const EPOLLERR = 0x8;
export const EPOLLHUP = 0x10;
export const EPOLLRDHUP = 0x2000;
export const EPOLL_CTL_ADD = 1;
export const EPOLL_CTL_DEL = 2;
export const EPOLL_CTL_MOD = 3;
export const EAGAIN = 11;
export const EINTR = 4;
export const MSG_NOSIGNAL = 0x4000;
export const F_GETFL = 3;
export const F_SETFL = 4;
export const O_NONBLOCK = 0o4000;

// struct epoll_event is packed on x86_64 (12 bytes: events, then data at offset 4) and naturally aligned on
// aarch64 (16 bytes: data at offset 8). We keep data as two u32 halves: low = index, high = kind.
export const EV_SIZE = process.arch === "x64" ? 12 : 16;
export const EV_DATA = process.arch === "x64" ? 1 : 2; // u32 index of data's low half within one event

// struct sockaddr_in for an IPv4 address "a.b.c.d" and port, written into m at offset 0. Returns its size.
export function sockaddrIn(m: Mem, ip: string, port: number): number {
  const u8 = m.u8;
  u8.fill(0, 0, 16);
  u8[0] = AF_INET & 0xff; // sa_family, host order (little-endian on both targets)
  u8[1] = 0;
  u8[2] = (port >> 8) & 0xff; // network order
  u8[3] = port & 0xff;
  const parts = ip.split(".");
  if (parts.length !== 4) throw new Error(`not an IPv4 address: ${ip}`);
  for (let i = 0; i < 4; i++) u8[4 + i] = Number(parts[i]) & 0xff;
  return 16;
}
