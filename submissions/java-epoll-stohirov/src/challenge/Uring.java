package challenge;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.invoke.VarHandle;

final class Uring {
    static final int ACCEPT = 1, RECV = 2, SEND = 3, POLLOUT = 4, PG_RECV = 5, PG_POLLOUT = 6, CANCEL = 7;

    static long userData(int kind, int tag, int fd) { return (long) kind << 56 | (long) tag << 32 | (fd & 0xffffffffL); }

    static int kind(long ud) { return (int) (ud >>> 56); }

    static int tag(long ud) { return (int) (ud >>> 32) & 0xffffff; }

    static int fd(long ud) { return (int) ud; }

    interface Handler {
        void completion(long userData, int res, int flags);
    }

    static final int CQE_F_BUFFER = 1, CQE_F_MORE = 2;

    private static final int OP_POLL_ADD = 6, OP_ACCEPT = 13, OP_ASYNC_CANCEL = 14, OP_SEND = 26, OP_RECV = 27;
    private static final int SQE_BUFFER_SELECT = 1 << 5, SQE_CQE_SKIP_SUCCESS = 1 << 6;
    private static final int RECV_MULTISHOT = 1 << 1, ACCEPT_MULTISHOT = 1;
    private static final int SETUP_CQSIZE = 1 << 3, SETUP_SUBMIT_ALL = 1 << 7, SETUP_SINGLE_ISSUER = 1 << 12,
            SETUP_DEFER_TASKRUN = 1 << 13;
    private static final int FEAT_SINGLE_MMAP = 1, FEAT_NODROP = 1 << 1, FEAT_EXT_ARG = 1 << 8;
    private static final int ENTER_GETEVENTS = 1, ENTER_EXT_ARG = 1 << 3;
    private static final int REGISTER_PBUF_RING = 22;
    private static final long OFF_SQ_RING = 0, OFF_CQ_RING = 0x8000000L, OFF_SQES = 0x10000000L;
    private static final int MSG_DONTWAIT = 0x40, MSG_NOSIGNAL = 0x4000, POLLOUT_EVENTS = 0x4;
    private static final int BUF_GROUP = 0;

    private static final VarHandle INT = JAVA_INT.varHandle(), SHORT = JAVA_SHORT.varHandle();

    private final Sys sys;
    private final int fd;
    private final MemorySegment sqRing, cqRing, sqes;
    private final long sqHeadOff, sqTailOff, cqHeadOff, cqTailOff, cqesOff;
    private final int sqMask, sqEntries, cqMask;
    private int sqTail, queued;
    private final MemorySegment waitArg = Arena.global().allocate(24, 8), timespec = Arena.global().allocate(16, 8);

    private final MemorySegment bufRing, bufs;
    private final int bufSize, bufMask;
    private int bufTail;

    final MemorySegment arena;
    private long arenaPos;
    int sendsInFlight;

    private Uring(Sys sys, int fd, MemorySegment sqRing, MemorySegment cqRing, MemorySegment sqes, MemorySegment params,
            int bufCount, int bufSize, long arenaSize) {
        this.sys = sys;
        this.fd = fd;
        this.sqRing = sqRing;
        this.cqRing = cqRing;
        this.sqes = sqes;
        sqHeadOff = params.get(JAVA_INT, 40);
        sqTailOff = params.get(JAVA_INT, 44);
        sqMask = sqRing.get(JAVA_INT, params.get(JAVA_INT, 48));
        sqEntries = sqRing.get(JAVA_INT, params.get(JAVA_INT, 52));
        long arrayOff = params.get(JAVA_INT, 64);
        for (int i = 0; i < sqEntries; i++) sqRing.set(JAVA_INT, arrayOff + 4L * i, i);
        sqTail = (int) INT.getAcquire(sqRing, sqTailOff);
        cqHeadOff = params.get(JAVA_INT, 80);
        cqTailOff = params.get(JAVA_INT, 84);
        cqMask = cqRing.get(JAVA_INT, params.get(JAVA_INT, 88));
        cqesOff = params.get(JAVA_INT, 100);

        this.bufSize = bufSize;
        bufMask = bufCount - 1;
        bufRing = Arena.global().allocate(16L * bufCount, 4096);
        bufs = Arena.global().allocate((long) bufCount * bufSize, 4096);
        MemorySegment reg = Arena.global().allocate(40, 8);
        reg.set(JAVA_LONG, 0, bufRing.address());
        reg.set(JAVA_INT, 8, bufCount);
        reg.set(JAVA_SHORT, 12, (short) BUF_GROUP);
        if (sys.uringRegister(fd, REGISTER_PBUF_RING, reg, 1) < 0) {
            throw new IllegalStateException("io_uring: no provided buffer rings (errno " + sys.errno() + ")");
        }
        for (int b = 0; b < bufCount; b++) addBuffer(b);
        publishBuffers();
        arena = Arena.global().allocate(arenaSize, 64);
    }

