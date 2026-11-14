package challenge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT_UNALIGNED;

public final class Loop implements Runnable, Pg.CompletionListener {

    // Maximum File Descriptor supported across the system (2M connections * 4B = 8 MB shared table)
    public static final int MAX_FD = 2_097_152;

    // Epoll batch size and buffer constants
    private static final int  MAX_EVENTS        = 1024;
    private static final long RECV_SCRATCH_SIZE = 65536L;  // 64 KB shared per-worker recv buffer
    private static final long JSON_SCRATCH_SIZE = 131072L; // 128 KB per-worker JSON serialization buffer
    private static final long OUT_SCRATCH_SIZE  = 131072L; // 128 KB per-worker Gzip output buffer
    private static final long UNESCAPE_SIZE     = 32768L;  // 32 KB per-worker POST body unescape scratch

    // Partial-Request Pool for rare TCP-fragmented HTTP requests (4095 slots * 1 KB = ~4 MB per worker)
    private static final int  PARTIAL_SLOTS     = 4095;
    private static final int  PARTIAL_SLOT_SIZE = 1024;

    // 64-bit epoll_data_t tags
    private static final long TAG_LISTEN   = 1L << 62;
    private static final long TAG_PG_READ  = 1L << 61;
    private static final long TAG_PG_WRITE = 1L << 60;
    private static final long TAG_MASK     = 0x7L << 60;

    // fdState bit layout:
    //   bits  0..15 : 16-bit connection generation sequence (0..65535)
    //   bit  16     : close connection after current response completes
    //   bits 17..28 : partial buffer slot + 1 (0 = none, 1..2048 = leased slot)
    //   bit  29     : socket currently open (cleared before Sys.close)
    private static final int SEQ_MASK           = 0xFFFF;
    private static final int FLAG_CLOSE_NEXT    = 1 << 16;
    private static final int PARTIAL_SHIFT      = 17;
    private static final int PARTIAL_VAL_MASK   = 0xFFF;
    private static final int PARTIAL_FIELD_MASK = PARTIAL_VAL_MASK << PARTIAL_SHIFT;
    private static final int FLAG_OPEN          = 1 << 29;

    // Little-endian HTTP Method & Path constants
    private static final int  METHOD_GET_SPACE  = 0x20544547; // "GET "
    private static final int  METHOD_POST       = 0x54534F50; // "POST"
    private static final int  CRLF_INT          = 0x0A0D0A0D; // "\r\n\r\n"

    private final int workerId;
    private final int[] listenPorts;
    private final long serverStartEpochSec;
    private final MemorySegment fdStateTable; // Shared 8 MB table (MAX_FD * 4 bytes)

    private final Pg.PgConn[] readPool;
    private final Pg.PgConn[] writePool;

    // Per-worker off-heap scratch buffers (100% cache-hot, zero GC footprint)
    private final MemorySegment errnoSeg;
    private final MemorySegment evScratch16B;
    private final MemorySegment eventsBuf;
    private final MemorySegment recvScratch;
    private final MemorySegment jsonScratch;
    private final MemorySegment outScratch;
    private final MemorySegment jwtScratch;
    private final MemorySegment unescapeScratch;
    private final MemorySegment zstreamSlab;

    // Partial request ring pool
    private final MemorySegment partialSlab;
    private final int[] partialLen = new int[PARTIAL_SLOTS + 1];
    private final int[] freePartialSlots = new int[PARTIAL_SLOTS];
    private int freePartialTop = PARTIAL_SLOTS;

    // Dirty PG connection bitmasks for end-of-tick single-Sync batch flushing
    private long dirtyReadMask  = 0L;
    private long dirtyWriteMask = 0L;
    private int  readRrCursor   = 0;
    private int  writeRrCursor  = 0;

    private int epfd = -1;
    private long nowEpochSec;

    public Loop(
            int workerId,
            int[] listenPorts,
            long serverStartEpochSec,
            MemorySegment fdStateTable,
            Pg.DbEndpoint dbEndpoint,
            int readConnsPerWorker,
            int writeConnsPerWorker) throws Exception {

        this.workerId            = workerId;
        this.listenPorts         = listenPorts;
        this.serverStartEpochSec = serverStartEpochSec;
        this.fdStateTable        = fdStateTable;

        // 1. Connect & prepare all PostgreSQL sockets first using a temporary confined arena,
        //    so failed startup retries never leak permanent Arena.global() memory.
        int[] readFds  = new int[readConnsPerWorker];
        int[] writeFds = new int[writeConnsPerWorker];
        java.util.Arrays.fill(readFds, -1);
        java.util.Arrays.fill(writeFds, -1);

        try (Arena initArena = Arena.ofConfined()) {
            MemorySegment initScratch16B = initArena.allocate(16L, 8L);
            for (int i = 0; i < readConnsPerWorker; i++) {
                readFds[i] = Pg.PgConn.connectAndPrepareBlocking(dbEndpoint, initScratch16B);
            }
            for (int i = 0; i < writeConnsPerWorker; i++) {
                writeFds[i] = Pg.PgConn.connectAndPrepareBlocking(dbEndpoint, initScratch16B);
            }
        } catch (Exception e) {
            for (int fd : readFds) {
                if (fd >= 0) Sys.close(fd);
            }
            for (int fd : writeFds) {
                if (fd >= 0) Sys.close(fd);
            }
            throw e;
        }

        // 2. Allocate all permanent per-worker & PG buffers from Arena.global() so HotSpot C2
        //    can elide thread-local Arena scope liveness checks on every MemorySegment access.
        Arena global         = Arena.global();
        this.errnoSeg        = Sys.errnoPtr();
        this.evScratch16B    = global.allocate(16L, 8L);
        this.eventsBuf       = global.allocate(MAX_EVENTS * Sys.EPOLL_EVENT_SIZE, 16L);
        this.recvScratch     = global.allocate(RECV_SCRATCH_SIZE, 64L);
        this.jsonScratch     = global.allocate(JSON_SCRATCH_SIZE, 64L);
        this.outScratch      = global.allocate(OUT_SCRATCH_SIZE, 64L);
        this.jwtScratch      = global.allocate(Jwt.SCRATCH_BYTES, 64L);
        this.unescapeScratch = global.allocate(UNESCAPE_SIZE, 64L);
        this.zstreamSlab     = global.allocate(Sys.ISAL_ZSTREAM_ALLOC_SIZE, 64L);
        this.partialSlab     = global.allocate((long) (PARTIAL_SLOTS + 1) * PARTIAL_SLOT_SIZE, 64L);

        Sys.isalInitStream(zstreamSlab);

        for (int i = 0; i < PARTIAL_SLOTS; i++) {
            freePartialSlots[i] = i + 1;
        }

        this.readPool = new Pg.PgConn[readConnsPerWorker];
        for (int i = 0; i < readConnsPerWorker; i++) {
            this.readPool[i] = new Pg.PgConn(i, false, readFds[i], global);
        }
        this.writePool = new Pg.PgConn[writeConnsPerWorker];
        for (int i = 0; i < writeConnsPerWorker; i++) {
            this.writePool[i] = new Pg.PgConn(i, true, writeFds[i], global);
        }
    }

