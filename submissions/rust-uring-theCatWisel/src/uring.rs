#![allow(dead_code)]

use std::ffi::c_void;
use std::sync::atomic::{fence, Ordering};

use crate::sys::{
    close, errno, socketpair, syscall, write, Timespec, AF_UNIX, EINTR, IPPROTO_TCP, SOCK_CLOEXEC,
    SOCK_NONBLOCK, SOCK_STREAM, TCP_QUICKACK,
};

#[cfg(target_arch = "x86_64")]
const SYS_MMAP: i64 = 9;
#[cfg(target_arch = "x86_64")]
const SYS_MUNMAP: i64 = 11;
#[cfg(target_arch = "x86_64")]
const SYS_MADVISE: i64 = 28;
#[cfg(target_arch = "x86_64")]
const SYS_IO_URING_SETUP: i64 = 425;
#[cfg(target_arch = "x86_64")]
const SYS_IO_URING_ENTER: i64 = 426;
#[cfg(target_arch = "x86_64")]
const SYS_IO_URING_REGISTER: i64 = 427;

const PROT_READ: i32 = 0x1;
const PROT_WRITE: i32 = 0x2;
const MAP_SHARED: i32 = 0x01;
const MAP_PRIVATE: i32 = 0x02;
const MAP_ANONYMOUS: i32 = 0x20;
const MAP_POPULATE: i32 = 0x08000;
const MADV_NOHUGEPAGE: i32 = 15;

pub const IOSQE_FIXED_FILE: u8 = 1 << 0;
pub const IOSQE_IO_LINK: u8 = 1 << 2;
pub const IOSQE_IO_HARDLINK: u8 = 1 << 3;
pub const IOSQE_BUFFER_SELECT: u8 = 1 << 5;
pub const IOSQE_CQE_SKIP_SUCCESS: u8 = 1 << 6;

pub const IORING_SETUP_CQSIZE: u32 = 1 << 3;
pub const IORING_SETUP_CLAMP: u32 = 1 << 4;
pub const IORING_SETUP_SUBMIT_ALL: u32 = 1 << 7;
pub const IORING_SETUP_COOP_TASKRUN: u32 = 1 << 8;
pub const IORING_SETUP_SINGLE_ISSUER: u32 = 1 << 12;
pub const IORING_SETUP_DEFER_TASKRUN: u32 = 1 << 13;
pub const IORING_SETUP_NO_SQARRAY: u32 = 1 << 16;

pub const IORING_OP_POLL_ADD: u8 = 6;
pub const IORING_OP_POLL_REMOVE: u8 = 7;
pub const IORING_OP_TIMEOUT: u8 = 11;
pub const IORING_OP_ACCEPT: u8 = 13;
pub const IORING_OP_ASYNC_CANCEL: u8 = 14;
pub const IORING_OP_CLOSE: u8 = 19;
pub const IORING_OP_SEND: u8 = 26;
pub const IORING_OP_RECV: u8 = 27;
pub const IORING_OP_URING_CMD: u8 = 46;

pub const SOCKET_URING_OP_SETSOCKOPT: u64 = 3;

pub const IORING_POLL_ADD_MULTI: u32 = 1 << 0;
pub const IORING_TIMEOUT_MULTISHOT: u32 = 1 << 6;
pub const IORING_ACCEPT_MULTISHOT: u16 = 1 << 0;
pub const IORING_RECV_MULTISHOT: u16 = 1 << 1;
pub const IORING_RECVSEND_BUNDLE: u16 = 1 << 4;

pub const IORING_ASYNC_CANCEL_ALL: u32 = 1 << 0;
pub const IORING_ASYNC_CANCEL_FD: u32 = 1 << 1;
pub const IORING_ASYNC_CANCEL_FD_FIXED: u32 = 1 << 4;

pub const IORING_FILE_INDEX_ALLOC: i32 = -1;

pub const IORING_CQE_F_BUFFER: u32 = 1 << 0;
pub const IORING_CQE_F_MORE: u32 = 1 << 1;
pub const IORING_CQE_F_SOCK_NONEMPTY: u32 = 1 << 2;
pub const IORING_CQE_BUFFER_SHIFT: u32 = 16;

const IORING_OFF_SQ_RING: i64 = 0;
const IORING_OFF_CQ_RING: i64 = 0x800_0000;
const IORING_OFF_SQES: i64 = 0x1000_0000;

const IORING_ENTER_GETEVENTS: u32 = 1 << 0;
const IORING_ENTER_EXT_ARG: u32 = 1 << 3;
const IORING_ENTER_REGISTERED_RING: u32 = 1 << 4;

const IORING_FEAT_SINGLE_MMAP: u32 = 1 << 0;
const IORING_FEAT_MIN_TIMEOUT: u32 = 1 << 15;

const IORING_REGISTER_FILES2: u32 = 13;
const IORING_REGISTER_RING_FDS: u32 = 20;
const IORING_REGISTER_PBUF_RING: u32 = 22;
const IORING_RSRC_REGISTER_SPARSE: u32 = 1 << 0;

pub const PBUF_BGID: u16 = 1;
pub const PBUF_SIZE: usize = 2048;

pub const PG_PBUF_BGID: u16 = 2;
pub const PG_PBUF_SIZE: usize = 32768;
pub const PG_PBUF_COUNT: usize = 256;

