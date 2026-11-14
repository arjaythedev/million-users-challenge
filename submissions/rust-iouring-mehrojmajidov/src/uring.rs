//! io_uring through its three system calls, with the rings mapped by hand. Only what the loop uses: multishot
//! accept, multishot receive into provided buffers, one-shot receive, send, and a wait that submits the queued
//! requests and times out in the same call. Everything here exists since Linux 6.1, except setting a socket
//! option (6.7), which the caller replaces with the system call where it is refused. The benchmark box runs 6.8.

use crate::sys;
use core::ffi::{c_long, c_void};
use core::sync::atomic::{AtomicU16, AtomicU32, Ordering};
use std::collections::VecDeque;

// The same numbers on every architecture Linux has added since 5.1.
const SYS_SETUP: c_long = 425;
const SYS_ENTER: c_long = 426;
const SYS_REGISTER: c_long = 427;

const SETUP_CQSIZE: u32 = 1 << 3;
const SETUP_CLAMP: u32 = 1 << 4;
const SETUP_SUBMIT_ALL: u32 = 1 << 7;
const SETUP_COOP_TASKRUN: u32 = 1 << 8;
const SETUP_SINGLE_ISSUER: u32 = 1 << 12;
const SETUP_DEFER_TASKRUN: u32 = 1 << 13;
const FEAT_SINGLE_MMAP: u32 = 1 << 0;
const FEAT_NODROP: u32 = 1 << 1;
const FEAT_EXT_ARG: u32 = 1 << 8;
const ENTER_GETEVENTS: u32 = 1 << 0;
const ENTER_EXT_ARG: u32 = 1 << 3;
const OFF_SQ_RING: i64 = 0;
const OFF_SQES: i64 = 0x1000_0000;
const REGISTER_PBUF_RING: u32 = 22;

