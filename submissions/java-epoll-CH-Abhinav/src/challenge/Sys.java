package challenge;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;
import static java.lang.foreign.ValueLayout.JAVA_SHORT_UNALIGNED;

public final class Sys {

    private Sys() {}

    // =========================================================================
    // Linux x86_64 Socket & Epoll Constants
    // =========================================================================
    public static final int AF_INET          = 2;
    public static final int SOCK_STREAM      = 1;
    public static final int SOCK_NONBLOCK    = 0x800;
    public static final int SOCK_CLOEXEC     = 0x80000;

    public static final int SOL_SOCKET       = 1;
    public static final int SO_REUSEADDR     = 2;
    public static final int SO_SNDBUF        = 7;
    public static final int SO_RCVBUF        = 8;
    public static final int SO_REUSEPORT     = 15;

    public static final int IPPROTO_TCP      = 6;
    public static final int TCP_NODELAY      = 1;
    public static final int TCP_CORK         = 3;
    public static final int TCP_DEFER_ACCEPT = 9;
    public static final int TCP_QUICKACK     = 12;

    public static final int MSG_DONTWAIT     = 0x40;
    public static final int MSG_NOSIGNAL     = 0x4000;
    public static final int SEND_FLAGS       = MSG_DONTWAIT | MSG_NOSIGNAL;

    public static final int EPOLL_CLOEXEC    = 0x80000;
    public static final int EPOLL_CTL_ADD    = 1;
    public static final int EPOLL_CTL_DEL    = 2;
    public static final int EPOLL_CTL_MOD    = 3;

    public static final int EPOLLIN          = 0x001;
    public static final int EPOLLOUT         = 0x004;
    public static final int EPOLLERR         = 0x008;
    public static final int EPOLLHUP         = 0x010;
    public static final int EPOLLRDHUP       = 0x2000;
    public static final int EPOLLET          = 1 << 31;

    // sizeof(struct epoll_event) on x86_64 is 12 bytes (__attribute__((__packed__)))
    // Offset 0: uint32_t events (4 bytes)
    // Offset 4: uint64_t data   (8 bytes, unaligned)
    public static final long EPOLL_EVENT_SIZE = 12L;

    // Linux Errno Constants
    public static final int EINTR            = 4;
    public static final int EAGAIN           = 11; // Also EWOULDBLOCK
    public static final int EPIPE            = 32;
    public static final int ECONNRESET       = 104;
    public static final int EINPROGRESS      = 115;

    // Linux clock_gettime Constants (vDSO user-space fast path)
    public static final int CLOCK_REALTIME_COARSE  = 5;
    public static final int CLOCK_MONOTONIC_COARSE = 6;

    // =========================================================================
    // Intel ISA-L (libisal.so.2) Struct Offsets & Constants (x86_64 LP64 ABI)
    // =========================================================================
    // struct isal_zstream layout in <isa-l/igzip_lib.h>:
    //   0: uint8_t *next_in        (8B)
    //   8: uint32_t avail_in       (4B)
    //  12: uint32_t total_in       (4B)
    //  16: uint8_t *next_out       (8B)
    //  24: uint32_t avail_out      (4B)
    //  28: uint32_t total_out      (4B)
    //  32: void    *hufftables     (8B)
    //  40: uint32_t level          (4B)
    //  44: uint32_t level_buf_size (4B)
    //  48: uint8_t *level_buf      (8B)
    //  56: uint16_t end_of_stream  (2B)
    //  58: uint16_t flush          (2B)
    //  60: uint16_t gzip_flag      (2B)
    //  62: uint16_t hist_bits      (2B)
    //  64: struct isal_zstate internal_state (sizeof isal_zstream = 82384B)
    public static final long ISAL_ZSTREAM_ALLOC_SIZE   = 131072L; // 128 KB safe slab per worker
    public static final long ISAL_OFFSET_NEXT_IN       = 0L;
    public static final long ISAL_OFFSET_AVAIL_IN      = 8L;
    public static final long ISAL_OFFSET_TOTAL_IN      = 12L;
    public static final long ISAL_OFFSET_NEXT_OUT      = 16L;
    public static final long ISAL_OFFSET_AVAIL_OUT     = 24L;
    public static final long ISAL_OFFSET_TOTAL_OUT     = 28L;
    public static final long ISAL_OFFSET_HUFFTABLES    = 32L;
    public static final long ISAL_OFFSET_LEVEL         = 40L;
    public static final long ISAL_OFFSET_LEVEL_BUFSZ   = 44L;
    public static final long ISAL_OFFSET_LEVEL_BUF     = 48L;
    public static final long ISAL_OFFSET_END_OF_STREAM = 56L;
    public static final long ISAL_OFFSET_FLUSH         = 58L;
    public static final long ISAL_OFFSET_GZIP_FLAG     = 60L;
    public static final long ISAL_OFFSET_HIST_BITS     = 62L;

