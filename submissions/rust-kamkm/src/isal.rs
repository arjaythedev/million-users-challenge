//! Bindings for the few ISA-L pieces the response path uses: stateless gzip at level 0 (igzip's
//! fastest mode, one hash lookup per position). The library is built from the pinned tarball in
//! build.sh and linked statically; the struct below mirrors `struct isal_zstream` from
//! isa-l 2.31.1's igzip_lib.h, with the internal state as an opaque tail generously larger than
//! `struct isal_zstate` (the extra bytes are never touched: init and deflate reset what they read).
use std::os::raw::c_int;
use std::ptr;

#[repr(C)]
pub struct Stream {
    next_in: *mut u8,
    avail_in: u32,
    total_in: u32,
    next_out: *mut u8,
    avail_out: u32,
    total_out: u32,
    hufftables: *mut u8,
    level: u32,
    level_buf_size: u32,
    level_buf: *mut u8,
    end_of_stream: u16,
    flush: u16,
    gzip_flag: u16,
    hist_bits: u16,
    // Opaque; on isa-l 2.31.1 x86_64 sizeof(struct isal_zstate) measures 82,320 bytes (the
    // 32 KiB-history build), so the tail is sized with margin from that measurement — an
    // undersized one is clobbered by init/deflate and blows up under load, not in the tests.
    internal_state: [u64; 10_400],
}

extern "C" {
    fn isal_deflate_stateless_init(stream: *mut Stream);
    fn isal_deflate_stateless(stream: *mut Stream) -> c_int;
}

const IGZIP_GZIP: u16 = 1;
const COMP_OK: c_int = 0;

impl Stream {
    pub fn new() -> Stream {
        Stream {
            next_in: ptr::null_mut(),
            avail_in: 0,
            total_in: 0,
            next_out: ptr::null_mut(),
            avail_out: 0,
            total_out: 0,
            hufftables: ptr::null_mut(),
            level: 0,
            level_buf_size: 0,
            level_buf: ptr::null_mut(),
            end_of_stream: 0,
            flush: 0,
            gzip_flag: 0,
            hist_bits: 0,
            internal_state: [0; 10_400],
        }
    }

    /// Output size that is always enough: a stored-block fallback is the worst case, plus the
    /// gzip header (10) and trailer (8) and a 5-byte block header per 64 KiB.
    pub fn bound(n: usize) -> usize {
        n + 5 * (n.div_ceil(65535)) + 18 + 64
    }

    /// Appends the gzip-compressed `input` to `out`. False (with `out` untouched) if the
    /// compressor reported anything but success, so the caller can send the plain body.
    pub fn gzip(&mut self, input: &[u8], out: &mut Vec<u8>) -> bool {
        let start = out.len();
        let cap = Self::bound(input.len());
        out.resize(start + cap, 0);
        unsafe {
            isal_deflate_stateless_init(self);
            self.next_in = input.as_ptr() as *mut u8;
            self.avail_in = input.len() as u32;
            self.next_out = out.as_mut_ptr().add(start);
            self.avail_out = cap as u32;
            self.gzip_flag = IGZIP_GZIP;
            self.level = 0;
            let ok = isal_deflate_stateless(self) == COMP_OK;
            let n = cap - self.avail_out as usize;
            if ok {
                out.set_len(start + n);
            } else {
                out.truncate(start);
            }
            ok
        }
    }
}