const OP_ACCEPT: u8 = 13;
const OP_SEND: u8 = 26;
const OP_RECV: u8 = 27;
const OP_PROVIDE_BUFFERS: u8 = 31;
const OP_URING_CMD: u8 = 46;
const SOCKET_OP_SETSOCKOPT: u64 = 3;
const SQE_BUFFER_SELECT: u8 = 1 << 5;
const SQE_CQE_SKIP_SUCCESS: u8 = 1 << 6;
const ACCEPT_MULTISHOT: u16 = 1 << 0;
const RECV_MULTISHOT: u16 = 1 << 1;
/// The completion carries a provided buffer; its number is in the flags' upper half.
pub const CQE_BUFFER: u32 = 1 << 0;
/// The multishot request stays armed; without this flag the completion is the request's last.
pub const CQE_MORE: u32 = 1 << 1;
const GROUP: u16 = 0;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Sqe {
    opcode: u8,
    flags: u8,
    ioprio: u16,
    fd: i32,
    off: u64,
    addr: u64,
    len: u32,
    op_flags: u32,
    user_data: u64,
    buf_group: u16,
    personality: u16,
    file_index: u32,
    pad: [u64; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Cqe {
    pub user_data: u64,
    pub res: i32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Default)]
struct SqOffsets {
    head: u32,
    tail: u32,
    ring_mask: u32,
    ring_entries: u32,
    flags: u32,
    dropped: u32,
    array: u32,
    resv1: u32,
    user_addr: u64,
}

#[repr(C)]
#[derive(Default)]
struct CqOffsets {
    head: u32,
    tail: u32,
    ring_mask: u32,
    ring_entries: u32,
    overflow: u32,
    cqes: u32,
    flags: u32,
    resv1: u32,
    user_addr: u64,
}

#[repr(C)]
#[derive(Default)]
struct Params {
    sq_entries: u32,
    cq_entries: u32,
    flags: u32,
    sq_thread_cpu: u32,
    sq_thread_idle: u32,
    features: u32,
    wq_fd: u32,
    resv: [u32; 3],
    sq_off: SqOffsets,
    cq_off: CqOffsets,
}

/// One slot of a provided-buffer ring. The ring's tail is overlaid on the first slot's last two bytes.
#[repr(C)]
struct BufSlot {
    addr: u64,
    len: u32,
    bid: u16,
    tail: u16,
}

#[repr(C)]
#[derive(Default)]
struct BufReg {
    ring_addr: u64,
    ring_entries: u32,
    bgid: u16,
    flags: u16,
    resv: [u64; 3],
}

#[repr(C)]
struct WaitArg {
    sigmask: u64,
    sigmask_sz: u32,
    pad: u32,
    ts: u64,
}

fn map(len: usize, flags: i32, fd: i32, off: i64) -> Result<*mut u8, i32> {
    let p = unsafe { sys::mmap(core::ptr::null_mut(), len, sys::PROT_READ | sys::PROT_WRITE, flags, fd, off) };
    if p as isize == -1 {
        Err(sys::errno())
    } else {
        Ok(p.cast())
    }
}

fn setup(entries: u32, completions: u32, p: &mut Params) -> Result<i32, i32> {
    p.cq_entries = completions;
    // One thread submits and reaps, and completions are worked off only when it asks for them, so the kernel
    // never interrupts the loop (or another core) to deliver one.
    p.flags = SETUP_CQSIZE | SETUP_CLAMP | SETUP_SUBMIT_ALL | SETUP_COOP_TASKRUN | SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN;
    let fd = unsafe { sys::syscall(SYS_SETUP, entries as usize, p as *mut Params) } as i32;
    if fd < 0 {
        Err(sys::errno())
    } else {
        Ok(fd)
    }
}

/// Whether this kernel lets this process make the kind of ring `Ring::new` makes. `Err` carries the errno.
pub fn probe() -> Result<(), i32> {
    let fd = setup(8, 16, &mut Params::default())?;
    unsafe { sys::close(fd) };
    Ok(())
}

pub struct Ring {
    fd: i32,
    sq_khead: *const AtomicU32,
    sq_ktail: *const AtomicU32,
    sq_mask: u32,
    sq_entries: u32,
    sqes: *mut Sqe,
    sq_tail: u32,
    cq_khead: *const AtomicU32,
    cq_ktail: *const AtomicU32,
    cq_mask: u32,
    cqes: *const Cqe,
    cq_head: u32,
    /// Requests that found the submission queue full. They go in, in order, once the kernel has taken a batch.
    backlog: VecDeque<Sqe>,
    pool: *mut u8,
    buf_size: usize,
    /// Null when the kernel refused a buffer ring: buffers then go back to it one request each.
    buf_ring: *mut BufSlot,
    buf_mask: u16,
    buf_tail: u16,
}

impl Ring {
    /// A ring for the calling thread, which must be the only one to use it, with `buffers` receive buffers of
    /// `buf_size` bytes (a power of two of them, at most 32,768). `buffer_ring`: hand them back through a
    /// shared ring where the kernel has that, instead of one request each. `Err` carries the errno.
    pub fn new(entries: u32, completions: u32, buffers: u32, buf_size: usize, buffer_ring: bool) -> Result<Ring, i32> {
        assert!(buffers.is_power_of_two() && buffers <= 32_768);
        let mut p = Params::default();
        let fd = setup(entries, completions, &mut p)?;
        let fail = |e: i32| {
            unsafe { sys::close(fd) };
            Err(e)
        };
        if p.features & (FEAT_SINGLE_MMAP | FEAT_NODROP | FEAT_EXT_ARG) != FEAT_SINGLE_MMAP | FEAT_NODROP | FEAT_EXT_ARG {
            return fail(sys::EINVAL);
        }
        let sq_len = p.sq_off.array as usize + p.sq_entries as usize * 4;
        let cq_len = p.cq_off.cqes as usize + p.cq_entries as usize * core::mem::size_of::<Cqe>();
        let shared = sys::MAP_SHARED | sys::MAP_POPULATE;
        let private = sys::MAP_PRIVATE | sys::MAP_ANONYMOUS;
        let (Ok(rings), Ok(sqes)) = (
            map(sq_len.max(cq_len), shared, fd, OFF_SQ_RING),
            map(p.sq_entries as usize * core::mem::size_of::<Sqe>(), shared, fd, OFF_SQES),
        ) else {
            return fail(sys::errno());
        };
        let (Ok(pool), Ok(buf_ring)) = (
            map(buffers as usize * buf_size, private, -1, 0),
            map(buffers as usize * core::mem::size_of::<BufSlot>(), private, -1, 0),
        ) else {
            return fail(sys::errno());
        };
        let at = |off: u32| unsafe { rings.add(off as usize) };
        let word = |off: u32| unsafe { *(at(off) as *const u32) };
        // The index array between the queue and its entries is the identity, once and for all.
        for i in 0..p.sq_entries {
            unsafe { *(at(p.sq_off.array) as *mut u32).add(i as usize) = i };
        }
        let mut ring = Ring {
            fd,
            sq_khead: at(p.sq_off.head).cast(),
            sq_ktail: at(p.sq_off.tail).cast(),
            sq_mask: word(p.sq_off.ring_mask),
            sq_entries: p.sq_entries,
            sqes: sqes.cast(),
            sq_tail: word(p.sq_off.tail),
            cq_khead: at(p.cq_off.head).cast(),
            cq_ktail: at(p.cq_off.tail).cast(),
            cq_mask: word(p.cq_off.ring_mask),
            cqes: at(p.cq_off.cqes).cast(),
            cq_head: word(p.cq_off.head),
            backlog: VecDeque::new(),
            pool,
            buf_size,
            buf_ring: buf_ring.cast(),
            buf_mask: (buffers - 1) as u16,
            buf_tail: 0,
        };

        let mut reg = BufReg { ring_addr: buf_ring as u64, ring_entries: buffers, bgid: GROUP, ..BufReg::default() };
        let register =
            |reg: &BufReg| unsafe { sys::syscall(SYS_REGISTER, fd as usize, REGISTER_PBUF_RING as usize, reg as *const BufReg, 1usize) };
        let mut rc = if buffer_ring { register(&reg) } else { -1 };
        if buffer_ring && rc < 0 && sys::errno() == sys::EINVAL {
            // Ubuntu's 6.8.0-136 and later check the reserved words the wrong way round (Launchpad #2162843):
            // they refuse zeros and take a nonzero first word. Kernels without the bug never get here.
            reg.resv[0] = 1;
            rc = register(&reg);
        }
        if rc == 0 {
            for bid in 0..buffers {
                ring.recycle(bid as u16);
            }
        } else {
            ring.buf_ring = core::ptr::null_mut();
            ring.push(Sqe {
                opcode: OP_PROVIDE_BUFFERS,
                flags: SQE_CQE_SKIP_SUCCESS,
                fd: buffers as i32,
                addr: pool as u64,
                len: buf_size as u32,
                buf_group: GROUP,
                ..Sqe::default()
            });
        }
        Ok(ring)
    }

    /// Whether receive buffers go back through a shared ring (no request each).
    pub fn has_buffer_ring(&self) -> bool {
        !self.buf_ring.is_null()
    }

    fn push(&mut self, sqe: Sqe) {
        let taken = unsafe { (*self.sq_khead).load(Ordering::Acquire) };
        if !self.backlog.is_empty() || self.sq_tail.wrapping_sub(taken) == self.sq_entries {
            self.backlog.push_back(sqe);
            return;
        }
        unsafe { *self.sqes.add((self.sq_tail & self.sq_mask) as usize) = sqe };
        self.sq_tail = self.sq_tail.wrapping_add(1);
    }

    /// Requests are waiting that the submission queue had no room for; the next `enter` should not sleep.
    pub fn backlogged(&self) -> bool {
        !self.backlog.is_empty()
    }

    /// Submits everything queued, then waits until `min_complete` completions are in or `timeout_ns` has passed
    /// (0: does not wait). One system call either way.
    pub fn enter(&mut self, min_complete: u32, timeout_ns: u64) {
        let taken = unsafe { (*self.sq_khead).load(Ordering::Acquire) };
        while self.sq_tail.wrapping_sub(taken) < self.sq_entries {
            let Some(sqe) = self.backlog.pop_front() else { break };
            unsafe { *self.sqes.add((self.sq_tail & self.sq_mask) as usize) = sqe };
            self.sq_tail = self.sq_tail.wrapping_add(1);
        }
        unsafe { (*self.sq_ktail).store(self.sq_tail, Ordering::Release) };
        let to_submit = self.sq_tail.wrapping_sub(taken);
        // Failures need no handling here: a timeout and an interrupted wait are normal, and after any other
        // (the completion queue is full, no memory for requests) the requests stay queued for the next call.
        if timeout_ns == 0 {
            let none = core::ptr::null::<c_void>();
            unsafe { sys::syscall(SYS_ENTER, self.fd as usize, to_submit as usize, 0usize, ENTER_GETEVENTS as usize, none, 0usize) };
        } else {
            let ts = sys::Timespec { sec: (timeout_ns / 1_000_000_000) as i64, nsec: (timeout_ns % 1_000_000_000) as i64 };
            let arg = WaitArg { sigmask: 0, sigmask_sz: 8, pad: 0, ts: &ts as *const sys::Timespec as u64 };
            unsafe {
                sys::syscall(
                    SYS_ENTER,
                    self.fd as usize,
                    to_submit as usize,
                    min_complete as usize,
                    (ENTER_GETEVENTS | ENTER_EXT_ARG) as usize,
                    &arg as *const WaitArg,
                    core::mem::size_of::<WaitArg>(),
                )
            };
        }
    }

    pub fn pop(&mut self) -> Option<Cqe> {
        if self.cq_head == unsafe { (*self.cq_ktail).load(Ordering::Acquire) } {
            return None;
        }
        let cqe = unsafe { *self.cqes.add((self.cq_head & self.cq_mask) as usize) };
        self.cq_head = self.cq_head.wrapping_add(1);
        unsafe { (*self.cq_khead).store(self.cq_head, Ordering::Release) };
        Some(cqe)
    }

    /// The user data and flags of the completion `ahead` places after the next one, if it has arrived: for a
    /// prefetch of what it will touch.
    pub fn peek(&self, ahead: u32) -> Option<(u64, u32)> {
        let waiting = unsafe { (*self.cq_ktail).load(Ordering::Relaxed) }.wrapping_sub(self.cq_head);
        (waiting > ahead).then(|| {
            let c = unsafe { &*self.cqes.add((self.cq_head.wrapping_add(ahead) & self.cq_mask) as usize) };
            (c.user_data, c.flags)
        })
    }

    /// Asks for the start of a provided buffer ahead of its use. The kernel filled every buffer of a pass
    /// before the first completion is looked at, so by then the early ones have left the nearest cache.
    #[inline]
    pub fn expect_buffer(&self, bid: u16) {
        let at = self.pool.wrapping_add(bid as usize * self.buf_size);
        crate::sys::prefetch(at);
        crate::sys::prefetch(at.wrapping_add(64));
    }

    /// Accepts on `fd` until told otherwise: one completion per connection, its descriptor as the result.
    pub fn accept(&mut self, fd: i32, flags: u32, user_data: u64) {
        self.push(Sqe { opcode: OP_ACCEPT, ioprio: ACCEPT_MULTISHOT, fd, op_flags: flags, user_data, ..Sqe::default() });
    }

    /// Receives on `fd` until it fails or ends: one completion per arrival, each with a provided buffer.
    pub fn receive_provided(&mut self, fd: i32, user_data: u64) {
        self.push(Sqe {
            opcode: OP_RECV,
            flags: SQE_BUFFER_SELECT,
            ioprio: RECV_MULTISHOT,
            fd,
            user_data,
            buf_group: GROUP,
            ..Sqe::default()
        });
    }

    /// One receive into memory that must stay where it is until the completion arrives.
    pub fn receive(&mut self, fd: i32, buf: *mut u8, len: usize, user_data: u64) {
        self.push(Sqe { opcode: OP_RECV, fd, addr: buf as u64, len: len as u32, user_data, ..Sqe::default() });
    }

    /// One send of memory that must stay as it is until the completion arrives. The kernel keeps at it until
    /// everything is sent or the socket fails, so a short count means the socket failed part-way.
    pub fn send(&mut self, fd: i32, buf: *const u8, len: usize, user_data: u64) {
        self.push(Sqe {
            opcode: OP_SEND,
            fd,
            addr: buf as u64,
            len: len as u32,
            op_flags: sys::MSG_WAITALL | sys::MSG_NOSIGNAL,
            user_data,
            ..Sqe::default()
        });
    }

    /// Sets an integer socket option without a system call. A completion arrives only if it failed (on a kernel
    /// before 6.7, say), carrying `user_data`; `value` is read when the request is submitted.
    pub fn set_option(&mut self, fd: i32, level: i32, name: i32, value: &'static i32, user_data: u64) {
        self.push(Sqe {
            opcode: OP_URING_CMD,
            flags: SQE_CQE_SKIP_SUCCESS,
            fd,
            off: SOCKET_OP_SETSOCKOPT,
            // the option's level and name share the address field
            addr: level as u32 as u64 | (name as u32 as u64) << 32,
            file_index: 4,
            pad: [value as *const i32 as u64, 0],
            user_data,
            ..Sqe::default()
        });
    }

    /// The bytes a receive completion delivered.
    ///
    /// # Safety
    /// `bid` and `len` come from that completion, and the slice is dropped before `recycle(bid)`.
    pub unsafe fn buffer(&self, bid: u16, len: usize) -> &'static [u8] {
        core::slice::from_raw_parts(self.pool.add(bid as usize * self.buf_size), len.min(self.buf_size))
    }

    /// Hands a receive buffer back to the kernel.
    pub fn recycle(&mut self, bid: u16) {
        let addr = unsafe { self.pool.add(bid as usize * self.buf_size) } as u64;
        if self.buf_ring.is_null() {
            return self.push(Sqe {
                opcode: OP_PROVIDE_BUFFERS,
                flags: SQE_CQE_SKIP_SUCCESS,
                fd: 1,
                off: bid as u64,
                addr,
                len: self.buf_size as u32,
                buf_group: GROUP,
                ..Sqe::default()
            });
        }
        unsafe {
            // field by field: the first slot's last two bytes are the tail
            let slot = self.buf_ring.add((self.buf_tail & self.buf_mask) as usize);
            (*slot).addr = addr;
            (*slot).len = self.buf_size as u32;
            (*slot).bid = bid;
            self.buf_tail = self.buf_tail.wrapping_add(1);
            (*(core::ptr::addr_of!((*self.buf_ring).tail) as *const AtomicU16)).store(self.buf_tail, Ordering::Release);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::os::fd::AsRawFd;
    use std::os::unix::net::UnixStream;

    fn ring(buffer_ring: bool) -> Option<Ring> {
        match Ring::new(64, 256, 8, 64, buffer_ring) {
            Ok(r) => Some(r),
            Err(e) => {
                // containers and some CI runners forbid io_uring; the server then runs its epoll loop
                eprintln!("io_uring is not available here (errno {e}); test skipped");
                None
            }
        }
    }

    fn reap(r: &mut Ring, want: usize) -> Vec<Cqe> {
        let mut got = Vec::new();
        for _ in 0..200 {
            r.enter(1, 10_000_000);
            while let Some(c) = r.pop() {
                got.push(c);
            }
            if got.len() >= want {
                break;
            }
        }
        got
    }

    #[test]
    fn probe_agrees_with_new() {
        assert_eq!(probe().is_ok(), ring(true).is_some());
    }

    #[test]
    fn layouts() {
        assert_eq!(core::mem::size_of::<Sqe>(), 64);
        assert_eq!(core::mem::size_of::<Cqe>(), 16);
        assert_eq!(core::mem::size_of::<Params>(), 120);
        assert_eq!(core::mem::size_of::<BufSlot>(), 16);
        assert_eq!(core::mem::size_of::<BufReg>(), 40);
        assert_eq!(core::mem::size_of::<WaitArg>(), 24);
    }

    #[test]
    fn multishot_receive_fills_provided_buffers_and_reports_the_end() {
        for buffer_ring in [true, false] {
            multishot_receive(buffer_ring);
        }
    }

    fn multishot_receive(buffer_ring: bool) {
        let Some(mut r) = ring(buffer_ring) else { return };
        assert_eq!(r.has_buffer_ring(), buffer_ring);
        let (mut a, b) = UnixStream::pair().unwrap();
        r.receive_provided(b.as_raw_fd(), 7);
        a.write_all(b"hello").unwrap();
        let c = reap(&mut r, 1);
        assert_eq!((c.len(), c[0].user_data, c[0].res), (1, 7, 5));
        assert!(c[0].flags & CQE_BUFFER != 0 && c[0].flags & CQE_MORE != 0);
        let bid = (c[0].flags >> 16) as u16;
        assert_eq!(unsafe { r.buffer(bid, 5) }, b"hello");
        r.recycle(bid);

        // more than one buffer's worth arrives as several completions, in order
        let big: Vec<u8> = (0..150u8).collect();
        a.write_all(&big).unwrap();
        let c = reap(&mut r, 3);
        let mut seen = Vec::new();
        for c in &c {
            assert!(c.res > 0 && c.flags & CQE_BUFFER != 0);
            let bid = (c.flags >> 16) as u16;
            seen.extend_from_slice(unsafe { r.buffer(bid, c.res as usize) });
            r.recycle(bid);
        }
        assert_eq!(seen, big);

        // every buffer has been used and handed back at least once by now: the pool does not run dry
        for round in 0..20u8 {
            a.write_all(&[round; 40]).unwrap();
            let c = reap(&mut r, 1);
            assert_eq!(c[0].res, 40, "round {round}");
            r.recycle((c[0].flags >> 16) as u16);
        }

        drop(a);
        let c = reap(&mut r, 1);
        assert_eq!(c[0].res, 0);
        assert_eq!(c[0].flags & CQE_MORE, 0, "the end of the stream ends the request");
    }

    #[test]
    fn send_and_one_shot_receive() {
        let Some(mut r) = ring(true) else { return };
        let (a, mut b) = UnixStream::pair().unwrap();
        let out = vec![0x5a_u8; 300_000];
        r.send(a.as_raw_fd(), out.as_ptr(), out.len(), 1);
        // larger than the socket buffer: the send completes only once the other side has read everything
        r.enter(0, 0);
        assert!(r.pop().is_none());
        let mut reader = b.try_clone().unwrap();
        let read = std::thread::spawn(move || {
            let mut got = vec![0u8; 300_000];
            reader.read_exact(&mut got).unwrap();
            got
        });
        let c = reap(&mut r, 1);
        assert_eq!((c[0].user_data, c[0].res as usize), (1, out.len()));
        assert_eq!(read.join().unwrap(), out);

        let mut inb = [0u8; 16];
        r.receive(a.as_raw_fd(), inb.as_mut_ptr(), inb.len(), 2);
        b.write_all(b"pong").unwrap();
        let c = reap(&mut r, 1);
        assert_eq!((c[0].user_data, c[0].res, c[0].flags & CQE_MORE), (2, 4, 0));
        assert_eq!(&inb[..4], b"pong");
    }

    #[test]
    fn socket_option_through_the_ring() {
        use std::net::{TcpListener, TcpStream};
        let Some(mut r) = ring(true) else { return };
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let s = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        static ON: i32 = 1;
        assert!(!s.nodelay().unwrap());
        r.set_option(s.as_raw_fd(), sys::IPPROTO_TCP, sys::TCP_NODELAY, &ON, 9);
        r.enter(0, 0);
        match r.pop() {
            // success is silent
            None => assert!(s.nodelay().unwrap()),
            // a kernel before 6.7 refuses the request, and says so
            Some(c) => assert!(c.user_data == 9 && c.res < 0 && !s.nodelay().unwrap()),
        }
    }

    #[test]
    fn a_full_submission_queue_backs_up_and_drains() {
        let Some(mut r) = ring(true) else { return };
        let (a, mut b) = UnixStream::pair().unwrap();
        let byte = [1u8];
        for i in 0..200u64 {
            r.send(a.as_raw_fd(), byte.as_ptr(), 1, 100 + i);
        }
        assert!(r.backlogged());
        let mut seen = 0;
        for _ in 0..50 {
            r.enter(0, 0);
            while let Some(c) = r.pop() {
                assert_eq!(c.res, 1);
                seen += 1;
            }
            if seen == 200 {
                break;
            }
        }
        assert_eq!(seen, 200);
        assert!(!r.backlogged());
        let mut got = [0u8; 200];
        b.read_exact(&mut got).unwrap();
    }
}