    @Override
    public void run() {
        // 1. Create epoll instance
        this.epfd = Sys.epollCreate();
        if (epfd < 0) {
            throw new IllegalStateException("epoll_create1 failed on worker " + workerId);
        }

        // 2. Register PostgreSQL Read & Write sockets in epoll (Edge-Triggered)
        for (int i = 0; i < readPool.length; i++) {
            Pg.PgConn c = readPool[i];
            Sys.epollCtl(epfd, Sys.EPOLL_CTL_ADD, c.fd, Sys.EPOLLIN | Sys.EPOLLET, TAG_PG_READ | i, evScratch16B);
        }
        for (int i = 0; i < writePool.length; i++) {
            Pg.PgConn c = writePool[i];
            Sys.epollCtl(epfd, Sys.EPOLL_CTL_ADD, c.fd, Sys.EPOLLIN | Sys.EPOLLET, TAG_PG_WRITE | i, evScratch16B);
        }

        // 3. Open SO_REUSEPORT listening sockets for every configured port (e.g., 80 and 81)
        for (int port : listenPorts) {
            int listenFd = Sys.createListenSocket(port, 65535, evScratch16B);
            Sys.epollCtl(
                epfd, Sys.EPOLL_CTL_ADD, listenFd,
                Sys.EPOLLIN, TAG_LISTEN | (listenFd & 0xFFFFFFFFL), evScratch16B
            );
        }

        // 4. Main Zero-Allocation Event Loop
        int timeoutMs = 1;
        while (true) {
            int nEvents = Sys.epollWait(epfd, eventsBuf, MAX_EVENTS, timeoutMs);
            this.nowEpochSec = Sys.clockRealTimeCoarseSec(evScratch16B);

            if (nEvents <= 0) {
                timeoutMs = 1;
                continue;
            }

            // Use non-blocking poll (critical(false) fast path) on the next iteration if batch was full
            timeoutMs = (nEvents == MAX_EVENTS) ? 0 : 1;

            // Pass 1: Process PostgreSQL socket events first to drain completed responses
            // and immediately free queue slots & scratch buffers
            for (int i = 0; i < nEvents; i++) {
                long data = Sys.epollEventData(eventsBuf, i);
                long tag  = data & TAG_MASK;
                if (tag == TAG_PG_READ) {
                    int idx = (int) (data & 0xFFFFL);
                    Pg.PgConn conn = readPool[idx];
                    int mask = Sys.epollEventMask(eventsBuf, i);
                    if ((mask & Sys.EPOLLIN) != 0) {
                        conn.onReadable(errnoSeg, jsonScratch, this);
                    }
                    if ((mask & Sys.EPOLLOUT) != 0) {
                        if (conn.flushIfNeeded(errnoSeg) && conn.epollOutArmed) {
                            conn.epollOutArmed = false;
                            Sys.epollCtl(epfd, Sys.EPOLL_CTL_MOD, conn.fd, Sys.EPOLLIN | Sys.EPOLLET, data, evScratch16B);
                        }
                    }
                } else if (tag == TAG_PG_WRITE) {
                    int idx = (int) (data & 0xFFFFL);
                    Pg.PgConn conn = writePool[idx];
                    int mask = Sys.epollEventMask(eventsBuf, i);
                    if ((mask & Sys.EPOLLIN) != 0) {
                        conn.onReadable(errnoSeg, jsonScratch, this);
                    }
                    if ((mask & Sys.EPOLLOUT) != 0) {
                        if (conn.flushIfNeeded(errnoSeg) && conn.epollOutArmed) {
                            conn.epollOutArmed = false;
                            Sys.epollCtl(epfd, Sys.EPOLL_CTL_MOD, conn.fd, Sys.EPOLLIN | Sys.EPOLLET, data, evScratch16B);
                        }
                    }
                }
            }

            // Pass 2: Process client HTTP requests and incoming connections
            for (int i = 0; i < nEvents; i++) {
                long data = Sys.epollEventData(eventsBuf, i);
                long tag  = data & TAG_MASK;

                if (tag == 0L) {
                    // Hot Path: Client Socket Event
                    int clientFd = (int) data;
                    int mask = Sys.epollEventMask(eventsBuf, i);
                    if ((mask & (Sys.EPOLLERR | Sys.EPOLLHUP | Sys.EPOLLRDHUP)) != 0) {
                        boolean stillOpen = true;
                        if ((mask & Sys.EPOLLIN) != 0) {
                            stillOpen = onClientReadable(clientFd);
                        }
                        if (stillOpen) {
                            closeClient(clientFd);
                        }
                    } else if ((mask & Sys.EPOLLIN) != 0) {
                        onClientReadable(clientFd);
                    }
                } else if (tag == TAG_LISTEN) {
                    int listenFd = (int) (data & 0xFFFFFFFFL);
                    onAcceptReady(listenFd);
                }
            }

            // 6. Flush all dirty Read & Write PostgreSQL connections in a single Sync batch per connection
            if (dirtyReadMask != 0L) {
                flushDirtyPool(readPool, dirtyReadMask, TAG_PG_READ);
                dirtyReadMask = 0L;
            }
            if (dirtyWriteMask != 0L) {
                flushDirtyPool(writePool, dirtyWriteMask, TAG_PG_WRITE);
                dirtyWriteMask = 0L;
            }
        }
    }