    public static final short ISAL_NO_FLUSH            = 0;
    public static final short IGZIP_DEFLATE            = 0; // Raw deflate (no header/trailer)
    public static final short IGZIP_GZIP               = 1; // 10B Gzip hdr + deflate + 8B trailer
    public static final short IGZIP_GZIP_NO_HDR        = 2; // Deflate + fused CRC32 + 8B trailer
    public static final int   ISAL_COMP_OK             = 0;

    // =========================================================================
    // FFM Downcall MethodHandles
    // =========================================================================
    private static final MethodHandle MH_ERRNO_LOCATION;
    private static final MethodHandle MH_SOCKET;
    private static final MethodHandle MH_SETSOCKOPT;
    private static final MethodHandle MH_BIND;
    private static final MethodHandle MH_LISTEN;
    private static final MethodHandle MH_ACCEPT4;
    private static final MethodHandle MH_CONNECT;
    private static final MethodHandle MH_RECV;
    private static final MethodHandle MH_SEND;
    private static final MethodHandle MH_CLOSE;

    private static final MethodHandle MH_EPOLL_CREATE1;
    private static final MethodHandle MH_EPOLL_CTL;
    private static final MethodHandle MH_EPOLL_WAIT_FAST;  // critical(false) for timeout == 0
    private static final MethodHandle MH_EPOLL_WAIT_BLOCK; // standard for timeout > 0

    private static final MethodHandle MH_SCHED_SETAFFINITY;
    private static final MethodHandle MH_CLOCK_GETTIME;

    private static final MethodHandle MH_ISAL_DEFLATE_STATELESS_INIT;
    private static final MethodHandle MH_ISAL_DEFLATE_STATELESS;
    private static final MethodHandle MH_CRC32_GZIP_REFL;