    static Uring open(Sys sys, int entries, int bufCount, int bufSize, long arenaSize) {
        MemorySegment p = Arena.global().allocate(120, 8);
        int[] flagSets = {
            SETUP_CQSIZE | SETUP_SUBMIT_ALL | SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN, SETUP_CQSIZE | SETUP_SUBMIT_ALL
        };
        int fd = -1;
        for (int flags : flagSets) {
            p.fill((byte) 0);
            p.set(JAVA_INT, 4, entries * 8);
            p.set(JAVA_INT, 8, flags);
            fd = sys.uringSetup(entries, p);
            if (fd >= 0) break;
        }
        if (fd < 0) {
            System.err.println("io_uring unavailable (errno " + sys.errno() + "): this loop uses epoll");
            return null;
        }
        int features = p.get(JAVA_INT, 20);
        if ((features & (FEAT_SINGLE_MMAP | FEAT_NODROP | FEAT_EXT_ARG)) != (FEAT_SINGLE_MMAP | FEAT_NODROP | FEAT_EXT_ARG)) {
            Sys.close(fd);
            System.err.println("io_uring too old (features " + Integer.toHexString(features) + "): using epoll");
            return null;
        }
        long sqSize = p.get(JAVA_INT, 64) + 4L * p.get(JAVA_INT, 0);
        long cqSize = p.get(JAVA_INT, 100) + 16L * p.get(JAVA_INT, 4);
        MemorySegment rings = sys.mmapShared(fd, Math.max(sqSize, cqSize), OFF_SQ_RING);
        MemorySegment sqes = sys.mmapShared(fd, 64L * p.get(JAVA_INT, 0), OFF_SQES);
        if (rings == null || sqes == null) {
            Sys.close(fd);
            System.err.println("io_uring mmap failed (errno " + sys.errno() + "): using epoll");
            return null;
        }
        try {
            return new Uring(sys, fd, rings, rings, sqes, p, bufCount, bufSize, arenaSize);
        } catch (IllegalStateException e) {
            Sys.close(fd);
            System.err.println(e.getMessage() + ": using epoll");
            return null;
        }
    }


    private long sqe() {
        if (queued == sqEntries) submit(0, 0);
        long off = 64L * (sqTail & sqMask);
        for (int k = 0; k < 64; k += 8) sqes.set(JAVA_LONG, off + k, 0L);
        sqTail++;
        queued++;
        return off;
    }

    private void prep(long off, int op, int flags, int ioprio, int fd, long addr, int len, int opFlags, long ud) {
        sqes.set(JAVA_BYTE, off, (byte) op);
        sqes.set(JAVA_BYTE, off + 1, (byte) flags);
        sqes.set(JAVA_SHORT, off + 2, (short) ioprio);
        sqes.set(JAVA_INT, off + 4, fd);
        sqes.set(JAVA_LONG, off + 16, addr);
        sqes.set(JAVA_INT, off + 24, len);
        sqes.set(JAVA_INT, off + 28, opFlags);
        sqes.set(JAVA_LONG, off + 32, ud);
    }