    private void armEpollOut(Pg.PgConn conn, long tagPrefix) {
        if (!conn.epollOutArmed) {
            conn.epollOutArmed = true;
            Sys.epollCtl(
                epfd, Sys.EPOLL_CTL_MOD, conn.fd,
                Sys.EPOLLIN | Sys.EPOLLOUT | Sys.EPOLLET,
                tagPrefix | conn.connIndex, evScratch16B
            );
        }
    }

    private void flushDirtyPool(Pg.PgConn[] pool, long mask, long tagPrefix) {
        while (mask != 0L) {
            int idx = Long.numberOfTrailingZeros(mask);
            mask &= (mask - 1L);
            Pg.PgConn conn = pool[idx];
            if (!conn.flushIfNeeded(errnoSeg)) {
                armEpollOut(conn, tagPrefix);
            }
        }
    }

    // =========================================================================
    // Connection Accept & Lifecycle Management
    // =========================================================================

    private void onAcceptReady(int listenFd) {
        while (true) {
            int clientFd = Sys.accept4NonBlock(listenFd);
            if (clientFd < 0) {
                int err = Sys.getErrno(errnoSeg);
                if (err == Sys.EINTR) continue;
                break; // EAGAIN
            }
            if (clientFd >= MAX_FD) {
                Sys.close(clientFd);
                continue;
            }

            Sys.setsockoptInt(clientFd, Sys.IPPROTO_TCP, Sys.TCP_NODELAY, 1, evScratch16B);

            // Increment 16-bit sequence generation for this FD, mark open, and clear flags/partial slot
            long stateOff = (long) clientFd << 2;
            int prevSeq = fdStateTable.get(JAVA_INT_UNALIGNED, stateOff) & SEQ_MASK;
            int nextSeq = (prevSeq + 1) & SEQ_MASK;
            if (nextSeq == 0) nextSeq = 1;
            fdStateTable.set(JAVA_INT_UNALIGNED, stateOff, nextSeq | FLAG_OPEN);

            Sys.epollCtl(
                epfd, Sys.EPOLL_CTL_ADD, clientFd,
                Sys.EPOLLIN | Sys.EPOLLRDHUP | Sys.EPOLLET,
                clientFd, evScratch16B
            );
        }
    }

    private void closeClient(int clientFd) {
        if (clientFd <= 0 || clientFd >= MAX_FD) return;
        long stateOff = (long) clientFd << 2;
        int state = fdStateTable.get(JAVA_INT_UNALIGNED, stateOff);
        if ((state & FLAG_OPEN) == 0) return; // Already closed

        int partialSlot = (state >>> PARTIAL_SHIFT) & PARTIAL_VAL_MASK;
        if (partialSlot != 0) {
            releasePartialSlot(partialSlot);
        }

        // Advance sequence number and clear FLAG_OPEN before closing FD so other threads
        // accepting the reused FD cannot collide and any in-flight DB queries abort.
        int nextSeq = ((state + 1) & SEQ_MASK);
        if (nextSeq == 0) nextSeq = 1;
        fdStateTable.set(JAVA_INT_UNALIGNED, stateOff, nextSeq);

        Sys.close(clientFd);
    }

    private int leasePartialSlot() {
        if (freePartialTop == 0) return 0;
        int slot = freePartialSlots[--freePartialTop];
        partialLen[slot] = 0;
        return slot;
    }

    private void releasePartialSlot(int slot) {
        if (slot > 0 && slot <= PARTIAL_SLOTS && freePartialTop < PARTIAL_SLOTS) {
            partialLen[slot] = 0;
            freePartialSlots[freePartialTop++] = slot;
        }
    }

    // =========================================================================
    // Zero-Copy Client Socket Read & HTTP/1.1 State Machine
    // =========================================================================

