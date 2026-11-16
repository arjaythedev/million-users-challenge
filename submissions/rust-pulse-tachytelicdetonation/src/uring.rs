use std::sync::atomic::{AtomicU16, AtomicU32, Ordering};

const SYS_SETUP: libc::c_long = 425;
const SYS_ENTER: libc::c_long = 426;
const SYS_REGISTER: libc::c_long = 427;

pub const SETUP_CQSIZE: u32 = 1 << 3;
pub const SETUP_SUBMIT_ALL: u32 = 1 << 7;
pub const SETUP_COOP_TASKRUN: u32 = 1 << 8;
pub const SETUP_SINGLE_ISSUER: u32 = 1 << 12;
pub const SETUP_DEFER_TASKRUN: u32 = 1 << 13;

const FEAT_SINGLE_MMAP: u32 = 1 << 0;
const FEAT_EXT_ARG: u32 = 1 << 8;

pub const ENTER_GETEVENTS: u32 = 1 << 0;
pub const ENTER_EXT_ARG: u32 = 1 << 3;

pub const OP_ACCEPT: u8 = 13;
pub const OP_ASYNC_CANCEL: u8 = 14;
pub const OP_CLOSE: u8 = 19;
pub const OP_SEND: u8 = 26;
pub const OP_RECV: u8 = 27;
pub const OP_PROVIDE_BUFFERS: u8 = 31;

pub const SQE_FIXED_FILE: u8 = 1 << 0;
pub const SQE_BUFFER_SELECT: u8 = 1 << 5;
pub const SQE_CQE_SKIP_SUCCESS: u8 = 1 << 6;

pub const ACCEPT_MULTISHOT: u16 = 1 << 0;
pub const RECV_MULTISHOT: u16 = 1 << 1;
pub const FILE_INDEX_ALLOC: u32 = !0;

pub const CQE_F_BUFFER: u32 = 1 << 0;
pub const CQE_F_MORE: u32 = 1 << 1;

const REGISTER_FILES2: u32 = 13;
const REGISTER_PBUF_RING: u32 = 22;
const RSRC_REGISTER_SPARSE: u32 = 1 << 0;