    void acceptMultishot(int listenFd, long ud) {
        prep(sqe(), OP_ACCEPT, 0, ACCEPT_MULTISHOT, listenFd, 0, 0, Sys.SOCK_NONBLOCK | Sys.SOCK_CLOEXEC, ud);
    }

    void recvMultishot(int fd, long ud) {
        long off = sqe();
        prep(off, OP_RECV, SQE_BUFFER_SELECT, RECV_MULTISHOT, fd, 0, 0, 0, ud);
        sqes.set(JAVA_SHORT, off + 40, (short) BUF_GROUP);
    }

    void send(int fd, long at, int len, long ud) {
        prep(sqe(), OP_SEND, 0, 0, fd, arena.address() + at, len, MSG_DONTWAIT | MSG_NOSIGNAL, ud);
        sendsInFlight++;
    }

    void pollOut(int fd, long ud) { prep(sqe(), OP_POLL_ADD, 0, 0, fd, 0, 0, POLLOUT_EVENTS, ud); }

    void cancel(long target) {
        prep(sqe(), OP_ASYNC_CANCEL, SQE_CQE_SKIP_SUCCESS, 0, -1, target, 0, 0, userData(CANCEL, 0, 0));
    }

    long alloc(int n) {
        if (sendsInFlight == 0) arenaPos = 0;
        if (arenaPos + n > arena.byteSize()) return -1;
        long at = arenaPos;
        arenaPos += (n + 63) & ~63;
        return at;
    }

    void submit(int minComplete, long timeoutNanos) {
        INT.setRelease(sqRing, sqTailOff, sqTail);
        int flags = 0;
        MemorySegment arg = MemorySegment.NULL;
        long argSize = 0;
        if (minComplete > 0) {
            timespec.set(JAVA_LONG, 0, timeoutNanos / 1_000_000_000L);
            timespec.set(JAVA_LONG, 8, timeoutNanos % 1_000_000_000L);
            waitArg.set(JAVA_LONG, 0, 0L);
            waitArg.set(JAVA_INT, 8, 8);
            waitArg.set(JAVA_INT, 12, 0);
            waitArg.set(JAVA_LONG, 16, timespec.address());
            flags = ENTER_GETEVENTS | ENTER_EXT_ARG;
            arg = waitArg;
            argSize = 24;
        }
        int r = sys.uringEnter(fd, queued, minComplete, flags, arg, argSize);
        if (r < 0) {
            int e = sys.errno();
            if (e != Sys.ETIME && e != Sys.EINTR && e != Sys.EBUSY && e != Sys.EAGAIN) {
                throw new IllegalStateException("io_uring_enter: errno " + e);
            }
        }
        queued = sqTail - (int) INT.getAcquire(sqRing, sqHeadOff);
    }


    int reap(Handler h) {
        int n = 0;
        int head = (int) INT.getAcquire(cqRing, cqHeadOff);
        while (true) {
            int tail = (int) INT.getAcquire(cqRing, cqTailOff);
            if (head == tail) break;
            while (head != tail) {
                long off = cqesOff + 16L * (head & cqMask);
                long ud = cqRing.get(JAVA_LONG, off);
                int res = cqRing.get(JAVA_INT, off + 8), flags = cqRing.get(JAVA_INT, off + 12);
                head++;
                INT.setRelease(cqRing, cqHeadOff, head);
                h.completion(ud, res, flags);
                n++;
            }
        }
        publishBuffers();
        return n;
    }

    MemorySegment buffer(int flags, int res) {
        return bufs.asSlice((long) (flags >>> 16) * bufSize, res);
    }

    void recycle(int flags) {
        if ((flags & CQE_F_BUFFER) != 0) addBuffer(flags >>> 16);
    }

    private void addBuffer(int bid) {
        long e = 16L * (bufTail & bufMask);
        bufRing.set(JAVA_LONG, e, bufs.address() + (long) bid * bufSize);
        bufRing.set(JAVA_INT, e + 8, bufSize);
        bufRing.set(JAVA_SHORT, e + 12, (short) bid);
        bufTail++;
    }

    void publishBuffers() {
        SHORT.setRelease(bufRing, 14L, (short) bufTail);
    }
}