static ZERO_OPTVAL: i32 = 0;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct IoUringSqe {
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
    pub file_index: i32,
    pub addr3: u64,
    pub pad2: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct IoUringCqe {
    pub user_data: u64,
    pub res: i32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct IoSqringOffsets {
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
#[derive(Clone, Copy, Default)]
struct IoCqringOffsets {
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
#[derive(Clone, Copy, Default)]
struct IoUringParams {
    sq_entries: u32,
    cq_entries: u32,
    flags: u32,
    sq_thread_cpu: u32,
    sq_thread_idle: u32,
    features: u32,
    wq_fd: u32,
    resv: [u32; 3],
    sq_off: IoSqringOffsets,
    cq_off: IoCqringOffsets,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct IoUringBuf {
    addr: u64,
    len: u32,
    bid: u16,
    resv: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct IoUringBufReg {
    ring_addr: u64,
    ring_entries: u32,
    bgid: u16,
    flags: u16,
    min_left: u32,
    resv: [u32; 5],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct IoUringRsrcRegister {
    nr: u32,
    flags: u32,
    resv2: u64,
    data: u64,
    tags: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct IoUringRsrcUpdate {
    offset: u32,
    resv: u32,
    data: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct IoUringGeteventsArg {
    sigmask: u64,
    sigmask_sz: u32,
    min_wait_usec: u32,
    ts: u64,
}

const _: () = assert!(std::mem::size_of::<IoUringSqe>() == 64);
const _: () = assert!(std::mem::size_of::<IoUringCqe>() == 16);
const _: () = assert!(std::mem::size_of::<IoUringBuf>() == 16);
const _: () = assert!(std::mem::size_of::<IoUringBufReg>() == 40);
const _: () = assert!(std::mem::size_of::<IoUringRsrcRegister>() == 32);
const _: () = assert!(std::mem::size_of::<IoUringRsrcUpdate>() == 16);
const _: () = assert!(std::mem::size_of::<IoUringGeteventsArg>() == 24);

unsafe fn sys_mmap(addr: *mut c_void, len: usize, prot: i32, flags: i32, fd: i32, off: i64) -> *mut u8 {
    let rc = syscall(SYS_MMAP, addr, len, prot, flags, fd, off);
    if rc < 0 && rc >= -4095 {
        std::ptr::null_mut()
    } else {
        rc as usize as *mut u8
    }
}

unsafe fn sys_munmap(addr: *mut u8, len: usize) {
    if !addr.is_null() && len > 0 {
        let _ = syscall(SYS_MUNMAP, addr as *mut c_void, len);
    }
}

pub struct Uring {
    pub ring_fd: i32,
    enter_fd: i32,
    enter_flags_base: u32,
    no_sqarray: bool,
    pub use_direct_fds: bool,
    pub direct_fds_cap: usize,
    pub has_pbuf: bool,
    pub has_pg_pbuf: bool,
    pub has_pg_recv_bundle: bool,
    pub has_min_timeout: bool,

    sq_head: *const u32,
    sq_tail: *mut u32,
    sq_mask: u32,
    sq_entries: u32,
    sq_array: *mut u32,
    sqes: *mut IoUringSqe,
    local_sq_tail: u32,

    cq_head: *mut u32,
    cq_tail: *const u32,
    cq_mask: u32,
    cqes: *const IoUringCqe,

    pbuf_ring: *mut IoUringBuf,
    pbuf_tail_ptr: *mut u16,
    pbuf_tail: u16,
    pbuf_mask: u16,
    pbuf_mem: *mut u8,

    pg_pbuf_ring: *mut IoUringBuf,
    pg_pbuf_tail_ptr: *mut u16,
    pg_pbuf_tail: u16,
    pg_pbuf_mask: u16,
    pg_pbuf_mem: *mut u8,
}

impl Uring {
    pub fn new(
        sq_want: u32,
        cq_want: u32,
        pbuf_want: usize,
        max_direct_fds: usize,
        want_direct_fds: bool,
    ) -> Result<Self, String> {
        let flag_sets = [
            IORING_SETUP_CQSIZE
                | IORING_SETUP_CLAMP
                | IORING_SETUP_SUBMIT_ALL
                | IORING_SETUP_COOP_TASKRUN
                | IORING_SETUP_SINGLE_ISSUER
                | IORING_SETUP_DEFER_TASKRUN
                | IORING_SETUP_NO_SQARRAY,
            IORING_SETUP_CQSIZE
                | IORING_SETUP_CLAMP
                | IORING_SETUP_SUBMIT_ALL
                | IORING_SETUP_COOP_TASKRUN
                | IORING_SETUP_SINGLE_ISSUER
                | IORING_SETUP_DEFER_TASKRUN,
            IORING_SETUP_CQSIZE
                | IORING_SETUP_CLAMP
                | IORING_SETUP_SUBMIT_ALL
                | IORING_SETUP_COOP_TASKRUN
                | IORING_SETUP_SINGLE_ISSUER,
            IORING_SETUP_CQSIZE | IORING_SETUP_CLAMP,
        ];

        let size_tiers: [(u32, u32); 5] = [
            (sq_want, cq_want),
            (4096, 16384),
            (4096, 8192),
            (2048, 4096),
            (1024, 2048),
        ];

        let mut params = IoUringParams::default();
        let mut ring_fd = -1i32;
        let mut chosen_flags = 0u32;
        let mut last_err = 0i32;

        'outer: for &(sq_try, cq_try) in &size_tiers {
            if sq_try > sq_want || cq_try > cq_want {
                if !(sq_try == sq_want && cq_try == cq_want) {
                    continue;
                }
            }
            for &fl in &flag_sets {
                params = IoUringParams::default();
                params.cq_entries = cq_try;
                params.flags = fl;
                let rc = unsafe {
                    syscall(
                        SYS_IO_URING_SETUP,
                        sq_try as usize,
                        &mut params as *mut IoUringParams,
                    )
                };
                if rc >= 0 {
                    ring_fd = rc as i32;
                    chosen_flags = fl;
                    break 'outer;
                }
                last_err = errno();
                if last_err == 12 {
                    break;
                }
            }
        }
        if ring_fd < 0 {
            return Err(format!("io_uring_setup failed errno={}", last_err));
        }

        let no_sqarray = (chosen_flags & IORING_SETUP_NO_SQARRAY) != 0;
        let has_min_timeout = (params.features & IORING_FEAT_MIN_TIMEOUT) != 0;
        let sq_ring_sz = if no_sqarray {
            (params.sq_off.array as usize).max(64)
        } else {
            (params.sq_off.array as usize) + (params.sq_entries as usize) * std::mem::size_of::<u32>()
        };
        let cq_ring_sz = (params.cq_off.cqes as usize)
            + (params.cq_entries as usize) * std::mem::size_of::<IoUringCqe>();
        let single_mmap = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
        let map_sz = if single_mmap {
            sq_ring_sz.max(cq_ring_sz)
        } else {
            sq_ring_sz
        };

        let sq_ptr = unsafe {
            sys_mmap(
                std::ptr::null_mut(),
                map_sz,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                ring_fd,
                IORING_OFF_SQ_RING,
            )
        };
        if sq_ptr.is_null() {
            unsafe {
                close(ring_fd);
            }
            return Err("mmap sq_ring failed".to_string());
        }

        let cq_ptr = if single_mmap {
            sq_ptr
        } else {
            let p = unsafe {
                sys_mmap(
                    std::ptr::null_mut(),
                    cq_ring_sz,
                    PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE,
                    ring_fd,
                    IORING_OFF_CQ_RING,
                )
            };
            if p.is_null() {
                unsafe {
                    sys_munmap(sq_ptr, map_sz);
                    close(ring_fd);
                }
                return Err("mmap cq_ring failed".to_string());
            }
            p
        };

        let sqes_sz = (params.sq_entries as usize) * std::mem::size_of::<IoUringSqe>();
        let sqes_ptr = unsafe {
            sys_mmap(
                std::ptr::null_mut(),
                sqes_sz,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                ring_fd,
                IORING_OFF_SQES,
            )
        };
        if sqes_ptr.is_null() {
            unsafe {
                if !single_mmap {
                    sys_munmap(cq_ptr, cq_ring_sz);
                }
                sys_munmap(sq_ptr, map_sz);
                close(ring_fd);
            }
            return Err("mmap sqes failed".to_string());
        }

        let sq_array = if no_sqarray {
            std::ptr::null_mut()
        } else {
            let arr = unsafe { sq_ptr.add(params.sq_off.array as usize) as *mut u32 };
            for i in 0..(params.sq_entries as usize) {
                unsafe {
                    *arr.add(i) = i as u32;
                }
            }
            arr
        };

        let mut up = IoUringRsrcUpdate {
            offset: u32::MAX,
            resv: 0,
            data: ring_fd as u64,
        };
        let reg_ring_rc = unsafe {
            syscall(
                SYS_IO_URING_REGISTER,
                ring_fd as usize,
                IORING_REGISTER_RING_FDS as usize,
                &mut up as *mut IoUringRsrcUpdate,
                1usize,
            )
        };
        let (enter_fd, enter_flags_base) = if reg_ring_rc == 1 {
            (up.offset as i32, IORING_ENTER_REGISTERED_RING)
        } else {
            (ring_fd, 0)
        };

        let mut has_pbuf = false;
        let mut pbuf_ring: *mut IoUringBuf = std::ptr::null_mut();
        let mut pbuf_mem: *mut u8 = std::ptr::null_mut();
        let mut pbuf_tail_ptr = std::ptr::null_mut();
        let mut pbuf_tail = 0u16;
        let mut pbuf_mask = 0u16;

        let pbuf_tiers: [usize; 5] = [pbuf_want, 8192, 4096, 2048, 1024];
        let mut prev_cnt = 0usize;
        for &pbuf_cnt in &pbuf_tiers {
            if pbuf_cnt == 0 || pbuf_cnt > pbuf_want || pbuf_cnt == prev_cnt || !pbuf_cnt.is_power_of_two() {
                continue;
            }
            prev_cnt = pbuf_cnt;
            let pbuf_ring_bytes = pbuf_cnt * std::mem::size_of::<IoUringBuf>();
            let cand_ring = unsafe {
                sys_mmap(
                    std::ptr::null_mut(),
                    pbuf_ring_bytes,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                    -1,
                    0,
                )
            } as *mut IoUringBuf;
            let pbuf_mem_bytes = pbuf_cnt * PBUF_SIZE;
            let cand_mem = unsafe {
                sys_mmap(
                    std::ptr::null_mut(),
                    pbuf_mem_bytes,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                    -1,
                    0,
                )
            };
            if cand_ring.is_null() || cand_mem.is_null() {
                unsafe {
                    sys_munmap(cand_ring as *mut u8, pbuf_ring_bytes);
                    sys_munmap(cand_mem, pbuf_mem_bytes);
                }
                continue;
            }
            unsafe {
                let _ = syscall(SYS_MADVISE, cand_mem, pbuf_mem_bytes, MADV_NOHUGEPAGE);
            }
            let mut reg_ok = false;
            for &min_left_try in &[0u32, 1u32] {
                let reg = IoUringBufReg {
                    ring_addr: cand_ring as usize as u64,
                    ring_entries: pbuf_cnt as u32,
                    bgid: PBUF_BGID,
                    flags: 0,
                    min_left: min_left_try,
                    resv: [0; 5],
                };
                let reg_rc = unsafe {
                    syscall(
                        SYS_IO_URING_REGISTER,
                        ring_fd as usize,
                        IORING_REGISTER_PBUF_RING as usize,
                        &reg as *const IoUringBufReg,
                        1usize,
                    )
                };
                if reg_rc >= 0 {
                    reg_ok = true;
                    break;
                }
            }
            if reg_ok {
                has_pbuf = true;
                pbuf_ring = cand_ring;
                pbuf_mem = cand_mem;
                pbuf_mask = (pbuf_cnt - 1) as u16;
                pbuf_tail_ptr = unsafe { (pbuf_ring as *mut u8).add(14) as *mut u16 };
                for bid in 0..(pbuf_cnt as u16) {
                    unsafe {
                        let entry = &mut *pbuf_ring.add((bid & pbuf_mask) as usize);
                        entry.addr = pbuf_mem.add((bid as usize) * PBUF_SIZE) as usize as u64;
                        entry.len = PBUF_SIZE as u32;
                        entry.bid = bid;
                    }
                }
                fence(Ordering::Release);
                pbuf_tail = pbuf_cnt as u16;
                unsafe {
                    std::ptr::write_volatile(pbuf_tail_ptr, pbuf_tail);
                }
                break;
            } else {
                unsafe {
                    sys_munmap(cand_ring as *mut u8, pbuf_ring_bytes);
                    sys_munmap(cand_mem, pbuf_mem_bytes);
                }
            }
        }

        let mut use_direct_fds = false;
        let mut direct_fds_cap = 0usize;
        if want_direct_fds && has_pbuf && max_direct_fds > 0 {
            let target_nr = max_direct_fds.min(1_048_576) as u32;
            let rsrc = IoUringRsrcRegister {
                nr: target_nr,
                flags: IORING_RSRC_REGISTER_SPARSE,
                resv2: 0,
                data: 0,
                tags: 0,
            };
            let rc = unsafe {
                syscall(
                    SYS_IO_URING_REGISTER,
                    ring_fd as usize,
                    IORING_REGISTER_FILES2 as usize,
                    &rsrc as *const IoUringRsrcRegister,
                    std::mem::size_of::<IoUringRsrcRegister>(),
                )
            };
            if rc >= 0 {
                use_direct_fds = true;
                direct_fds_cap = target_nr as usize;
            }
        }

        let mut has_pg_pbuf = false;
        let mut pg_pbuf_ring: *mut IoUringBuf = std::ptr::null_mut();
        let mut pg_pbuf_mem: *mut u8 = std::ptr::null_mut();
        let mut pg_pbuf_tail_ptr: *mut u16 = std::ptr::null_mut();
        let mut pg_pbuf_tail = 0u16;
        let mut pg_pbuf_mask = 0u16;

        if has_pbuf {
            let pg_ring_bytes = (PG_PBUF_COUNT * std::mem::size_of::<IoUringBuf>()).max(4096);
            let cand_ring = unsafe {
                sys_mmap(
                    std::ptr::null_mut(),
                    pg_ring_bytes,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                    -1,
                    0,
                )
            } as *mut IoUringBuf;
            let pg_mem_bytes = PG_PBUF_COUNT * PG_PBUF_SIZE;
            let cand_mem = unsafe {
                sys_mmap(
                    std::ptr::null_mut(),
                    pg_mem_bytes,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                    -1,
                    0,
                )
            };
            if !cand_ring.is_null() && !cand_mem.is_null() {
                unsafe {
                    let _ = syscall(SYS_MADVISE, cand_mem, pg_mem_bytes, MADV_NOHUGEPAGE);
                }
                let mut reg_rc = -22i64;
                for &min_left_try in &[0u32, 1u32] {
                    let reg = IoUringBufReg {
                        ring_addr: cand_ring as usize as u64,
                        ring_entries: PG_PBUF_COUNT as u32,
                        bgid: PG_PBUF_BGID,
                        flags: 0,
                        min_left: min_left_try,
                        resv: [0; 5],
                    };
                    reg_rc = unsafe {
                        syscall(
                            SYS_IO_URING_REGISTER,
                            ring_fd as usize,
                            IORING_REGISTER_PBUF_RING as usize,
                            &reg as *const IoUringBufReg,
                            1usize,
                        )
                    };
                    if reg_rc >= 0 {
                        break;
                    }
                }
                if reg_rc >= 0 {
                    has_pg_pbuf = true;
                    pg_pbuf_ring = cand_ring;
                    pg_pbuf_mem = cand_mem;
                    pg_pbuf_mask = (PG_PBUF_COUNT - 1) as u16;
                    pg_pbuf_tail_ptr = unsafe { (pg_pbuf_ring as *mut u8).add(14) as *mut u16 };
                    for bid in 0..(PG_PBUF_COUNT as u16) {
                        unsafe {
                            let entry = &mut *pg_pbuf_ring.add((bid & pg_pbuf_mask) as usize);
                            entry.addr = pg_pbuf_mem.add((bid as usize) * PG_PBUF_SIZE) as usize as u64;
                            entry.len = PG_PBUF_SIZE as u32;
                            entry.bid = bid;
                        }
                    }
                    fence(Ordering::Release);
                    pg_pbuf_tail = PG_PBUF_COUNT as u16;
                    unsafe {
                        std::ptr::write_volatile(pg_pbuf_tail_ptr, pg_pbuf_tail);
                    }
                } else {
                    unsafe {
                        sys_munmap(cand_ring as *mut u8, pg_ring_bytes);
                        sys_munmap(cand_mem, pg_mem_bytes);
                    }
                }
            } else {
                unsafe {
                    sys_munmap(cand_ring as *mut u8, pg_ring_bytes);
                    sys_munmap(cand_mem, pg_mem_bytes);
                }
            }
        }

        let sq_tail = unsafe { sq_ptr.add(params.sq_off.tail as usize) as *mut u32 };
        let init_tail = unsafe { std::ptr::read_volatile(sq_tail) };

        let mut ring = Self {
            ring_fd,
            enter_fd,
            enter_flags_base,
            no_sqarray,
            use_direct_fds,
            direct_fds_cap,
            has_pbuf,
            has_pg_pbuf,
            has_pg_recv_bundle: false,
            has_min_timeout,
            sq_head: unsafe { sq_ptr.add(params.sq_off.head as usize) as *const u32 },
            sq_tail,
            sq_mask: unsafe { *(sq_ptr.add(params.sq_off.ring_mask as usize) as *const u32) },
            sq_entries: params.sq_entries,
            sq_array,
            sqes: sqes_ptr as *mut IoUringSqe,
            local_sq_tail: init_tail,
            cq_head: unsafe { cq_ptr.add(params.cq_off.head as usize) as *mut u32 },
            cq_tail: unsafe { cq_ptr.add(params.cq_off.tail as usize) as *const u32 },
            cq_mask: unsafe { *(cq_ptr.add(params.cq_off.ring_mask as usize) as *const u32) },
            cqes: unsafe { cq_ptr.add(params.cq_off.cqes as usize) as *const IoUringCqe },
            pbuf_ring,
            pbuf_tail_ptr,
            pbuf_tail,
            pbuf_mask,
            pbuf_mem,
            pg_pbuf_ring,
            pg_pbuf_tail_ptr,
            pg_pbuf_tail,
            pg_pbuf_mask,
            pg_pbuf_mem,
        };
        if ring.has_pg_pbuf
            && std::env::var("FEED_URING_BUNDLE")
                .map(|v| v != "0")
                .unwrap_or(true)
        {
            ring.probe_pg_recv_bundle();
        }
        Ok(ring)
    }

    fn probe_pg_recv_bundle(&mut self) {
        let mut sv = [-1i32; 2];
        if unsafe { socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv.as_mut_ptr()) }
            != 0
        {
            return;
        }
        let probe_byte: u8 = 0x5A;
        if unsafe { write(sv[1], &probe_byte as *const u8 as *const c_void, 1) } != 1 {
            unsafe {
                close(sv[0]);
                close(sv[1]);
            }
            return;
        }
        const PROBE_UD: u64 = 0x7FFF_FFFF_FFFF_0001;
        const PROBE_CANCEL_UD: u64 = 0x7FFF_FFFF_FFFF_0002;
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_RECV;
            (*sqe).flags = IOSQE_BUFFER_SELECT;
            (*sqe).ioprio = IORING_RECV_MULTISHOT | IORING_RECVSEND_BUNDLE;
            (*sqe).fd = sv[0];
            (*sqe).len = 0;
            (*sqe).buf_index = PG_PBUF_BGID;
            (*sqe).user_data = PROBE_UD;
        }
        let _ = self.submit_and_wait_ns(5_000_000);
        let (mut h, t) = self.cq_snapshot();
        let mut ok = false;
        let mut still_armed = false;
        let mut recycled = false;
        while h != t {
            let c = self.cqe_at(h);
            if c.user_data == PROBE_UD {
                if (c.flags & IORING_CQE_F_BUFFER) != 0 {
                    let bid = (c.flags >> IORING_CQE_BUFFER_SHIFT) as u16;
                    self.recycle_pg_pbuf_span(bid, c.res);
                    recycled = true;
                }
                if c.res == 1 && (c.flags & IORING_CQE_F_BUFFER) != 0 {
                    ok = true;
                }
                if (c.flags & IORING_CQE_F_MORE) != 0 {
                    still_armed = true;
                }
            }
            h = h.wrapping_add(1);
        }
        self.advance_cq(h);

        if still_armed {
            let csqe = self.next_sqe();
            unsafe {
                (*csqe).opcode = IORING_OP_ASYNC_CANCEL;
                (*csqe).fd = -1;
                (*csqe).addr = PROBE_UD;
                (*csqe).user_data = PROBE_CANCEL_UD;
            }
            let _ = self.submit_and_wait_ns(5_000_000);
            let (mut h2, t2) = self.cq_snapshot();
            while h2 != t2 {
                let c = self.cqe_at(h2);
                if c.user_data == PROBE_UD && (c.flags & IORING_CQE_F_BUFFER) != 0 {
                    let bid = (c.flags >> IORING_CQE_BUFFER_SHIFT) as u16;
                    self.recycle_pg_pbuf_span(bid, c.res);
                    recycled = true;
                }
                h2 = h2.wrapping_add(1);
            }
            self.advance_cq(h2);
        }

        if recycled {
            self.commit_pg_pbufs();
        }
        unsafe {
            close(sv[0]);
            close(sv[1]);
        }
        self.has_pg_recv_bundle = ok;
    }

    #[inline(always)]
    pub fn client_fixed_flag(&self) -> u8 {
        if self.use_direct_fds {
            IOSQE_FIXED_FILE
        } else {
            0
        }
    }

    #[inline(always)]
    pub fn pbuf_raw(&self, bid: u16, len: usize) -> (*const u8, usize) {
        let clamped = len.min(PBUF_SIZE);
        let ptr = unsafe { self.pbuf_mem.add((bid as usize) * PBUF_SIZE) as *const u8 };
        (ptr, clamped)
    }

    #[inline(always)]
    pub fn recycle_pbuf(&mut self, bid: u16) {
        if !self.has_pbuf {
            return;
        }
        let idx = (self.pbuf_tail & self.pbuf_mask) as usize;
        unsafe {
            let entry = &mut *self.pbuf_ring.add(idx);
            entry.addr = self.pbuf_mem.add((bid as usize) * PBUF_SIZE) as usize as u64;
            entry.len = PBUF_SIZE as u32;
            entry.bid = bid;
        }
        self.pbuf_tail = self.pbuf_tail.wrapping_add(1);
    }

    #[inline(always)]
    pub fn commit_pbufs(&mut self) {
        if !self.has_pbuf {
            return;
        }
        fence(Ordering::Release);
        unsafe {
            std::ptr::write_volatile(self.pbuf_tail_ptr, self.pbuf_tail);
        }
    }

    #[inline(always)]
    pub fn pg_pbuf_spans(
        &self,
        bid: u16,
        total_len: usize,
    ) -> ((*const u8, usize), (*const u8, usize)) {
        let ring_bytes = PG_PBUF_COUNT * PG_PBUF_SIZE;
        let clamped = total_len.min(ring_bytes);
        let start_off = ((bid & self.pg_pbuf_mask) as usize) * PG_PBUF_SIZE;
        let first_len = clamped.min(ring_bytes - start_off);
        let second_len = clamped - first_len;
        let p0 = unsafe { self.pg_pbuf_mem.add(start_off) as *const u8 };
        let p1 = self.pg_pbuf_mem as *const u8;
        ((p0, first_len), (p1, second_len))
    }

    #[inline(always)]
    pub fn recycle_pg_pbuf(&mut self, bid: u16) {
        if !self.has_pg_pbuf {
            return;
        }
        let idx = (self.pg_pbuf_tail & self.pg_pbuf_mask) as usize;
        unsafe {
            let entry = &mut *self.pg_pbuf_ring.add(idx);
            entry.addr = self.pg_pbuf_mem.add((bid as usize) * PG_PBUF_SIZE) as usize as u64;
            entry.len = PG_PBUF_SIZE as u32;
            entry.bid = bid;
        }
        self.pg_pbuf_tail = self.pg_pbuf_tail.wrapping_add(1);
    }

    #[inline(always)]
    pub fn recycle_pg_pbuf_span(&mut self, bid: u16, res: i32) {
        if !self.has_pg_pbuf {
            return;
        }
        let count = if res > 0 {
            let clamped = (res as usize).min(PG_PBUF_COUNT * PG_PBUF_SIZE);
            (clamped + PG_PBUF_SIZE - 1) / PG_PBUF_SIZE
        } else {
            1
        };
        for k in 0..count {
            self.recycle_pg_pbuf((bid.wrapping_add(k as u16)) & self.pg_pbuf_mask);
        }
    }

    #[inline(always)]
    pub fn commit_pg_pbufs(&mut self) {
        if !self.has_pg_pbuf {
            return;
        }
        fence(Ordering::Release);
        unsafe {
            std::ptr::write_volatile(self.pg_pbuf_tail_ptr, self.pg_pbuf_tail);
        }
    }

    #[inline(always)]
    pub fn ensure_sqe_space(&mut self, needed: u32) {
        let head = unsafe { std::ptr::read_volatile(self.sq_head) };
        let used = self.local_sq_tail.wrapping_sub(head);
        if self.sq_entries.saturating_sub(used) < needed {
            let _ = self.submit_only();
            let head2 = unsafe { std::ptr::read_volatile(self.sq_head) };
            if self.sq_entries.saturating_sub(self.local_sq_tail.wrapping_sub(head2)) < needed {
                let _ = self.submit_and_getevents();
            }
        }
    }

    #[inline(always)]
    pub fn next_sqe(&mut self) -> *mut IoUringSqe {
        self.ensure_sqe_space(1);
        let idx = (self.local_sq_tail & self.sq_mask) as usize;
        self.local_sq_tail = self.local_sq_tail.wrapping_add(1);
        unsafe {
            if !self.no_sqarray {
                *self.sq_array.add(idx) = idx as u32;
            }
            let sqe = self.sqes.add(idx);
            std::ptr::write_bytes(sqe, 0, 1);
            sqe
        }
    }

    #[inline(always)]
    pub fn prep_multishot_accept(&mut self, lfd: i32, accept_flags: u32, user_data: u64) {
        let use_direct = self.use_direct_fds;
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_ACCEPT;
            (*sqe).ioprio = IORING_ACCEPT_MULTISHOT;
            (*sqe).fd = lfd;
            if use_direct {
                (*sqe).op_flags = accept_flags & !(SOCK_CLOEXEC as u32);
                (*sqe).file_index = IORING_FILE_INDEX_ALLOC;
            } else {
                (*sqe).op_flags = accept_flags;
            }
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_poll_add(&mut self, fd: i32, poll_mask: u32, multishot: bool, user_data: u64) {
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_POLL_ADD;
            (*sqe).fd = fd;
            (*sqe).len = if multishot { IORING_POLL_ADD_MULTI } else { 0 };
            (*sqe).op_flags = poll_mask;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_client_poll_add(&mut self, fd: i32, poll_mask: u32, multishot: bool, user_data: u64) {
        let fixed = self.client_fixed_flag();
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_POLL_ADD;
            (*sqe).flags = fixed;
            (*sqe).fd = fd;
            (*sqe).len = if multishot { IORING_POLL_ADD_MULTI } else { 0 };
            (*sqe).op_flags = poll_mask;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_poll_remove(&mut self, target_user_data: u64, user_data: u64) {
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_POLL_REMOVE;
            (*sqe).flags = IOSQE_CQE_SKIP_SUCCESS;
            (*sqe).fd = -1;
            (*sqe).addr = target_user_data;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_async_cancel(&mut self, target_user_data: u64, user_data: u64) {
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_ASYNC_CANCEL;
            (*sqe).flags = IOSQE_CQE_SKIP_SUCCESS;
            (*sqe).fd = -1;
            (*sqe).addr = target_user_data;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_multishot_recv_pbuf(&mut self, fd: i32, user_data: u64) {
        let fixed = self.client_fixed_flag();
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_RECV;
            (*sqe).flags = IOSQE_BUFFER_SELECT | fixed;
            (*sqe).ioprio = IORING_RECV_MULTISHOT;
            (*sqe).fd = fd;
            (*sqe).len = 0;
            (*sqe).buf_index = PBUF_BGID;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_pg_multishot_recv(&mut self, fd: i32, user_data: u64) {
        let ioprio = if self.has_pg_recv_bundle {
            IORING_RECV_MULTISHOT | IORING_RECVSEND_BUNDLE
        } else {
            IORING_RECV_MULTISHOT
        };
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_RECV;
            (*sqe).flags = IOSQE_BUFFER_SELECT;
            (*sqe).ioprio = ioprio;
            (*sqe).fd = fd;
            (*sqe).len = 0;
            (*sqe).buf_index = PG_PBUF_BGID;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_pg_send(
        &mut self,
        fd: i32,
        ptr: *const u8,
        len: usize,
        msg_flags: u32,
        user_data: u64,
    ) {
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_SEND;
            (*sqe).fd = fd;
            (*sqe).addr = ptr as usize as u64;
            (*sqe).len = len as u32;
            (*sqe).op_flags = msg_flags;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_linked_no_quickack(&mut self, fd: i32, user_data: u64) {
        let fixed = self.client_fixed_flag();
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_URING_CMD;
            (*sqe).flags = fixed | IOSQE_IO_LINK | IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
            (*sqe).fd = fd;
            (*sqe).off = SOCKET_URING_OP_SETSOCKOPT;
            (*sqe).addr = (IPPROTO_TCP as u32 as u64) | ((TCP_QUICKACK as u32 as u64) << 32);
            (*sqe).file_index = std::mem::size_of::<i32>() as i32;
            (*sqe).addr3 = &ZERO_OPTVAL as *const i32 as usize as u64;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_client_send(
        &mut self,
        fd: i32,
        ptr: *const u8,
        len: usize,
        msg_flags: u32,
        user_data: u64,
    ) {
        let fixed = self.client_fixed_flag();
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_SEND;
            (*sqe).flags = fixed;
            (*sqe).fd = fd;
            (*sqe).addr = ptr as usize as u64;
            (*sqe).len = len as u32;
            (*sqe).op_flags = msg_flags;
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_client_close(&mut self, fd: i32, user_data: u64) {
        let use_direct = self.use_direct_fds;
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_CLOSE;
            (*sqe).flags = IOSQE_CQE_SKIP_SUCCESS;
            if use_direct {
                (*sqe).fd = 0;
                (*sqe).file_index = fd + 1;
            } else {
                (*sqe).fd = fd;
            }
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn prep_direct_cancel_ud_and_close(
        &mut self,
        slot: i32,
        target_ud: u64,
        ignore_ud: u64,
    ) {
        self.ensure_sqe_space(2);
        let sqe_c = self.next_sqe();
        unsafe {
            (*sqe_c).opcode = IORING_OP_ASYNC_CANCEL;
            (*sqe_c).flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
            (*sqe_c).fd = -1;
            (*sqe_c).addr = target_ud;
            (*sqe_c).user_data = ignore_ud;
        }
        let sqe_cl = self.next_sqe();
        unsafe {
            (*sqe_cl).opcode = IORING_OP_CLOSE;
            (*sqe_cl).flags = IOSQE_CQE_SKIP_SUCCESS;
            (*sqe_cl).fd = 0;
            (*sqe_cl).file_index = slot + 1;
            (*sqe_cl).user_data = ignore_ud;
        }
    }

    #[inline(always)]
    pub fn prep_direct_cancel_and_close(&mut self, slot: i32, ignore_ud: u64) {
        self.ensure_sqe_space(2);
        let sqe_c = self.next_sqe();
        unsafe {
            (*sqe_c).opcode = IORING_OP_ASYNC_CANCEL;
            (*sqe_c).flags = IOSQE_IO_HARDLINK | IOSQE_CQE_SKIP_SUCCESS;
            (*sqe_c).fd = slot;
            (*sqe_c).op_flags =
                IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_FD_FIXED | IORING_ASYNC_CANCEL_ALL;
            (*sqe_c).user_data = ignore_ud;
        }
        let sqe_cl = self.next_sqe();
        unsafe {
            (*sqe_cl).opcode = IORING_OP_CLOSE;
            (*sqe_cl).flags = IOSQE_CQE_SKIP_SUCCESS;
            (*sqe_cl).fd = 0;
            (*sqe_cl).file_index = slot + 1;
            (*sqe_cl).user_data = ignore_ud;
        }
    }

    pub fn submit_only(&mut self) -> i32 {
        let head = unsafe { std::ptr::read_volatile(self.sq_head) };
        let to_submit = self.local_sq_tail.wrapping_sub(head);
        if to_submit == 0 {
            return 0;
        }
        fence(Ordering::Release);
        unsafe {
            std::ptr::write_volatile(self.sq_tail, self.local_sq_tail);
        }
        loop {
            let rc = unsafe {
                syscall(
                    SYS_IO_URING_ENTER,
                    self.enter_fd as usize,
                    to_submit as usize,
                    0usize,
                    self.enter_flags_base as usize,
                    std::ptr::null::<c_void>(),
                    0usize,
                )
            };
            if rc >= 0 {
                return rc as i32;
            }
            if errno() != EINTR {
                return -errno();
            }
        }
    }

    #[inline(always)]
    pub fn prep_timeout(&mut self, ts: *const Timespec, multishot: bool, user_data: u64) {
        let sqe = self.next_sqe();
        unsafe {
            (*sqe).opcode = IORING_OP_TIMEOUT;
            (*sqe).fd = -1;
            (*sqe).off = 0;
            (*sqe).addr = ts as usize as u64;
            (*sqe).len = 1;
            (*sqe).op_flags = if multishot {
                IORING_TIMEOUT_MULTISHOT
            } else {
                0
            };
            (*sqe).user_data = user_data;
        }
    }

    #[inline(always)]
    pub fn submit_and_getevents(&mut self) -> i32 {
        let head = unsafe { std::ptr::read_volatile(self.sq_head) };
        let to_submit = self.local_sq_tail.wrapping_sub(head);
        if to_submit > 0 {
            fence(Ordering::Release);
            unsafe {
                std::ptr::write_volatile(self.sq_tail, self.local_sq_tail);
            }
        }
        let flags = self.enter_flags_base | IORING_ENTER_GETEVENTS;
        let rc = unsafe {
            syscall(
                SYS_IO_URING_ENTER,
                self.enter_fd as usize,
                to_submit as usize,
                0usize,
                flags as usize,
                std::ptr::null::<c_void>(),
                0usize,
            )
        };
        rc as i32
    }

    #[inline(always)]
    pub fn submit_and_wait_one_notimer(&mut self) -> i32 {
        let head = unsafe { std::ptr::read_volatile(self.sq_head) };
        let to_submit = self.local_sq_tail.wrapping_sub(head);
        if to_submit > 0 {
            fence(Ordering::Release);
            unsafe {
                std::ptr::write_volatile(self.sq_tail, self.local_sq_tail);
            }
        }
        let cq_h = unsafe { std::ptr::read_volatile(self.cq_head) };
        let cq_t = unsafe { std::ptr::read_volatile(self.cq_tail) };
        let min_complete = if cq_t != cq_h { 0usize } else { 1usize };
        let flags = self.enter_flags_base | IORING_ENTER_GETEVENTS;
        let rc = unsafe {
            syscall(
                SYS_IO_URING_ENTER,
                self.enter_fd as usize,
                to_submit as usize,
                min_complete,
                flags as usize,
                std::ptr::null::<c_void>(),
                0usize,
            )
        };
        rc as i32
    }

    #[inline(always)]
    pub fn submit_and_wait_ns(&mut self, wait_ns: u64) -> i32 {
        self.submit_and_wait_coalesced(wait_ns, 1, 0, false)
    }

    pub fn submit_and_wait_coalesced(
        &mut self,
        wait_ns: u64,
        want_min: u32,
        min_wait_usec: u32,
        in_burst: bool,
    ) -> i32 {
        let head = unsafe { std::ptr::read_volatile(self.sq_head) };
        let to_submit = self.local_sq_tail.wrapping_sub(head);
        if to_submit > 0 {
            fence(Ordering::Release);
            unsafe {
                std::ptr::write_volatile(self.sq_tail, self.local_sq_tail);
            }
        }

        let cq_h = unsafe { std::ptr::read_volatile(self.cq_head) };
        let cq_t = unsafe { std::ptr::read_volatile(self.cq_tail) };
        let ready = cq_t.wrapping_sub(cq_h);

        let (effective_want, effective_wait_ns) = if want_min > 1 && min_wait_usec > 0 {
            if self.has_min_timeout {
                (want_min, wait_ns)
            } else if in_burst {
                let burst_ns = (min_wait_usec as u64) * 1_000;
                (want_min, wait_ns.min(burst_ns))
            } else {
                (1, wait_ns)
            }
        } else {
            (want_min.min(1), wait_ns)
        };

        let min_complete = if effective_wait_ns == 0 || ready >= effective_want {
            0usize
        } else {
            (effective_want - ready) as usize
        };

        let ts = Timespec {
            tv_sec: (effective_wait_ns / 1_000_000_000) as i64,
            tv_nsec: (effective_wait_ns % 1_000_000_000) as i64,
        };
        let ext = IoUringGeteventsArg {
            sigmask: 0,
            sigmask_sz: 8,
            min_wait_usec: if min_complete > 1 && self.has_min_timeout {
                (effective_wait_ns / 1_000).min(min_wait_usec as u64) as u32
            } else {
                0
            },
            ts: if min_complete > 0 {
                &ts as *const Timespec as usize as u64
            } else {
                0
            },
        };
        let flags = self.enter_flags_base
            | IORING_ENTER_GETEVENTS
            | if min_complete > 0 {
                IORING_ENTER_EXT_ARG
            } else {
                0
            };
        let arg_ptr = if min_complete > 0 {
            &ext as *const IoUringGeteventsArg as *const c_void
        } else {
            std::ptr::null()
        };
        let arg_sz = if min_complete > 0 {
            std::mem::size_of::<IoUringGeteventsArg>()
        } else {
            0
        };

        let rc = unsafe {
            syscall(
                SYS_IO_URING_ENTER,
                self.enter_fd as usize,
                to_submit as usize,
                min_complete,
                flags as usize,
                arg_ptr,
                arg_sz,
            )
        };
        rc as i32
    }

    #[inline(always)]
    pub fn cq_snapshot(&self) -> (u32, u32) {
        let h = unsafe { std::ptr::read_volatile(self.cq_head) };
        let t = unsafe { std::ptr::read_volatile(self.cq_tail) };
        fence(Ordering::Acquire);
        (h, t)
    }

    #[inline(always)]
    pub fn cqe_at(&self, seq: u32) -> IoUringCqe {
        unsafe { *self.cqes.add((seq & self.cq_mask) as usize) }
    }

    #[inline(always)]
    pub fn advance_cq(&mut self, new_head: u32) {
        fence(Ordering::Release);
        unsafe {
            std::ptr::write_volatile(self.cq_head, new_head);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sys::{
        bind, connect, listen, read, set_sockopt_int, socket, write, SockAddrIn, AF_INET,
        MSG_NOSIGNAL, SOCK_NONBLOCK, SOCK_STREAM, SOL_SOCKET, SO_REUSEADDR,
    };

    #[test]
    fn test_uring_init_and_nop() {
        let mut ring = Uring::new(128, 256, 1024, 4096, true).expect("Uring::new should succeed");
        let sqe = ring.next_sqe();
        unsafe {
            (*sqe).opcode = 0;
            (*sqe).user_data = 0xDEAD_BEEF_CAFE_1234;
        }
        let rc = ring.submit_and_wait_ns(50_000_000);
        assert!(rc >= 0);
        let (h, t) = ring.cq_snapshot();
        assert_ne!(h, t);
        let cqe = ring.cqe_at(h);
        assert_eq!(cqe.user_data, 0xDEAD_BEEF_CAFE_1234);
        assert_eq!(cqe.res, 0);
        ring.advance_cq(h.wrapping_add(1));
        unsafe {
            close(ring.ring_fd);
        }
    }

    #[test]
    fn test_uring_direct_accept_multishot_recv_and_linked_setsockopt() {
        let mut ring = Uring::new(128, 256, 1024, 4096, true).expect("Uring::new should succeed");
        if !ring.has_pbuf {
            unsafe {
                close(ring.ring_fd);
            }
            return;
        }

        let lfd = unsafe { socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) };
        assert!(lfd >= 0);
        set_sockopt_int(lfd, SOL_SOCKET, SO_REUSEADDR, 1);
        let mut addr = SockAddrIn {
            sin_family: AF_INET as u16,
            sin_port: 0,
            sin_addr: u32::from_be_bytes([127, 0, 0, 1]).to_be(),
            sin_zero: [0; 8],
        };
        assert_eq!(
            unsafe { bind(lfd, &addr, std::mem::size_of::<SockAddrIn>() as u32) },
            0
        );
        unsafe {
            extern "C" {
                fn getsockname(fd: i32, addr: *mut SockAddrIn, len: *mut u32) -> i32;
            }
            let mut alen = std::mem::size_of::<SockAddrIn>() as u32;
            assert_eq!(getsockname(lfd, &mut addr, &mut alen), 0);
            assert_eq!(listen(lfd, 16), 0);
        }

        ring.prep_multishot_accept(lfd, (SOCK_NONBLOCK | SOCK_CLOEXEC) as u32, 1001);
        let _ = ring.submit_only();

        let cli = unsafe { socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0) };
        assert!(cli >= 0);
        assert_eq!(
            unsafe { connect(cli, &addr, std::mem::size_of::<SockAddrIn>() as u32) },
            0
        );
        let req = b"GET /feed HTTP/1.1\r\nHost: localhost\r\n\r\n";
        assert_eq!(
            unsafe { write(cli, req.as_ptr() as *const c_void, req.len()) },
            req.len() as isize
        );

        let _ = ring.submit_and_wait_ns(50_000_000);
        let (mut h, t) = ring.cq_snapshot();
        assert_ne!(h, t);
        let acc_cqe = ring.cqe_at(h);
        h = h.wrapping_add(1);
        ring.advance_cq(h);
        assert_eq!(acc_cqe.user_data, 1001);
        assert!(acc_cqe.res >= 0);
        let cfd = acc_cqe.res;

        ring.prep_multishot_recv_pbuf(cfd, 2002);
        let _ = ring.submit_and_wait_ns(50_000_000);
        let (mut h, t) = ring.cq_snapshot();
        assert_ne!(h, t);
        let rx_cqe = ring.cqe_at(h);
        h = h.wrapping_add(1);
        ring.advance_cq(h);
        assert_eq!(rx_cqe.user_data, 2002);
        assert_eq!(rx_cqe.res as usize, req.len());
        assert_ne!(rx_cqe.flags & IORING_CQE_F_BUFFER, 0);
        let bid = (rx_cqe.flags >> IORING_CQE_BUFFER_SHIFT) as u16;
        let (p_ptr, p_len) = ring.pbuf_raw(bid, rx_cqe.res as usize);
        let got_slice = unsafe { std::slice::from_raw_parts(p_ptr, p_len) };
        assert_eq!(got_slice, req);
        ring.recycle_pbuf(bid);
        ring.commit_pbufs();

        let resp = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
        ring.ensure_sqe_space(2);
        ring.prep_linked_no_quickack(cfd, 2999);
        ring.prep_client_send(cfd, resp.as_ptr(), resp.len(), MSG_NOSIGNAL as u32, 3003);
        let _ = ring.submit_and_wait_ns(50_000_000);
        let (mut h, t) = ring.cq_snapshot();
        let mut saw_send = false;
        while h != t {
            let c = ring.cqe_at(h);
            if c.user_data == 3003 {
                assert_eq!(c.res as usize, resp.len());
                saw_send = true;
            }
            h = h.wrapping_add(1);
        }
        ring.advance_cq(h);
        assert!(saw_send);

        let mut out = [0u8; 64];
        let rn = unsafe { read(cli, out.as_mut_ptr() as *mut c_void, out.len()) };
        assert_eq!(rn as usize, resp.len());
        assert_eq!(&out[..resp.len()], resp);

        if ring.use_direct_fds {
            ring.prep_direct_cancel_ud_and_close(cfd, 2002, 9999);
        } else {
            ring.prep_async_cancel(2002, 9999);
            ring.prep_client_close(cfd, 9999);
        }
        let _ = ring.submit_and_wait_ns(50_000_000);

        unsafe {
            close(cli);
            close(lfd);
            close(ring.ring_fd);
        }
    }

    #[test]
    fn test_uring_pg_recvsend_bundle_and_wrap() {
        let mut ring = Uring::new(128, 256, 1024, 4096, true).expect("Uring::new should succeed");
        if !ring.has_pg_pbuf {
            unsafe {
                close(ring.ring_fd);
            }
            return;
        }

        let last_bid = (PG_PBUF_COUNT - 1) as u16;
        unsafe {
            std::ptr::write_bytes(
                ring.pg_pbuf_mem.add((last_bid as usize) * PG_PBUF_SIZE),
                0xA5,
                PG_PBUF_SIZE,
            );
            std::ptr::write_bytes(ring.pg_pbuf_mem, 0x5A, 4096);
        }
        let ((p0, n0), (p1, n1)) = ring.pg_pbuf_spans(last_bid, PG_PBUF_SIZE + 4096);
        assert_eq!(n0, PG_PBUF_SIZE);
        assert_eq!(n1, 4096);
        let s0 = unsafe { std::slice::from_raw_parts(p0, n0) };
        let s1 = unsafe { std::slice::from_raw_parts(p1, n1) };
        assert!(s0.iter().all(|&b| b == 0xA5));
        assert!(s1.iter().all(|&b| b == 0x5A));

        let mut sv = [-1i32; 2];
        assert_eq!(
            unsafe {
                socketpair(
                    AF_UNIX,
                    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                    0,
                    sv.as_mut_ptr(),
                )
            },
            0
        );
        let mut payload = vec![0u8; 80_000];
        for (i, b) in payload.iter_mut().enumerate() {
            *b = ((i * 31 + 17) & 0xFF) as u8;
        }
        let wn = unsafe { write(sv[1], payload.as_ptr() as *const c_void, payload.len()) };
        assert!(wn > 0);
        let expected_len = wn as usize;

        ring.prep_pg_multishot_recv(sv[0], 7701);
        let mut collected = Vec::with_capacity(expected_len);
        for _ in 0..16 {
            if collected.len() >= expected_len {
                break;
            }
            let _ = ring.submit_and_wait_ns(20_000_000);
            let (mut h, t) = ring.cq_snapshot();
            let mut recycled_any = false;
            while h != t {
                let c = ring.cqe_at(h);
                if c.user_data == 7701 && c.res > 0 && (c.flags & IORING_CQE_F_BUFFER) != 0 {
                    let bid = (c.flags >> IORING_CQE_BUFFER_SHIFT) as u16;
                    let ((rp0, rn0), (rp1, rn1)) = ring.pg_pbuf_spans(bid, c.res as usize);
                    if rn0 > 0 {
                        collected.extend_from_slice(unsafe { std::slice::from_raw_parts(rp0, rn0) });
                    }
                    if rn1 > 0 {
                        collected.extend_from_slice(unsafe { std::slice::from_raw_parts(rp1, rn1) });
                    }
                    ring.recycle_pg_pbuf_span(bid, c.res);
                    recycled_any = true;
                }
                h = h.wrapping_add(1);
            }
            ring.advance_cq(h);
            if recycled_any {
                ring.commit_pg_pbufs();
            }
        }
        assert_eq!(collected.len(), expected_len);
        assert_eq!(&collected[..], &payload[..expected_len]);

        ring.prep_async_cancel(7701, 7702);
        let _ = ring.submit_and_wait_ns(10_000_000);
        unsafe {
            close(sv[0]);
            close(sv[1]);
            close(ring.ring_fd);
        }
    }
}