#[repr(C)]
#[derive(Default)]
struct SqOff {
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
struct CqOff {
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
    sq_off: SqOff,
    cq_off: CqOff,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Sqe {
    pub opcode: u8,
    pub flags: u8,
    pub ioprio: u16,
    pub fd: i32,
    pub off: u64,
    pub addr: u64,
    pub len: u32,
    pub op_flags: u32,
    pub user_data: u64,
    pub buf_index: u16,
    pub personality: u16,
    pub file_index: u32,
    pub addr3: u64,
    pub pad: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Cqe {
    pub user_data: u64,
    pub res: i32,
    pub flags: u32,
}

#[repr(C)]
struct GetEventsArg {
    sigmask: u64,
    sigmask_sz: u32,
    min_wait_usec: u32,
    ts: u64,
}

#[repr(C)]
struct RsrcRegister {
    nr: u32,
    flags: u32,
    resv2: u64,
    data: u64,
    tags: u64,
}

#[repr(C)]
struct BufReg {
    ring_addr: u64,
    ring_entries: u32,
    bgid: u16,
    flags: u16,
    resv: [u64; 3],
}

pub struct Ring {
    pub fd: i32,
    pub setup_flags: u32,
    sq_head: *const AtomicU32,
    sq_tail: *const AtomicU32,
    sq_mask: u32,
    sq_entries: u32,
    sqes: *mut Sqe,
    cq_head: *const AtomicU32,
    cq_tail: *const AtomicU32,
    cq_mask: u32,
    cqes: *const Cqe,
    tail: u32,
    submitted: u32,
}

unsafe fn mmap(fd: i32, len: usize, off: i64) -> Result<*mut u8, i32> {
    let p = libc::mmap(
        std::ptr::null_mut(),
        len,
        libc::PROT_READ | libc::PROT_WRITE,
        libc::MAP_SHARED | libc::MAP_POPULATE,
        fd,
        off,
    );
    if p == libc::MAP_FAILED {
        Err(-*libc::__errno_location())
    } else {
        Ok(p as *mut u8)
    }
}

fn errno() -> i32 {
    unsafe { *libc::__errno_location() }
}

impl Ring {
    pub fn new(entries: u32, cq_entries: u32) -> Result<Ring, String> {
        let base = SETUP_CQSIZE;
        let tries = [
            base | SETUP_SUBMIT_ALL | SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN,
            base | SETUP_SUBMIT_ALL | SETUP_SINGLE_ISSUER | SETUP_COOP_TASKRUN,
            base | SETUP_SUBMIT_ALL | SETUP_COOP_TASKRUN,
            base | SETUP_SUBMIT_ALL,
            base,
        ];
        let mut last = 0;
        for flags in tries {
            let mut p = Params { flags, cq_entries, ..Default::default() };
            let fd = unsafe { libc::syscall(SYS_SETUP, entries, &mut p as *mut Params) } as i32;
            if fd < 0 {
                last = errno();
                if last == libc::ENOSYS || last == libc::EPERM {
                    break;
                }
                continue;
            }
            if p.features & FEAT_EXT_ARG == 0 {
                unsafe { libc::close(fd) };
                return Err("io_uring lacks EXT_ARG".into());
            }
            return unsafe { Self::map(fd, flags, &p) }.map_err(|e| format!("io_uring mmap errno {}", e));
        }
        Err(format!("io_uring_setup failed: errno {}", last))
    }

    unsafe fn map(fd: i32, flags: u32, p: &Params) -> Result<Ring, i32> {
        let sq_sz = p.sq_off.array as usize + p.sq_entries as usize * 4;
        let cq_sz = p.cq_off.cqes as usize + p.cq_entries as usize * std::mem::size_of::<Cqe>();
        let single = p.features & FEAT_SINGLE_MMAP != 0;
        let sq = mmap(fd, if single { sq_sz.max(cq_sz) } else { sq_sz }, 0)?;
        let cq = if single { sq } else { mmap(fd, cq_sz, 0x8000000)? };
        let sqes = mmap(fd, p.sq_entries as usize * std::mem::size_of::<Sqe>(), 0x10000000)? as *mut Sqe;
        let array = sq.add(p.sq_off.array as usize) as *mut u32;
        for i in 0..p.sq_entries {
            *array.add(i as usize) = i;
        }
        let tail = (*(sq.add(p.sq_off.tail as usize) as *const AtomicU32)).load(Ordering::Relaxed);
        Ok(Ring {
            fd,
            setup_flags: flags,
            sq_head: sq.add(p.sq_off.head as usize) as *const AtomicU32,
            sq_tail: sq.add(p.sq_off.tail as usize) as *const AtomicU32,
            sq_mask: *(sq.add(p.sq_off.ring_mask as usize) as *const u32),
            sq_entries: p.sq_entries,
            sqes,
            cq_head: cq.add(p.cq_off.head as usize) as *const AtomicU32,
            cq_tail: cq.add(p.cq_off.tail as usize) as *const AtomicU32,
            cq_mask: *(cq.add(p.cq_off.ring_mask as usize) as *const u32),
            cqes: cq.add(p.cq_off.cqes as usize) as *const Cqe,
            tail,
            submitted: tail,
        })
    }

    #[inline]
    pub fn sqe(&mut self) -> &mut Sqe {
        unsafe {
            if self.tail.wrapping_sub((*self.sq_head).load(Ordering::Acquire)) >= self.sq_entries {
                self.submit_only();
            }
            let s = &mut *self.sqes.add((self.tail & self.sq_mask) as usize);
            *s = std::mem::zeroed();
            self.tail = self.tail.wrapping_add(1);
            s
        }
    }

    fn publish(&mut self) -> u32 {
        unsafe { (*self.sq_tail).store(self.tail, Ordering::Release) };
        let n = self.tail.wrapping_sub(self.submitted);
        self.submitted = self.tail;
        n
    }

    pub fn submit_only(&mut self) {
        let n = self.publish();
        if n > 0 {
            unsafe { libc::syscall(SYS_ENTER, self.fd, n, 0, 0, std::ptr::null::<u8>(), 0usize) };
        }
    }

    pub fn submit_wait(&mut self, min_complete: u32, timeout_ns: u64) -> i32 {
        let n = self.publish();
        let ts = libc::timespec { tv_sec: (timeout_ns / 1_000_000_000) as _, tv_nsec: (timeout_ns % 1_000_000_000) as _ };
        let arg = GetEventsArg {
            sigmask: 0,
            sigmask_sz: 8,
            min_wait_usec: 0,
            ts: if timeout_ns > 0 { &ts as *const _ as u64 } else { 0 },
        };
        unsafe {
            libc::syscall(
                SYS_ENTER,
                self.fd,
                n,
                min_complete,
                ENTER_GETEVENTS | ENTER_EXT_ARG,
                &arg as *const GetEventsArg,
                std::mem::size_of::<GetEventsArg>(),
            ) as i32
        }
    }

    pub fn reap(&mut self, out: &mut Vec<Cqe>, max: usize) -> usize {
        unsafe {
            let head = (*self.cq_head).load(Ordering::Relaxed);
            let tail = (*self.cq_tail).load(Ordering::Acquire);
            let n = (tail.wrapping_sub(head) as usize).min(max);
            for i in 0..n {
                out.push(*self.cqes.add((head.wrapping_add(i as u32) & self.cq_mask) as usize));
            }
            (*self.cq_head).store(head.wrapping_add(n as u32), Ordering::Release);
            n
        }
    }

    pub fn register_files_sparse(&self, nr: u32) -> i32 {
        let r = RsrcRegister { nr, flags: RSRC_REGISTER_SPARSE, resv2: 0, data: 0, tags: 0 };
        let rc = unsafe {
            libc::syscall(SYS_REGISTER, self.fd, REGISTER_FILES2, &r as *const RsrcRegister, std::mem::size_of::<RsrcRegister>())
        };
        if rc < 0 {
            -errno()
        } else {
            0
        }
    }
}

pub enum BufMode {
    Ring,
    Legacy,
}

pub struct Bufs {
    pub mode: BufMode,
    pub base: *mut u8,
    pub size: usize,
    ring: *mut u8,
    mask: u16,
    tail: u16,
}

pub const BGID: u16 = 0;

impl Bufs {
    pub fn new(r: &mut Ring, count: u32, size: usize) -> Result<Bufs, String> {
        unsafe {
            let base = libc::mmap(
                std::ptr::null_mut(),
                count as usize * size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            );
            if base == libc::MAP_FAILED {
                return Err("buffer mmap failed".into());
            }
            let ring_sz = count as usize * 16;
            let ring = libc::mmap(
                std::ptr::null_mut(),
                ring_sz,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS | libc::MAP_POPULATE,
                -1,
                0,
            );
            if ring == libc::MAP_FAILED {
                return Err("ring mmap failed".into());
            }
            let mut b = Bufs {
                mode: BufMode::Ring,
                base: base as *mut u8,
                size,
                ring: ring as *mut u8,
                mask: (count - 1) as u16,
                tail: 0,
            };
            let mut reg = BufReg { ring_addr: ring as u64, ring_entries: count, bgid: BGID, flags: 0, resv: [0; 3] };
            let mut rc = libc::syscall(SYS_REGISTER, r.fd, REGISTER_PBUF_RING, &reg as *const BufReg, 1);
            if rc < 0 && errno() == libc::EINVAL {
                reg.resv[0] = 1;
                rc = libc::syscall(SYS_REGISTER, r.fd, REGISTER_PBUF_RING, &reg as *const BufReg, 1);
            }
            if rc < 0 {
                b.mode = BufMode::Legacy;
                let s = r.sqe();
                s.opcode = OP_PROVIDE_BUFFERS;
                s.fd = count as i32;
                s.addr = base as u64;
                s.len = size as u32;
                s.off = 0;
                s.buf_index = BGID;
                s.flags = SQE_CQE_SKIP_SUCCESS;
                s.user_data = 0;
                r.submit_only();
            } else {
                for bid in 0..count {
                    b.push(bid as u16);
                }
                b.commit();
            }
            Ok(b)
        }
    }

    #[inline]
    pub fn data(&self, bid: u16, len: usize) -> &'static [u8] {
        unsafe { std::slice::from_raw_parts(self.base.add(bid as usize * self.size), len) }
    }

    #[inline]
    fn push(&mut self, bid: u16) {
        unsafe {
            let e = self.ring.add(((self.tail & self.mask) as usize) * 16);
            *(e as *mut u64) = self.base.add(bid as usize * self.size) as u64;
            *(e.add(8) as *mut u32) = self.size as u32;
            *(e.add(12) as *mut u16) = bid;
        }
        self.tail = self.tail.wrapping_add(1);
    }

    #[inline]
    pub fn commit(&mut self) {
        unsafe { (*(self.ring.add(14) as *const AtomicU16)).store(self.tail, Ordering::Release) };
    }

    #[inline]
    pub fn give_back(&mut self, r: &mut Ring, bid: u16) {
        match self.mode {
            BufMode::Ring => {
                self.push(bid);
                self.commit();
            }
            BufMode::Legacy => {
                let s = r.sqe();
                s.opcode = OP_PROVIDE_BUFFERS;
                s.fd = 1;
                s.addr = unsafe { self.base.add(bid as usize * self.size) } as u64;
                s.len = self.size as u32;
                s.off = bid as u64;
                s.buf_index = BGID;
                s.flags = SQE_CQE_SKIP_SUCCESS;
                s.user_data = 0;
            }
        }
    }

    pub fn ring_mode(&self) -> bool {
        matches!(self.mode, BufMode::Ring)
    }
}