    private boolean onClientReadable(int clientFd) {
        long stateOff = (long) clientFd << 2;

        while (true) {
            int state = fdStateTable.get(JAVA_INT_UNALIGNED, stateOff);
            if ((state & FLAG_OPEN) == 0) return false;
            int partialSlot = (state >>> PARTIAL_SHIFT) & PARTIAL_VAL_MASK;
            int preLen = 0;

            if (partialSlot != 0) {
                preLen = partialLen[partialSlot];
                long slotOff = (long) partialSlot * PARTIAL_SLOT_SIZE;
                MemorySegment.copy(partialSlab, slotOff, recvScratch, 0L, preLen);
            }

            long n = Sys.recv(clientFd, recvScratch, preLen, RECV_SCRATCH_SIZE - preLen);
            if (n > 0) {
                int totalLen = preLen + (int) n;
                int consumed = processHttpBuffer(clientFd, stateOff, recvScratch, totalLen);
                if (consumed < 0) {
                    // Malformed request or close requested -> already closed
                    return false;
                }
                int rem = totalLen - consumed;
                if (rem == 0) {
                    if (partialSlot != 0) {
                        releasePartialSlot(partialSlot);
                        int curState = fdStateTable.get(JAVA_INT_UNALIGNED, stateOff);
                        if ((curState & FLAG_OPEN) != 0) {
                            fdStateTable.set(JAVA_INT_UNALIGNED, stateOff, curState & ~PARTIAL_FIELD_MASK);
                        }
                    }
                } else {
                    if (rem > PARTIAL_SLOT_SIZE) {
                        sendStaticAndMaybeClose(clientFd, Out.RESP_400_GENERIC, Out.RESP_400_GENERIC_LEN, true);
                        return false;
                    }
                    if (partialSlot == 0) {
                        partialSlot = leasePartialSlot();
                        if (partialSlot == 0) {
                            sendStaticAndMaybeClose(clientFd, Out.RESP_503, Out.RESP_503_LEN, true);
                            return false;
                        }
                        int curState = fdStateTable.get(JAVA_INT_UNALIGNED, stateOff);
                        if ((curState & FLAG_OPEN) == 0) {
                            releasePartialSlot(partialSlot);
                            return false;
                        }
                        fdStateTable.set(
                            JAVA_INT_UNALIGNED, stateOff,
                            (curState & ~PARTIAL_FIELD_MASK) | (partialSlot << PARTIAL_SHIFT)
                        );
                    }
                    long slotOff = (long) partialSlot * PARTIAL_SLOT_SIZE;
                    MemorySegment.copy(recvScratch, consumed, partialSlab, slotOff, rem);
                    partialLen[partialSlot] = rem;
                }
            } else if (n == 0) {
                closeClient(clientFd);
                return false;
            } else {
                int err = Sys.getErrno(errnoSeg);
                if (err == Sys.EINTR) continue;
                if (err == Sys.EAGAIN) break;
                closeClient(clientFd);
                return false;
            }
        }
        return true;
    }

    /**
     * Parses one or more pipelined HTTP/1.1 requests inside buf[0..totalLen).
     *
     * @return Number of bytes consumed, or -1 if the connection was closed.
     */
    private int processHttpBuffer(int clientFd, long stateOff, MemorySegment buf, int totalLen) {
        int parsePos = 0;

        while (parsePos < totalLen) {
            int avail = totalLen - parsePos;
            if (avail < 14) {
                return parsePos; // Need more bytes for minimum HTTP request
            }

            // 1. Locate end of HTTP headers ("\r\n\r\n")
            int hdrEnd = findHeaderEnd(buf, parsePos, totalLen);
            if (hdrEnd < 0) {
                if (avail > 8192) {
                    sendStaticAndMaybeClose(clientFd, Out.RESP_400_GENERIC, Out.RESP_400_GENERIC_LEN, true);
                    return -1;
                }
                return parsePos; // Incomplete headers
            }

            // 2. Parse Request Line: Method + Path
            int linePos = parsePos;
            int mWord = buf.get(JAVA_INT_UNALIGNED, linePos);
            boolean isGet  = (mWord == METHOD_GET_SPACE);
            boolean isPost = (!isGet && mWord == METHOD_POST && buf.get(JAVA_BYTE, linePos + 4L) == ' ');

            int pathStart;
            if (isGet) {
                pathStart = linePos + 4;
            } else if (isPost) {
                pathStart = linePos + 5;
            } else {
                // Check if another valid HTTP method (HEAD/PUT/DELETE/PATCH/OPTIONS) -> 405, else 400
                sendStaticAndMaybeClose(clientFd, Out.RESP_405, Out.RESP_405_LEN, true);
                return -1;
            }

            // Find end of request target (' ' or '?') and end of request line ('\r')
            int pathEnd = pathStart;
            int queryEnd = -1;
            while (pathEnd < hdrEnd) {
                byte b = buf.get(JAVA_BYTE, pathEnd);
                if (b == ' ') break;
                if (b == '?' && queryEnd < 0) {
                    queryEnd = pathEnd;
                }
                pathEnd++;
            }
            if (pathEnd >= hdrEnd) {
                sendStaticAndMaybeClose(clientFd, Out.RESP_400_GENERIC, Out.RESP_400_GENERIC_LEN, true);
                return -1;
            }
            int cleanPathEnd = (queryEnd >= 0) ? queryEnd : pathEnd;
            int pathLen = cleanPathEnd - pathStart;

            int reqLineEnd = pathEnd + 1;
            while (reqLineEnd < hdrEnd && buf.get(JAVA_BYTE, reqLineEnd) != '\r') {
                reqLineEnd++;
            }

            // 3. Scan HTTP Headers (Content-Length, Authorization, Accept-Encoding, Connection)
            int contentLength = 0;
            long authOff      = -1L;
            int authLen       = 0;
            boolean gzip      = false;
            boolean connClose = false;

            int hPos = reqLineEnd + 2; // skip "\r\n"
            int headersLimit = hdrEnd - 4;
            while (hPos < headersLimit) {
                int lineEnd = hPos;
                while (lineEnd < headersLimit && buf.get(JAVA_BYTE, lineEnd) != '\r') {
                    lineEnd++;
                }
                if (lineEnd == hPos) {
                    // Blank line hit early (e.g., two pipelined GET requests matched by end-of-packet check)
                    if (hPos >= parsePos + 2 && buf.get(JAVA_INT_UNALIGNED, hPos - 2L) == CRLF_INT) {
                        hdrEnd = hPos + 2;
                        break;
                    }
                    sendStaticAndMaybeClose(clientFd, Out.RESP_400_GENERIC, Out.RESP_400_GENERIC_LEN, true);
                    return -1;
                }
                int colon = hPos;
                while (colon < lineEnd && buf.get(JAVA_BYTE, colon) != ':') {
                    colon++;
                }
                if (colon < lineEnd) {
                    int nameLen = colon - hPos;
                    int valStart = colon + 1;
                    while (valStart < lineEnd && (buf.get(JAVA_BYTE, valStart) == ' ' || buf.get(JAVA_BYTE, valStart) == '\t')) {
                        valStart++;
                    }
                    int valLen = lineEnd - valStart;
                    byte firstCharLower = (byte) (buf.get(JAVA_BYTE, hPos) | 0x20);

                    if (nameLen == 14 && firstCharLower == 'c' &&
                        (buf.get(JAVA_BYTE, hPos + 8) | 0x20) == 'l') {
                        // Content-Length (14 chars)
                        contentLength = parsePositiveInt(buf, valStart, valLen);
                        if (contentLength < 0 || contentLength > 32768) {
                            sendStaticAndMaybeClose(clientFd, Out.RESP_400_GENERIC, Out.RESP_400_GENERIC_LEN, true);
                            return -1;
                        }
                    } else if (nameLen == 13 && firstCharLower == 'a' &&
                               (buf.get(JAVA_BYTE, hPos + 1) | 0x20) == 'u') {
                        // Authorization (13 chars)
                        authOff = valStart;
                        authLen = valLen;
                    } else if (nameLen == 15 && firstCharLower == 'a' &&
                               (buf.get(JAVA_BYTE, hPos + 7) | 0x20) == 'e') {
                        // Accept-Encoding (15 chars; disambiguated from Accept-Language)
                        if (containsGzip(buf, valStart, valLen)) {
                            gzip = true;
                        }
                    } else if (nameLen == 10 && firstCharLower == 'c' &&
                               (buf.get(JAVA_BYTE, hPos + 4) | 0x20) == 'e') {
                        // Connection (10 chars)
                        if (valLen >= 5 && (buf.get(JAVA_BYTE, valStart) | 0x20) == 'c') {
                            connClose = true;
                        }
                    }
                }
                hPos = lineEnd + 2;
            }

            // 4. Verify complete HTTP body has arrived
            int reqTotalEnd = hdrEnd + contentLength;
            if (reqTotalEnd > totalLen) {
                return parsePos; // Wait for remaining body bytes
            }

            // Capture generation sequence + per-request Connection: close flag without mutating fdStateTable
            int curState = fdStateTable.get(JAVA_INT_UNALIGNED, stateOff);
            int reqToken = (curState & SEQ_MASK) | (connClose ? FLAG_CLOSE_NEXT : 0);

            // 5. Route & Dispatch Request
            boolean keepOpen = dispatchRequest(
                clientFd, reqToken, isGet, isPost,
                buf, pathStart, pathLen,
                authOff, authLen,
                hdrEnd, contentLength,
                gzip, connClose
            );
            if (!keepOpen) {
                return -1;
            }

            parsePos = reqTotalEnd;
        }

        return parsePos;
    }