    static {
        Linker linker = Linker.nativeLinker();
        SymbolLookup libc = linker.defaultLookup();
        Linker.Option critical = Linker.Option.critical(false);

        MH_ERRNO_LOCATION = linker.downcallHandle(
            libc.find("__errno_location").orElseThrow(),
            FunctionDescriptor.of(ADDRESS),
            critical
        );
        MH_SOCKET = linker.downcallHandle(
            libc.find("socket").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT),
            critical
        );
        MH_SETSOCKOPT = linker.downcallHandle(
            libc.find("setsockopt").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT),
            critical
        );
        MH_BIND = linker.downcallHandle(
            libc.find("bind").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT),
            critical
        );
        MH_LISTEN = linker.downcallHandle(
            libc.find("listen").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT),
            critical
        );
        MH_ACCEPT4 = linker.downcallHandle(
            libc.find("accept4").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT),
            critical
        );
        MH_CONNECT = linker.downcallHandle(
            libc.find("connect").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT),
            critical
        );
        MH_RECV = linker.downcallHandle(
            libc.find("recv").orElseThrow(),
            FunctionDescriptor.of(JAVA_LONG, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_INT),
            critical
        );
        MH_SEND = linker.downcallHandle(
            libc.find("send").orElseThrow(),
            FunctionDescriptor.of(JAVA_LONG, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_INT),
            critical
        );
        MH_CLOSE = linker.downcallHandle(
            libc.find("close").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT),
            critical
        );
        MH_EPOLL_CREATE1 = linker.downcallHandle(
            libc.find("epoll_create1").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT),
            critical
        );
        MH_EPOLL_CTL = linker.downcallHandle(
            libc.find("epoll_ctl").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS),
            critical
        );
        MH_EPOLL_WAIT_FAST = linker.downcallHandle(
            libc.find("epoll_wait").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT),
            critical
        );
        MH_EPOLL_WAIT_BLOCK = linker.downcallHandle(
            libc.find("epoll_wait").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT)
        );
        MH_SCHED_SETAFFINITY = linker.downcallHandle(
            libc.find("sched_setaffinity").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_LONG, ADDRESS),
            critical
        );
        MH_CLOCK_GETTIME = linker.downcallHandle(
            libc.find("clock_gettime").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS),
            critical
        );

        // Load Intel ISA-L (libisal.so.2)
        SymbolLookup isal;
        try {
            isal = SymbolLookup.libraryLookup("libisal.so.2", Arena.global());
        } catch (IllegalArgumentException e) {
            try {
                isal = SymbolLookup.libraryLookup("/lib/x86_64-linux-gnu/libisal.so.2", Arena.global());
            } catch (IllegalArgumentException e2) {
                isal = SymbolLookup.libraryLookup("/usr/lib/x86_64-linux-gnu/libisal.so.2", Arena.global());
            }
        }

        MH_ISAL_DEFLATE_STATELESS_INIT = linker.downcallHandle(
            isal.find("isal_deflate_stateless_init").orElseThrow(),
            FunctionDescriptor.ofVoid(ADDRESS),
            critical
        );
        MH_ISAL_DEFLATE_STATELESS = linker.downcallHandle(
            isal.find("isal_deflate_stateless").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, ADDRESS),
            critical
        );
        MH_CRC32_GZIP_REFL = linker.downcallHandle(
            isal.find("crc32_gzip_refl").orElseThrow(),
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG),
            critical
        );
    }

    // =========================================================================
    // Zero-Overhead Syscall Wrappers
    // =========================================================================

    /**
     * Call ONCE per worker thread at startup and store the returned 4-byte MemorySegment.
     * Subsequent errno checks read directly from this segment: errnoSeg.get(JAVA_INT, 0).
     */
    public static MemorySegment errnoPtr() {
        try {
            MemorySegment ptr = (MemorySegment) MH_ERRNO_LOCATION.invokeExact();
            return ptr.reinterpret(4L);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int getErrno(MemorySegment errnoSeg) {
        return errnoSeg.get(JAVA_INT, 0L);
    }

    public static int socket(int domain, int type, int protocol) {
        try {
            return (int) MH_SOCKET.invokeExact(domain, type, protocol);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int setsockoptInt(int fd, int level, int optname, int optval, MemorySegment scratch4B) {
        try {
            scratch4B.set(JAVA_INT_UNALIGNED, 0L, optval);
            return (int) MH_SETSOCKOPT.invokeExact(fd, level, optname, scratch4B, 4);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int bindIpv4(int fd, int ipv4HostOrder, int port, MemorySegment scratch16B) {
        try {
            // struct sockaddr_in (16 bytes):
            // 0: sin_family (AF_INET = 2, short)
            // 2: sin_port   (big-endian uint16)
            // 4: sin_addr   (big-endian uint32)
            // 8: sin_zero   (8 bytes of 0)
            scratch16B.set(JAVA_LONG_UNALIGNED, 0L, 0L);
            scratch16B.set(JAVA_LONG_UNALIGNED, 8L, 0L);
            scratch16B.set(JAVA_SHORT_UNALIGNED, 0L, (short) AF_INET);
            scratch16B.set(JAVA_SHORT_UNALIGNED, 2L, Short.reverseBytes((short) port));
            scratch16B.set(JAVA_INT_UNALIGNED, 4L, Integer.reverseBytes(ipv4HostOrder));
            return (int) MH_BIND.invokeExact(fd, scratch16B, 16);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int listen(int fd, int backlog) {
        try {
            return (int) MH_LISTEN.invokeExact(fd, backlog);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int accept4NonBlock(int listenFd) {
        try {
            return (int) MH_ACCEPT4.invokeExact(
                listenFd,
                MemorySegment.NULL,
                MemorySegment.NULL,
                SOCK_NONBLOCK | SOCK_CLOEXEC
            );
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int connectIpv4(int fd, int ipv4HostOrder, int port, MemorySegment scratch16B) {
        try {
            scratch16B.set(JAVA_LONG_UNALIGNED, 0L, 0L);
            scratch16B.set(JAVA_LONG_UNALIGNED, 8L, 0L);
            scratch16B.set(JAVA_SHORT_UNALIGNED, 0L, (short) AF_INET);
            scratch16B.set(JAVA_SHORT_UNALIGNED, 2L, Short.reverseBytes((short) port));
            scratch16B.set(JAVA_INT_UNALIGNED, 4L, Integer.reverseBytes(ipv4HostOrder));
            return (int) MH_CONNECT.invokeExact(fd, scratch16B, 16);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static long recv(int fd, MemorySegment buf, long offset, long len) {
        try {
            return (long) MH_RECV.invokeExact(fd, buf.address() + offset, len, MSG_DONTWAIT);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static long send(int fd, MemorySegment buf, long offset, long len) {
        try {
            return (long) MH_SEND.invokeExact(fd, buf.address() + offset, len, SEND_FLAGS);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int close(int fd) {
        try {
            return (int) MH_CLOSE.invokeExact(fd);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int epollCreate() {
        try {
            return (int) MH_EPOLL_CREATE1.invokeExact(EPOLL_CLOEXEC);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int epollCtl(int epfd, int op, int fd, int events, long data64, MemorySegment evScratch12B) {
        try {
            evScratch12B.set(JAVA_INT_UNALIGNED, 0L, events);
            evScratch12B.set(JAVA_LONG_UNALIGNED, 4L, data64);
            return (int) MH_EPOLL_CTL.invokeExact(epfd, op, fd, evScratch12B);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int epollWait(int epfd, MemorySegment eventsBuf, int maxEvents, int timeoutMs) {
        try {
            if (timeoutMs == 0) {
                return (int) MH_EPOLL_WAIT_FAST.invokeExact(epfd, eventsBuf, maxEvents, 0);
            }
            return (int) MH_EPOLL_WAIT_BLOCK.invokeExact(epfd, eventsBuf, maxEvents, timeoutMs);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int epollEventMask(MemorySegment eventsBuf, int index) {
        return eventsBuf.get(JAVA_INT_UNALIGNED, (long) index * EPOLL_EVENT_SIZE);
    }

    public static long epollEventData(MemorySegment eventsBuf, int index) {
        return eventsBuf.get(JAVA_LONG_UNALIGNED, (long) index * EPOLL_EVENT_SIZE + 4L);
    }

    /**
     * Pins the calling kernel thread (pid = 0) to the specified logical CPU core.
     */
    public static void pinCurrentThreadToCore(int coreId, MemorySegment scratch128B) {
        try {
            for (long off = 0; off < 128L; off += 8L) {
                scratch128B.set(JAVA_LONG_UNALIGNED, off, 0L);
            }
            int byteIdx = coreId >>> 3;
            int bitMask = 1 << (coreId & 7);
            scratch128B.set(JAVA_BYTE, byteIdx, (byte) bitMask);
            int rc = (int) MH_SCHED_SETAFFINITY.invokeExact(0, 128L, scratch128B);
            if (rc < 0) {
                // Ignore if container/cpuset restricts the mask; OS scheduler keeps allowed cores.
            }
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    /**
     * Reads coarse epoch seconds + nanoseconds from the Linux vDSO page without a kernel transition.
     * struct timespec: offset 0 = tv_sec (long), offset 8 = tv_nsec (long).
     */
    public static long clockRealTimeCoarseSec(MemorySegment timespec16B) {
        try {
            int rc = (int) MH_CLOCK_GETTIME.invokeExact(CLOCK_REALTIME_COARSE, timespec16B);
            return rc == 0 ? timespec16B.get(JAVA_LONG_UNALIGNED, 0L) : 0L;
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static long clockMonotonicCoarseMs(MemorySegment timespec16B) {
        try {
            int rc = (int) MH_CLOCK_GETTIME.invokeExact(CLOCK_MONOTONIC_COARSE, timespec16B);
            if (rc != 0) return 0L;
            long sec = timespec16B.get(JAVA_LONG_UNALIGNED, 0L);
            long nsec = timespec16B.get(JAVA_LONG_UNALIGNED, 8L);
            return sec * 1000L + (nsec / 1_000_000L);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    // =========================================================================
    // Intel ISA-L (libisal.so.2) Hardware AVX-512 Compression Wrappers
    // =========================================================================

    /**
     * Initializes the per-worker 128 KB isal_zstream slab once at worker startup.
     */
    public static void isalInitStream(MemorySegment zstreamSlab) {
        try {
            MH_ISAL_DEFLATE_STATELESS_INIT.invokeExact(zstreamSlab);
            zstreamSlab.set(JAVA_INT_UNALIGNED, ISAL_OFFSET_LEVEL, 0);          // Level 0 = fastest AVX-512 path
            zstreamSlab.set(JAVA_INT_UNALIGNED, ISAL_OFFSET_LEVEL_BUFSZ, 0);
            zstreamSlab.set(JAVA_LONG_UNALIGNED, ISAL_OFFSET_LEVEL_BUF, 0L);
            zstreamSlab.set(JAVA_SHORT_UNALIGNED, ISAL_OFFSET_END_OF_STREAM, (short) 1);
            zstreamSlab.set(JAVA_SHORT_UNALIGNED, ISAL_OFFSET_FLUSH, ISAL_NO_FLUSH);
            zstreamSlab.set(JAVA_SHORT_UNALIGNED, ISAL_OFFSET_GZIP_FLAG, IGZIP_GZIP_NO_HDR);
            zstreamSlab.set(JAVA_SHORT_UNALIGNED, ISAL_OFFSET_HIST_BITS, (short) 0);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    /**
     * Compresses [inSeg + inOffset .. +inLen] directly into [outSeg + outOffset ..]
     * using IGZIP_GZIP_NO_HDR (assuming the 10-byte static Gzip header is already at outOffset - 10).
     * ISA-L fuses the CRC32 calculation inside the AVX-512 deflate pass and appends the 8-byte Gzip trailer.
     *
     * @return Total bytes written by ISA-L (compressed deflate body + 8-byte Gzip trailer), or -1 on error.
     */
    public static int isalDeflateGzipNoHdr(
            MemorySegment zstreamSlab,
            MemorySegment inSeg, long inOffset, int inLen,
            MemorySegment outSeg, long outOffset, int maxOutLen) {
        try {
            zstreamSlab.set(JAVA_LONG_UNALIGNED, ISAL_OFFSET_NEXT_IN, inSeg.address() + inOffset);
            zstreamSlab.set(JAVA_INT_UNALIGNED, ISAL_OFFSET_AVAIL_IN, inLen);
            zstreamSlab.set(JAVA_INT_UNALIGNED, ISAL_OFFSET_TOTAL_IN, 0);

            zstreamSlab.set(JAVA_LONG_UNALIGNED, ISAL_OFFSET_NEXT_OUT, outSeg.address() + outOffset);
            zstreamSlab.set(JAVA_INT_UNALIGNED, ISAL_OFFSET_AVAIL_OUT, maxOutLen);
            zstreamSlab.set(JAVA_INT_UNALIGNED, ISAL_OFFSET_TOTAL_OUT, 0);

            zstreamSlab.set(JAVA_SHORT_UNALIGNED, ISAL_OFFSET_END_OF_STREAM, (short) 1);
            zstreamSlab.set(JAVA_SHORT_UNALIGNED, ISAL_OFFSET_FLUSH, ISAL_NO_FLUSH);
            zstreamSlab.set(JAVA_SHORT_UNALIGNED, ISAL_OFFSET_GZIP_FLAG, IGZIP_GZIP_NO_HDR);

            int rc = (int) MH_ISAL_DEFLATE_STATELESS.invokeExact(zstreamSlab);
            if (rc != ISAL_COMP_OK) {
                return -1;
            }
            return zstreamSlab.get(JAVA_INT_UNALIGNED, ISAL_OFFSET_TOTAL_OUT);
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    public static int crc32GzipRefl(int initCrc, MemorySegment buf, long offset, long len) {
        try {
            return (int) MH_CRC32_GZIP_REFL.invokeExact(
                initCrc,
                offset == 0L ? buf : buf.asSlice(offset, len),
                len
            );
        } catch (Throwable t) {
            throw new AssertionError(t);
        }
    }

    /**
     * Creates and configures a non-blocking TCP listening socket with SO_REUSEADDR,
     * SO_REUSEPORT, TCP_NODELAY, and TCP_DEFER_ACCEPT.
     */
    public static int createListenSocket(int port, int backlog, MemorySegment scratch16B) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw new IllegalStateException("socket() failed for port " + port);
        }
        setsockoptInt(fd, SOL_SOCKET, SO_REUSEADDR, 1, scratch16B);
        setsockoptInt(fd, SOL_SOCKET, SO_REUSEPORT, 1, scratch16B);
        setsockoptInt(fd, IPPROTO_TCP, TCP_NODELAY, 1, scratch16B);
        setsockoptInt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, 1, scratch16B);

        if (bindIpv4(fd, 0, port, scratch16B) < 0) {
            close(fd);
            throw new IllegalStateException("bind() failed on port " + port);
        }
        if (listen(fd, backlog) < 0) {
            close(fd);
            throw new IllegalStateException("listen() failed on port " + port);
        }
        return fd;
    }
}