    private static int findHeaderEnd(MemorySegment buf, int start, int limit) {
        int end = limit - 4;
        if (start <= end && buf.get(JAVA_INT_UNALIGNED, end) == CRLF_INT) {
            return limit;
        }
        for (int i = start; i <= end; i++) {
            if (buf.get(JAVA_INT_UNALIGNED, i) == CRLF_INT) {
                return i + 4;
            }
        }
        return -1;
    }

    private static int parsePositiveInt(MemorySegment buf, int off, int len) {
        while (len > 0 && (buf.get(JAVA_BYTE, off + len - 1) == ' ' || buf.get(JAVA_BYTE, off + len - 1) == '\t')) {
            len--;
        }
        if (len <= 0 || len > 9) return -1;
        int val = 0;
        for (int i = 0; i < len; i++) {
            int d = buf.get(JAVA_BYTE, off + i) - '0';
            if (d < 0 || d > 9) return -1;
            val = val * 10 + d;
        }
        return val;
    }

    private static boolean containsGzip(MemorySegment buf, int off, int len) {
        int end = off + len - 4;
        for (int i = off; i <= end; i++) {
            if ((buf.get(JAVA_BYTE, i)     | 0x20) == 'g' &&
                (buf.get(JAVA_BYTE, i + 1) | 0x20) == 'z' &&
                (buf.get(JAVA_BYTE, i + 2) | 0x20) == 'i' &&
                (buf.get(JAVA_BYTE, i + 3) | 0x20) == 'p') {
                return true;
            }
        }
        return false;
    }

    private static boolean isBearer(MemorySegment buf, long off, int len) {
        int i = 0;
        while (i < len && (buf.get(JAVA_BYTE, off + i) == ' ' || buf.get(JAVA_BYTE, off + i) == '\t')) {
            i++;
        }
        if (len - i < 7) return false;
        return (buf.get(JAVA_BYTE, off + i)     | 0x20) == 'b' &&
               (buf.get(JAVA_BYTE, off + i + 1) | 0x20) == 'e' &&
               (buf.get(JAVA_BYTE, off + i + 2) | 0x20) == 'a' &&
               (buf.get(JAVA_BYTE, off + i + 3) | 0x20) == 'r' &&
               (buf.get(JAVA_BYTE, off + i + 4) | 0x20) == 'e' &&
               (buf.get(JAVA_BYTE, off + i + 5) | 0x20) == 'r' &&
                buf.get(JAVA_BYTE, off + i + 6) == ' ';
    }

    // =========================================================================
    // Route Matching & Non-Blocking PostgreSQL Enqueue
    // =========================================================================

    private boolean dispatchRequest(
            int clientFd, int reqSeq, boolean isGet, boolean isPost,
            MemorySegment buf, int pathOff, int pathLen,
            long authOff, int authLen,
            int bodyOff, int bodyLen,
            boolean gzip, boolean connClose) {

        if (pathLen < 1 || buf.get(JAVA_BYTE, pathOff) != '/') {
            return sendStaticAndMaybeClose(clientFd, Out.RESP_400_GENERIC, Out.RESP_400_GENERIC_LEN, connClose);
        }

        // Route 1: /feed (5 chars)
        if (pathLen == 5 &&
            buf.get(JAVA_BYTE, pathOff + 1) == 'f' &&
            buf.get(JAVA_BYTE, pathOff + 2) == 'e' &&
            buf.get(JAVA_BYTE, pathOff + 3) == 'e' &&
            buf.get(JAVA_BYTE, pathOff + 4) == 'd') {

            if (!isGet) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_405, Out.RESP_405_LEN, connClose);
            }
            Pg.PgConn pg = selectReadConn(32);
            if (pg == null) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_503, Out.RESP_503_LEN, connClose);
            }
            pg.enqueueFeed(clientFd, reqSeq, gzip);
            dirtyReadMask |= (1L << pg.connIndex);
            return true;
        }

        // Route 2: /health (7 chars)
        if (pathLen == 7 &&
            buf.get(JAVA_BYTE, pathOff + 1) == 'h' &&
            buf.get(JAVA_BYTE, pathOff + 2) == 'e' &&
            buf.get(JAVA_BYTE, pathOff + 3) == 'a' &&
            buf.get(JAVA_BYTE, pathOff + 4) == 'l' &&
            buf.get(JAVA_BYTE, pathOff + 5) == 't' &&
            buf.get(JAVA_BYTE, pathOff + 6) == 'h') {

            if (!isGet) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_405, Out.RESP_405_LEN, connClose);
            }
            Pg.PgConn pg = selectReadConn(32);
            if (pg == null) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_503, Out.RESP_503_LEN, connClose);
            }
            pg.enqueueHealth(clientFd, reqSeq);
            dirtyReadMask |= (1L << pg.connIndex);
            return true;
        }

        // Route 3: /posts (6 chars)
        if (pathLen == 6 &&
            buf.get(JAVA_BYTE, pathOff + 1) == 'p' &&
            buf.get(JAVA_BYTE, pathOff + 2) == 'o' &&
            buf.get(JAVA_BYTE, pathOff + 3) == 's' &&
            buf.get(JAVA_BYTE, pathOff + 4) == 't' &&
            buf.get(JAVA_BYTE, pathOff + 5) == 's') {

            if (!isPost) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_405, Out.RESP_405_LEN, connClose);
            }
            if (authOff < 0 || !isBearer(buf, authOff, authLen)) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_401_MISSING, Out.RESP_401_MISSING_LEN, connClose);
            }
            long userId = Jwt.verifyAuthHeader(buf, authOff, authLen, nowEpochSec, jwtScratch);
            if (userId == -2L) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_401_PAYLOAD, Out.RESP_401_PAYLOAD_LEN, connClose);
            }
            if (userId <= 0L) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_401_INVALID, Out.RESP_401_INVALID_LEN, connClose);
            }
            if (bodyLen <= 0) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_400_MALFORMED, Out.RESP_400_MALFORMED_LEN, connClose);
            }
            long bodyHandle = Json.parsePostBody(buf, bodyOff, bodyLen, unescapeScratch);
            if (bodyHandle == -3L) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_400_BODY_TOO_LONG, Out.RESP_400_BODY_TOO_LONG_LEN, connClose);
            }
            if (bodyHandle == -2L) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_400_BODY_REQUIRED, Out.RESP_400_BODY_REQUIRED_LEN, connClose);
            }
            if (bodyHandle < 0L) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_400_MALFORMED, Out.RESP_400_MALFORMED_LEN, connClose);
            }

            MemorySegment postSeg = Json.isUnescapedInScratch(bodyHandle) ? unescapeScratch : buf;
            long postOff          = Json.sliceOffset(bodyHandle);
            int postLen           = Json.sliceLength(bodyHandle);

            Pg.PgConn pg = selectWriteConn(64 + postLen);
            if (pg == null) {
                return sendStaticAndMaybeClose(clientFd, Out.RESP_503, Out.RESP_503_LEN, connClose);
            }
            pg.enqueueInsertPost(
                clientFd, reqSeq, gzip, userId,
                postSeg, postOff, postLen,
                jwtScratch, Jwt.usernameOffset(jwtScratch), Jwt.usernameLength(jwtScratch)
            );
            dirtyWriteMask |= (1L << pg.connIndex);
            return true;
        }

        // Route 4 & 5: /posts/:id or /posts/:id/like (starts with "/posts/", length >= 8)
        if (pathLen >= 8 &&
            buf.get(JAVA_BYTE, pathOff + 1) == 'p' &&
            buf.get(JAVA_BYTE, pathOff + 2) == 'o' &&
            buf.get(JAVA_BYTE, pathOff + 3) == 's' &&
            buf.get(JAVA_BYTE, pathOff + 4) == 't' &&
            buf.get(JAVA_BYTE, pathOff + 5) == 's' &&
            buf.get(JAVA_BYTE, pathOff + 6) == '/') {

            // Check if it ends with "/like" (5 chars)
            boolean isLikeRoute = (pathLen >= 13 &&
                buf.get(JAVA_BYTE, pathOff + pathLen - 5) == '/' &&
                buf.get(JAVA_BYTE, pathOff + pathLen - 4) == 'l' &&
                buf.get(JAVA_BYTE, pathOff + pathLen - 3) == 'i' &&
                buf.get(JAVA_BYTE, pathOff + pathLen - 2) == 'k' &&
                buf.get(JAVA_BYTE, pathOff + pathLen - 1) == 'e');

            int idStart = pathOff + 7;
            int idEnd = isLikeRoute ? (pathOff + pathLen - 5) : (pathOff + pathLen);

            // Check if there are any other '/' characters inside the ID segment -> 404 Not Found
            for (int k = idStart; k < idEnd; k++) {
                if (buf.get(JAVA_BYTE, k) == '/') {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_404_ROUTE, Out.RESP_404_ROUTE_LEN, connClose);
                }
            }

            if (isLikeRoute) {
                if (!isPost) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_405, Out.RESP_405_LEN, connClose);
                }
                if (authOff < 0 || !isBearer(buf, authOff, authLen)) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_401_MISSING, Out.RESP_401_MISSING_LEN, connClose);
                }
                long userId = Jwt.verifyAuthHeader(buf, authOff, authLen, nowEpochSec, jwtScratch);
                if (userId == -2L) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_401_PAYLOAD, Out.RESP_401_PAYLOAD_LEN, connClose);
                }
                if (userId <= 0L) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_401_INVALID, Out.RESP_401_INVALID_LEN, connClose);
                }
                long postId = parsePathId(buf, idStart, idEnd - idStart);
                if (postId <= 0L) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_400_BAD_ID, Out.RESP_400_BAD_ID_LEN, connClose);
                }
                Pg.PgConn pg = selectWriteConn(64);
                if (pg == null) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_503, Out.RESP_503_LEN, connClose);
                }
                pg.enqueueLikePost(clientFd, reqSeq, gzip, userId, postId);
                dirtyWriteMask |= (1L << pg.connIndex);
                return true;
            } else {
                // GET /posts/:id
                if (!isGet) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_405, Out.RESP_405_LEN, connClose);
                }
                long postId = parsePathId(buf, idStart, idEnd - idStart);
                if (postId <= 0L) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_400_BAD_ID, Out.RESP_400_BAD_ID_LEN, connClose);
                }
                Pg.PgConn pg = selectReadConn(48);
                if (pg == null) {
                    return sendStaticAndMaybeClose(clientFd, Out.RESP_503, Out.RESP_503_LEN, connClose);
                }
                pg.enqueueGetPost(clientFd, reqSeq, gzip, postId);
                dirtyReadMask |= (1L << pg.connIndex);
                return true;
            }
        }

        // Unknown route -> 404 Not Found
        return sendStaticAndMaybeClose(clientFd, Out.RESP_404_ROUTE, Out.RESP_404_ROUTE_LEN, connClose);
    }

    private static long parsePathId(MemorySegment buf, int off, int len) {
        if (len <= 0 || len > 18) return -1L;
        long val = 0L;
        for (int i = 0; i < len; i++) {
            int d = buf.get(JAVA_BYTE, off + i) - '0';
            if (d < 0 || d > 9) return -1L;
            val = val * 10L + d;
        }
        return val > 0L ? val : -1L;
    }

    private Pg.PgConn selectReadConn(int extraTxBytes) {
        int len = readPool.length;
        Pg.PgConn best = null;
        int bestLoad = Integer.MAX_VALUE;
        for (int i = 0; i < len; i++) {
            int idx = (readRrCursor + i) % len;
            Pg.PgConn c = readPool[idx];
            if (c.canEnqueue(extraTxBytes)) {
                // Mid-tick eager flush: if connection accumulated 16 unsynced queries, flush immediately
                if (c.unsyncedQueries >= 16) {
                    if (!c.flushIfNeeded(errnoSeg)) {
                        armEpollOut(c, TAG_PG_READ);
                    }
                }
                int load = c.inFlightCount();
                if (load == 0) {
                    readRrCursor = idx + 1;
                    return c;
                }
                if (load < bestLoad) {
                    bestLoad = load;
                    best = c;
                }
            }
        }
        readRrCursor++;
        if (best != null && best.unsyncedQueries >= 16) {
            if (!best.flushIfNeeded(errnoSeg)) {
                armEpollOut(best, TAG_PG_READ);
            }
        }
        return best;
    }

    private Pg.PgConn selectWriteConn(int extraTxBytes) {
        int len = writePool.length;
        // Coalesce onto the current write connection only up to 16 unsynced writes
        int curIdx = writeRrCursor % len;
        Pg.PgConn cur = writePool[curIdx];
        if (cur.canEnqueue(extraTxBytes) && cur.unsyncedQueries < 16) {
            return cur;
        }
        // Connection reached 16 unsynced writes: flush immediately and round-robin to keep all backends busy
        if (cur.unsyncedQueries >= 16) {
            if (!cur.flushIfNeeded(errnoSeg)) {
                armEpollOut(cur, TAG_PG_WRITE);
            }
        }
        writeRrCursor++;

        Pg.PgConn best = null;
        int bestLoad = Integer.MAX_VALUE;
        for (int i = 0; i < len; i++) {
            int idx = (writeRrCursor + i) % len;
            Pg.PgConn c = writePool[idx];
            if (c.canEnqueue(extraTxBytes)) {
                if (c.unsyncedQueries >= 16) {
                    if (!c.flushIfNeeded(errnoSeg)) {
                        armEpollOut(c, TAG_PG_WRITE);
                    }
                }
                int load = c.inFlightCount();
                if (load < bestLoad) {
                    bestLoad = load;
                    best = c;
                }
            }
        }
        writeRrCursor++;
        return best;
    }

    // =========================================================================
    // PostgreSQL Completion Callbacks (Pg.CompletionListener)
    // =========================================================================

    @Override
    public void onHealthResult(int clientFd, int reqSeq, boolean dbOk) {
        if (!isClientRequestValid(clientFd, reqSeq)) return;
        boolean closeAfter = (reqSeq & FLAG_CLOSE_NEXT) != 0;

        long uptime = nowEpochSec - serverStartEpochSec;
        long endPos = Json.writeHealthResponse(jsonScratch, Out.BODY_OFFSET, dbOk, uptime);
        int jsonLen = (int) (endPos - Out.BODY_OFFSET);

        long packed = dbOk
                ? Out.frame200Plain(jsonScratch, jsonLen)
                : Out.frame503Plain(jsonScratch, jsonLen);
        sendDynamicAndMaybeClose(clientFd, closeAfter, jsonScratch, packed);
    }

    @Override
    public void onFeedOrPostResult(
            int clientFd, int reqSeq, byte reqType, boolean gzip,
            boolean sqlError, int rowCount,
            MemorySegment jsonBuf, int jsonLen) {

        if (!isClientRequestValid(clientFd, reqSeq)) return;
        boolean closeAfter = (reqSeq & FLAG_CLOSE_NEXT) != 0;

        if (sqlError) {
            sendStaticAndMaybeClose(clientFd, Out.RESP_500, Out.RESP_500_LEN, closeAfter);
            return;
        }
        if (reqType == Pg.REQ_GET_POST && rowCount == 0) {
            sendStaticAndMaybeClose(clientFd, Out.RESP_404_POST, Out.RESP_404_POST_LEN, closeAfter);
            return;
        }

        if (gzip) {
            long packed = Out.frame200Gzip(zstreamSlab, jsonBuf, jsonLen, outScratch);
            sendDynamicAndMaybeClose(clientFd, closeAfter, outScratch, packed);
        } else {
            long packed = Out.frame200Plain(jsonBuf, jsonLen);
            sendDynamicAndMaybeClose(clientFd, closeAfter, jsonBuf, packed);
        }
    }

    @Override
    public void onInsertPostResult(
            int clientFd, int reqSeq, boolean gzip, boolean sqlError,
            MemorySegment pgRxBuf, long idOff, int idLen,
            long createdOff, int createdLen,
            MemorySegment reqCopyBuf, int bodyOff, int bodyLen,
            int authorOff, int authorLen) {

        if (!isClientRequestValid(clientFd, reqSeq)) return;
        boolean closeAfter = (reqSeq & FLAG_CLOSE_NEXT) != 0;

        if (sqlError) {
            sendStaticAndMaybeClose(clientFd, Out.RESP_500, Out.RESP_500_LEN, closeAfter);
            return;
        }

        long endPos = Json.writeCreatedPostObject(
            jsonScratch, Out.BODY_OFFSET,
            pgRxBuf, idOff, idLen,
            createdOff, createdLen,
            reqCopyBuf, bodyOff, bodyLen,
            authorOff, authorLen
        );
        int jsonLen = (int) (endPos - Out.BODY_OFFSET);

        if (gzip) {
            long packed = Out.frame201Gzip(zstreamSlab, jsonScratch, jsonLen, outScratch);
            sendDynamicAndMaybeClose(clientFd, closeAfter, outScratch, packed);
        } else {
            long packed = Out.frame201Plain(jsonScratch, jsonLen);
            sendDynamicAndMaybeClose(clientFd, closeAfter, jsonScratch, packed);
        }
    }

    @Override
    public void onLikePostResult(
            int clientFd, int reqSeq, boolean gzip,
            boolean fkNotFound, boolean sqlError,
            boolean insertedNew, long postId) {

        if (!isClientRequestValid(clientFd, reqSeq)) return;
        boolean closeAfter = (reqSeq & FLAG_CLOSE_NEXT) != 0;

        if (fkNotFound) {
            sendStaticAndMaybeClose(clientFd, Out.RESP_404_POST, Out.RESP_404_POST_LEN, closeAfter);
            return;
        }
        if (sqlError) {
            sendStaticAndMaybeClose(clientFd, Out.RESP_500, Out.RESP_500_LEN, closeAfter);
            return;
        }

        long endPos = Json.writeLikeResponse(jsonScratch, Out.BODY_OFFSET, !insertedNew, postId);
        int jsonLen = (int) (endPos - Out.BODY_OFFSET);

        if (insertedNew) {
            if (gzip) {
                long packed = Out.frame201Gzip(zstreamSlab, jsonScratch, jsonLen, outScratch);
                sendDynamicAndMaybeClose(clientFd, closeAfter, outScratch, packed);
            } else {
                long packed = Out.frame201Plain(jsonScratch, jsonLen);
                sendDynamicAndMaybeClose(clientFd, closeAfter, jsonScratch, packed);
            }
        } else {
            if (gzip) {
                long packed = Out.frame200Gzip(zstreamSlab, jsonScratch, jsonLen, outScratch);
                sendDynamicAndMaybeClose(clientFd, closeAfter, outScratch, packed);
            } else {
                long packed = Out.frame200Plain(jsonScratch, jsonLen);
                sendDynamicAndMaybeClose(clientFd, closeAfter, jsonScratch, packed);
            }
        }
    }

    private boolean isClientRequestValid(int clientFd, int reqToken) {
        if (clientFd <= 0 || clientFd >= MAX_FD) return false;
        int state = fdStateTable.get(JAVA_INT_UNALIGNED, (long) clientFd << 2);
        return (state & FLAG_OPEN) != 0 && (state & SEQ_MASK) == (reqToken & SEQ_MASK);
    }

    private void sendDynamicAndMaybeClose(
            int clientFd, boolean closeAfter, MemorySegment seg, long packedHandle) {

        long off = Out.packetOffset(packedHandle);
        int len  = Out.packetLength(packedHandle);
        long sent = Sys.send(clientFd, seg, off, len);
        if (sent < len || closeAfter) {
            closeClient(clientFd);
        }
    }

    private boolean sendStaticAndMaybeClose(
            int clientFd, MemorySegment staticResp, int len, boolean closeAfter) {

        long sent = Sys.send(clientFd, staticResp, 0L, len);
        if (sent < len || closeAfter) {
            closeClient(clientFd);
            return false;
        }
        return true;
    }
}
