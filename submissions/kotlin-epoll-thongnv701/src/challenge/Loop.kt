package challenge

import java.io.IOException
import java.lang.foreign.Arena
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.net.InetAddress
import java.util.ArrayDeque
import java.util.concurrent.locks.LockSupport

class LoopConfig(
    @JvmField val readers: Int,
    @JvmField val writers: Int,
    @JvmField val packMax: Int,
    @JvmField val gzipLevel: Int,
    @JvmField val gzipMin: Int,
    @JvmField val batchMin: Int,
    @JvmField val batchNanos: Long,
)

private const val READ_CAP = 64 * 1024
private const val MAX_HEAD = 16 * 1024
private const val MAX_BODY = 32 * 1024
private const val OUT_CAP = 1 shl 20
private const val OUT_RESERVE = 128 * 1024
private const val PG_IO = 256 * 1024
private const val MAX_EVENTS = 4096
private const val ACCEPT_BURST = 256
private const val MAX_BATCH = 256
private const val RECONNECT_NANOS = 1_000_000_000L
private const val LATE_ACK_MILLIS = 25
private const val HEDGE_MILLIS = 10
private const val TAG_CONN = 0
private const val TAG_LISTEN = 1
private const val TAG_PG = 2

private const val NOT_FOUND = 0
private const val HEALTH = 1
private const val FEED = 2
private const val GET_POST = 3
private const val CREATE = 4
private const val LIKE = 5

private const val INCOMPLETE = 0
private const val MALFORMED = -1
private const val TOO_LARGE = -2

private fun err(message: String): ByteArray = ascii("{\"error\":\"$message\"}")

private val E_NOT_FOUND = err("not found")
private val E_INVALID_ID = err("invalid post id")
private val E_NO_POST = err("post not found")
private val E_MISSING_TOKEN = err("missing bearer token")
private val E_INVALID_TOKEN = err("invalid or expired token")
private val E_BAD_PAYLOAD = err("invalid token payload")
private val E_MALFORMED = err("malformed JSON body")
private val E_BODY_REQUIRED = err("body is required")
private val E_BODY_TOO_LONG = err("body must be at most 500 characters")
private val E_INTERNAL = err("internal server error")
private val E_BAD_REQUEST = err("bad request")
private val E_TOO_LARGE = err("request too large")
private val E_NOT_IMPLEMENTED = err("not implemented")
private val LIKED_NEW = ascii("{\"liked\":true,\"already_liked\":false,\"post_id\":")
private val LIKED_AGAIN = ascii("{\"liked\":true,\"already_liked\":true,\"post_id\":")
private val HEALTH_OK = ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":")
private val FEED_OPEN = ascii("{\"posts\":[")
private val FEED_CLOSE = ascii("]}")
private val POST_OPEN = ascii("{\"post\":")
private val POST_CLOSE = ascii("}")
private val P_ID = ascii("{\"id\":")
private val P_BODY = ascii(",\"body\":\"")
private val P_CREATED = ascii("\",\"created_at\":\"")
private val P_AUTHOR = ascii("\",\"author\":\"")
private val P_LIKES = ascii("\",\"like_count\":")
private val CLOSE_BRACE = byteArrayOf('}'.code.toByte())

@Volatile private var nodelayLogged = false

/** A post row (id text, body, created_at binary, username, like_count text) as the SPEC's post object. */
private fun post(o: Buf, b: ByteArray, start: Int) {
    var p = start
    val idLen = i32(b, p)
    val idAt = p + 4
    p = idAt + idLen
    val bodyLen = i32(b, p)
    val bodyAt = p + 4
    p = bodyAt + bodyLen
    val ts = i64(b, p + 4)
    p += 4 + i32(b, p)
    val userLen = i32(b, p)
    val userAt = p + 4
    p = userAt + userLen
    val likesLen = i32(b, p)
    val likesAt = p + 4
    o.put(P_ID)
    o.put(b, idAt, idLen)
    o.put(P_BODY)
    o.escaped(b, bodyAt, bodyLen)
    o.put(P_CREATED)
    o.timestamp(ts)
    o.put(P_AUTHOR)
    o.escaped(b, userAt, userLen)
    o.put(P_LIKES)
    o.put(b, likesAt, likesLen)
    o.put('}'.code)
}

/**
 * One event loop thread: its own listening sockets on every port (SO_REUSEPORT), its own clients and its own
 * Postgres connections, all in one epoll set. Nothing is shared with other loops.
 */
class Loop(
    private val id: Int,
    db: PgConfig,
    private val cfg: LoopConfig,
    private val jwt: Jwt,
    host: InetAddress,
    ports: IntArray,
    private val startNanos: Long,
) : Runnable, PgHandler {
    private val sys = Sys()
    private val ep: Int
    private val listenFds: IntArray
    private var nodelayChecked = false
    private var nodelayInherited = false
    private val pgs: Array<Pg>
    private val readers: Int = cfg.readers
    private var pack: Pg? = null
    private var nowMillis = 0

    private val events: MemorySegment = Arena.global().allocate(MAX_EVENTS.toLong() * EVENT_SIZE, 8)
    private val inSeg: MemorySegment = Arena.global().allocate(READ_CAP.toLong())
    private val wbuf: MemorySegment = Arena.global().allocate(OUT_CAP.toLong())
    private val pgIo: MemorySegment = Arena.global().allocate(PG_IO.toLong())
    private val rb = ByteArray(READ_CAP + MAX_HEAD + MAX_BODY)
    private val out = Out(OUT_CAP)
    private val scratch = Buf(4096)
    private val gzip: Gzip? = if (cfg.gzipLevel > 0) Gzip(2 * OUT_RESERVE) else null
    private val gzipMin: Int = cfg.gzipMin
    private var byFd = arrayOfNulls<Conn>(1024)

    private val writes = ArrayDeque<Conn>()
    private val solo = ArrayDeque<Conn>()
    private val batch = arrayOfNulls<Conn>(MAX_BATCH)
    private val dirty = ArrayList<Pg>()
    private var lastAcceptError = 0L
    private var lastConnectFailure = 0L
    private var connectFailed = false

    private var pathFrom = 0
    private var pathTo = 0
    private var authFrom = 0
    private var authTo = 0
    private var bodyFrom = 0
    private var bodyLen = 0
    private var chunkedLen = 0

    init {
        pgs = Array(cfg.readers + cfg.writers) { i -> Pg(i, i < cfg.readers, db, sys) }
        ep = sys.epollCreate()
        listenFds = IntArray(ports.size) { i -> sys.listen(host, ports[i], 65535) }
    }

    override fun run() {
        for (fd in listenFds) sys.epollCtl(ep, EPOLL_CTL_ADD, fd, EPOLLIN, TAG_LISTEN)
        for (pg in pgs) reconnect(pg)
        while (true) {
            var n = sys.epollWait(ep, events, MAX_EVENTS, 1000)
            tick()
            if (n < 0) {
                if (sys.errno() != EINTR) throw IllegalStateException("epoll_wait: errno ${sys.errno()}")
                n = 0
            }
            for (i in 0 until n) {
                val fd = eventFd(events, i)
                val tag = eventTag(events, i)
                val flags = eventFlags(events, i)
                if (tag == TAG_CONN) onConn(fd, flags)
                else if (tag == TAG_LISTEN) accept(fd)
                else onPg(pgs[tag - TAG_PG], fd, flags)
            }
            pumpWrites()
            flushPgs()
            // Postgres has its packets; a light wake-up waits so the next one carries more.
            if (cfg.batchNanos > 0 && n > 0 && n < cfg.batchMin) LockSupport.parkNanos(cfg.batchNanos)
        }
    }

    private fun tick() {
        nowMillis = (System.nanoTime() / 1_000_000L).toInt()
    }

    private fun accept(listenFd: Int) {
        for (k in 0 until ACCEPT_BURST) {
            val fd = sys.accept(listenFd)
            if (fd < 0) {
                val e = sys.errno()
                if (e == EINTR || e == ECONNABORTED) continue
                if (e != EAGAIN) acceptError(e)
                return
            }
            accepted(fd)
        }
    }

    private fun acceptError(errno: Int) {
        if (errno == ECONNABORTED || System.nanoTime() - lastAcceptError < 1_000_000_000L) return
        lastAcceptError = System.nanoTime()
        System.err.println("loop $id: accept: errno $errno")
    }

    private fun accepted(fd: Int) {
        if (!nodelayChecked) {
            nodelayChecked = true
            nodelayInherited = sys.getInt(fd, IPPROTO_TCP, TCP_NODELAY) > 0
            if (!nodelayLogged) {
                nodelayLogged = true
                System.err.println("TCP_NODELAY " + if (nodelayInherited) "inherited from the listening socket" else "set per socket")
            }
        }
        if (!nodelayInherited) sys.setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1)
        if (sys.epollCtl(ep, EPOLL_CTL_ADD, fd, EPOLLIN, TAG_CONN) != 0) {
            closeFd(fd)
            return
        }
        if (fd >= byFd.size) byFd = byFd.copyOf(maxOf(fd + 1, byFd.size * 2))
        byFd[fd] = Conn(fd)
    }

    private fun armWritable(c: Conn) {
        sys.epollCtl(ep, EPOLL_CTL_MOD, c.fd, EPOLLOUT, TAG_CONN)
    }

    private fun onConn(fd: Int, flags: Int) {
        val c = (if (fd < byFd.size) byFd[fd] else null) ?: return
        try {
            if (c.pending != null) {
                if ((flags and (EPOLLOUT or EPOLLERR or EPOLLHUP)) != 0) onWritable(c)
            } else if ((flags and (EPOLLIN or EPOLLERR or EPOLLHUP)) != 0) {
                onReadable(c)
            }
        } catch (t: Throwable) {
            System.err.println("loop $id: connection error: $t")
            close(c)
        }
    }

    private fun close(c: Conn) {
        if (c.closed) return
        c.closed = true
        c.left = null
        c.pending = null
        byFd[c.fd] = null
        closeFd(c.fd)
    }

    private fun retryable(errno: Int): Boolean = errno == EAGAIN || errno == EINTR

    private fun onReadable(c: Conn) {
        val have = c.left?.size ?: 0
        val room = minOf(READ_CAP, rb.size - have)
        if (room <= 0) {
            close(c)
            return
        }
        val r = sys.read(c.fd, inSeg, room.toLong())
        if (r <= 0) {
            if (r == 0L || !retryable(sys.errno())) close(c)
            return
        }
        onData(c, inSeg, r.toInt())
    }

    private fun onData(c: Conn, data: MemorySegment, n: Int) {
        val l = c.left
        val have = l?.size ?: 0
        if (have + n > rb.size) {
            close(c)
            return
        }
        if (l != null) System.arraycopy(l, 0, rb, 0, have)
        MemorySegment.copy(data, JAVA_BYTE, 0L, rb, have, n)
        c.left = null
        if (c.waiting || c.pending != null) {
            c.left = rb.copyOf(have + n)
            return
        }
        process(c, have + n)
    }

    private fun onWritable(c: Conn) {
        val p = c.pending!!
        val len = minOf(p.size - c.pendingFrom, OUT_CAP)
        MemorySegment.copy(p, c.pendingFrom, wbuf, JAVA_BYTE, 0L, len)
        val w = sys.write(c.fd, wbuf, len.toLong())
        if (w < 0) {
            if (!retryable(sys.errno())) close(c)
            return
        }
        c.pendingFrom += w.toInt()
        if (c.pendingFrom < p.size) return
        c.pending = null
        c.pendingFrom = 0
        if (c.closeAfterWrite && !c.waiting) {
            close(c)
            return
        }
        sys.epollCtl(ep, EPOLL_CTL_MOD, c.fd, EPOLLIN, TAG_CONN)
        resume(c)
    }

    /** Replays pipelined bytes that arrived while the connection waited on Postgres or a slow socket. */
    private fun resume(c: Conn) {
        if (c.closed || c.waiting || c.pending != null) return
        val l = c.left ?: return
        c.left = null
        System.arraycopy(l, 0, rb, 0, l.size)
        process(c, l.size)
    }

    private fun send(c: Conn, from: Int, to: Int) {
        if (c.closed || from == to) return
        val len = to - from
        val pending = c.pending
        if (pending != null) {
            val rest = pending.size - c.pendingFrom
            val p = ByteArray(rest + len)
            if (rest > 0) System.arraycopy(pending, c.pendingFrom, p, 0, rest)
            MemorySegment.copy(out.seg, JAVA_BYTE, from.toLong(), p, rest, len)
            c.pending = p
            c.pendingFrom = 0
            return
        }
        var w = sys.write(c.fd, out.seg.asSlice(from.toLong(), len.toLong()), len.toLong())
        if (w < 0) {
            if (!retryable(sys.errno())) {
                close(c)
                return
            }
            w = 0
        }
        if (w < len) {
            val p = ByteArray(len - w.toInt())
            MemorySegment.copy(out.seg, JAVA_BYTE, from + w, p, 0, p.size)
            c.pending = p
            c.pendingFrom = 0
            armWritable(c)
        } else if (c.closeAfterWrite && !c.waiting) {
            close(c)
        }
    }

    private fun flush(c: Conn) {
        val n = out.pos
        out.pos = 0
        send(c, 0, n)
    }

    private fun process(c: Conn, end: Int) {
        val b = rb
        var p = 0
        while (p < end && !c.closed) {
            if (out.free() < OUT_RESERVE) {
                flush(c)
                if (c.pending != null || c.closed) break
            }
            val used = request(c, b, p, end)
            if (used == 0) break
            p += used
            if (c.closeAfterWrite) p = end
            if (c.waiting) break
        }
        c.left = if (!c.closed && p < end) b.copyOfRange(p, end) else null
        flush(c)
    }

    /** Parses one request at b[p, end) and handles it; returns the bytes it used, 0 if it is incomplete. */
    private fun request(c: Conn, b: ByteArray, p: Int, end: Int): Int {
        val he = headerEnd(b, p, end)
        if (he < 0) {
            if (end - p > MAX_HEAD) return reject(c, S431, E_TOO_LARGE, end - p)
            return 0
        }
        val sp1 = indexOfByte(b, ' '.code, p, he)
        val sp2 = if (sp1 < 0) -1 else indexOfByte(b, ' '.code, sp1 + 1, he)
        val eol = if (sp2 < 0) -1 else indexOfByte(b, '\r'.code, sp2 + 1, he)
        if (eol < 0 || eol - sp2 - 1 != 8 || !startsWith(b, sp2 + 1, "HTTP/1.")) {
            return reject(c, S400, E_BAD_REQUEST, end - p)
        }
        val http11 = b[sp2 + 8].toInt() == '1'.code
        val isGet = sp1 - p == 3 && b[p].toInt() == 'G'.code && b[p + 1].toInt() == 'E'.code && b[p + 2].toInt() == 'T'.code
        val isPost = sp1 - p == 4 && b[p].toInt() == 'P'.code && b[p + 1].toInt() == 'O'.code &&
            b[p + 2].toInt() == 'S'.code && b[p + 3].toInt() == 'T'.code
        pathFrom = sp1 + 1
        pathTo = sp2
        val q = indexOfByte(b, '?'.code, pathFrom, pathTo)
        if (q >= 0) pathTo = q

        authFrom = -1
        authTo = -1
        var contentLength = 0L
        var connClose = false
        var connKeepAlive = false
        var hasLength = false
        var acceptGzip = false
        var teFrom = -1
        var teTo = -1
        var line = eol + 2
        while (line < he - 2) {
            val le = indexOfByte(b, '\r'.code, line, he)
            val colon = indexOfByte(b, ':'.code, line, le)
            if (colon > line) {
                var vs = colon + 1
                var ve = le
                while (vs < ve && (b[vs].toInt() == ' '.code || b[vs].toInt() == '\t'.code)) vs++
                while (ve > vs && (b[ve - 1].toInt() == ' '.code || b[ve - 1].toInt() == '\t'.code)) ve--
                val nl = colon - line
                if (nl == 13 && nameIs(b, line, "authorization")) {
                    authFrom = vs
                    authTo = ve
                } else if (nl == 14 && nameIs(b, line, "content-length")) {
                    contentLength = 0
                    hasLength = true
                    if (vs == ve) return reject(c, S400, E_BAD_REQUEST, end - p)
                    for (k in vs until ve) {
                        val digit = b[k] >= '0'.code.toByte() && b[k] <= '9'.code.toByte()
                        if (!digit) return reject(c, S400, E_BAD_REQUEST, end - p)
                        if (contentLength > MAX_BODY) return reject(c, S413, E_TOO_LARGE, end - p)
                        contentLength = contentLength * 10 + (b[k] - '0'.code.toByte())
                    }
                } else if (nl == 10 && nameIs(b, line, "connection")) {
                    if (containsIgnoreCase(b, vs, ve, "close")) connClose = true
                    if (containsIgnoreCase(b, vs, ve, "keep-alive")) connKeepAlive = true
                } else if (nl == 15 && nameIs(b, line, "accept-encoding")) {
                    acceptGzip = containsIgnoreCase(b, vs, ve, "gzip")
                } else if (nl == 17 && nameIs(b, line, "transfer-encoding")) {
                    if (teFrom >= 0) return reject(c, S400, E_BAD_REQUEST, end - p)
                    teFrom = vs
                    teTo = ve
                }
            }
            line = le + 2
        }
        val msgEnd: Int
        if (teFrom >= 0) {
            if (!http11 || hasLength) return reject(c, S400, E_BAD_REQUEST, end - p)
            if (teTo - teFrom != 7 || !nameIs(b, teFrom, "chunked")) {
                val last = teTo - teFrom > 7 && nameIs(b, teTo - 7, "chunked")
                return reject(c, if (last) S501 else S400, if (last) E_NOT_IMPLEMENTED else E_BAD_REQUEST, end - p)
            }
            msgEnd = chunked(b, he, end, false)
            if (msgEnd == MALFORMED) return reject(c, S400, E_BAD_REQUEST, end - p)
            if (msgEnd == TOO_LARGE) return reject(c, S413, E_TOO_LARGE, end - p)
            if (msgEnd == INCOMPLETE) return 0
            bodyLen = chunkedLen
            chunked(b, he, msgEnd, true)
        } else {
            if (contentLength > MAX_BODY) return reject(c, S413, E_TOO_LARGE, end - p)
            msgEnd = he + contentLength.toInt()
            if (end < msgEnd) return 0
            bodyLen = contentLength.toInt()
        }
        bodyFrom = he

        c.closeAfterWrite = if (http11) connClose else !connKeepAlive
        c.gzip = acceptGzip
        out.close = c.closeAfterWrite
        val mark = out.pos
        try {
            handle(c, route(b, isGet, isPost), b)
        } catch (t: Throwable) {
            System.err.println("loop $id: request failed: $t")
            out.pos = mark
            out.respond(S500, E_INTERNAL)
        }
        return msgEnd - p
    }

    /** Validates (compact = false) or decodes in place (compact = true) a chunked body starting at `from`. */
    private fun chunked(b: ByteArray, from: Int, end: Int, compact: Boolean): Int {
        var k = from
        var len = 0
        while (true) {
            var size = 0
            val start = k
            while (k < end) {
                val d = Character.digit(b[k].toInt(), 16)
                if (d < 0) break
                size = size * 16 + d
                if (len + size > MAX_BODY) return TOO_LARGE
                k++
            }
            if (k == end) return INCOMPLETE
            val x = b[k].toInt()
            if (k == start || (x != '\r'.code && x != ';'.code && x != ' '.code && x != '\t'.code)) return MALFORMED
            val eol = indexOfByte(b, '\r'.code, k, end)
            if (eol < 0 || eol + 1 == end) return INCOMPLETE
            if (b[eol + 1].toInt() != '\n'.code) return MALFORMED
            k = eol + 2
            if (size == 0) break
            if (end - k < size + 2) return INCOMPLETE
            if (b[k + size].toInt() != '\r'.code || b[k + size + 1].toInt() != '\n'.code) return MALFORMED
            if (compact) System.arraycopy(b, k, b, from + len, size)
            len += size
            k += size + 2
        }
        while (true) {
            val eol = indexOfByte(b, '\r'.code, k, end)
            if (eol < 0 || eol + 1 == end) return INCOMPLETE
            if (b[eol + 1].toInt() != '\n'.code) return MALFORMED
            if (eol == k) {
                chunkedLen = len
                return eol + 2
            }
            k = eol + 2
        }
    }

    private fun reject(c: Conn, status: ByteArray, body: ByteArray, consumed: Int): Int {
        c.closeAfterWrite = true
        out.close = true
        out.respond(status, body)
        return maxOf(consumed, 1)
    }

    private fun route(b: ByteArray, isGet: Boolean, isPost: Boolean): Int {
        val len = pathTo - pathFrom
        if (isGet) {
            if (len == 7 && startsWith(b, pathFrom, "/health")) return HEALTH
            if (len == 5 && startsWith(b, pathFrom, "/feed")) return FEED
            if (len > 7 && startsWith(b, pathFrom, "/posts/") && indexOfByte(b, '/'.code, pathFrom + 7, pathTo) < 0) {
                return GET_POST
            }
            return NOT_FOUND
        }
        if (isPost) {
            if (len == 6 && startsWith(b, pathFrom, "/posts")) return CREATE
            if (len > 12 && startsWith(b, pathFrom, "/posts/") && endsWith(b, pathTo, "/like") &&
                indexOfByte(b, '/'.code, pathFrom + 7, pathTo - 5) < 0
            ) {
                return LIKE
            }
        }
        return NOT_FOUND
    }

    private fun handle(c: Conn, route: Int, b: ByteArray) {
        when (route) {
            HEALTH -> enqueueRead(c, OP_PING)
            FEED -> enqueueRead(c, OP_FEED)
            GET_POST -> {
                val id = parseId(b, pathFrom + 7, pathTo)
                if (id == -1L) out.respond(S400, E_INVALID_ID)
                else if (id == -2L) out.respond(S404, E_NO_POST)
                else {
                    c.id = id
                    enqueueRead(c, OP_POST)
                }
            }
            CREATE -> create(c, b)
            LIKE -> like(c, b)
            else -> out.respond(S404, E_NOT_FOUND)
        }
    }

    private fun auth(b: ByteArray): Boolean {
        val r = jwt.verify(b, authFrom, authTo)
        if (r == JWT_OK) return true
        out.respond(S401, if (r == JWT_MISSING) E_MISSING_TOKEN else if (r == JWT_INVALID) E_INVALID_TOKEN else E_BAD_PAYLOAD)
        return false
    }

    private fun create(c: Conn, b: ByteArray) {
        if (!auth(b)) return
        val doc = try {
            Json.parse(utf8(b, bodyFrom, bodyLen))
        } catch (e: Malformed) {
            out.respond(S400, E_MALFORMED)
            return
        }
        val raw = jsonGet(doc, "body")
        if (raw !is String) {
            out.respond(S400, E_BODY_REQUIRED)
            return
        }
        val body = jsTrim(raw)
        if (body.isEmpty()) {
            out.respond(S400, E_BODY_REQUIRED)
            return
        }
        if (body.codePointCount(0, body.length) > 500) {
            out.respond(S400, E_BODY_TOO_LONG)
            return
        }
        c.userId = jwt.userId
        c.user = jwt.username
        c.text = body.toByteArray(Charsets.UTF_8)
        enqueueWrite(c, OP_CREATE)
    }

    private fun like(c: Conn, b: ByteArray) {
        if (!auth(b)) return
        val id = parseId(b, pathFrom + 7, pathTo - 5)
        if (id == -1L) {
            out.respond(S400, E_INVALID_ID)
            return
        }
        if (id == -2L) {
            out.respond(S404, E_NO_POST)
            return
        }
        c.userId = jwt.userId
        c.id = id
        enqueueWrite(c, OP_LIKE)
    }

    private fun enqueueRead(c: Conn, op: Byte) {
        val pg = packReader()
        if (pg == null) {
            if (op == OP_PING) degraded(pgs[0].lastError)
            else out.respond(S500, E_INTERNAL)
            return
        }
        c.op = op
        c.rows = 0
        c.waiting = true
        c.since = nowMillis
        pg.pack(c)
        markDirty(pg)
    }

    private fun enqueueWrite(c: Conn, op: Byte) {
        c.op = op
        c.rows = 0
        c.waiting = true
        c.since = nowMillis
        writes.add(c)
    }

    /** The reader whose pack this loop pass's reads join: one Sync per pack. */
    private fun packReader(): Pg? {
        val pg = pack
        if (pg != null && pg.alive() && pg.packSize() < cfg.packMax) return pg
        if (pg != null && pg.alive()) pg.closePack()
        val next = pickReader()
        pack = next
        return next
    }

    private fun pickReader(): Pg? {
        var best: Pg? = null
        for (i in 0 until readers) {
            val pg = pgs[i]
            if (pg.alive() && (best == null || pg.inFlight() < best.inFlight())) best = pg
        }
        if (best != null) return best
        for (i in 0 until readers) if (reconnect(pgs[i])) return pgs[i]
        return null
    }

    /**
     * Group commit: while a write transaction is in flight, new writes queue; the next idle writer takes the whole
     * queue as one transaction. A batch that has not committed within HEDGE_MILLIS no longer holds the queue back.
     */
    private fun pumpWrites() {
        if (writes.isEmpty() && solo.isEmpty()) return
        var writer: Pg? = null
        var inFlight = false
        for (i in readers until pgs.size) {
            val pg = pgs[i]
            if (!pg.alive()) continue
            if (pg.inFlight() == 0) {
                if (writer == null) writer = pg
            } else if (nowMillis - pg.sentMillis < HEDGE_MILLIS) {
                return
            } else {
                inFlight = true
            }
        }
        var i = readers
        while (writer == null && i < pgs.size) {
            if (!pgs[i].alive() && reconnect(pgs[i])) writer = pgs[i]
            i++
        }
        if (writer == null) {
            if (!inFlight) failWrites()
            return
        }
        writer.sentMillis = nowMillis
        if (!solo.isEmpty()) {
            while (!solo.isEmpty()) {
                val c = solo.poll()
                if (!c.closed) writer.query(c)
            }
        } else {
            var n = 0
            while (n < MAX_BATCH && !writes.isEmpty()) {
                val c = writes.poll()
                if (!c.closed) batch[n++] = c
            }
            if (n > 0) writer.batch(batch, n)
            batch.fill(null, 0, n)
        }
        markDirty(writer)
    }

    private fun failWrites() {
        val pg = pgs[readers]
        val failed = ArrayList<Conn>(solo)
        failed.addAll(writes)
        solo.clear()
        writes.clear()
        for (c in failed) finish(pg, c, "08006", pg.lastError)
    }

    private fun markDirty(pg: Pg) {
        if (pg.dirty) return
        pg.dirty = true
        dirty.add(pg)
    }

    private fun flushPgs() {
        pack = null
        for (i in 0 until dirty.size) {
            val pg = dirty[i]
            pg.dirty = false
            if (pg.alive()) {
                pg.closePack()
                flushPg(pg)
            }
        }
        dirty.clear()
    }

    private fun flushPg(pg: Pg) {
        if (!pg.flush(pgIo)) {
            fail(pg)
            return
        }
        val want = !pg.flushed()
        if (want != pg.wantWrite) {
            pg.wantWrite = want
            sys.epollCtl(ep, EPOLL_CTL_MOD, pg.fd, EPOLLIN or (if (want) EPOLLOUT else 0), TAG_PG + pg.tag)
        }
    }

    private fun onPg(pg: Pg, fd: Int, flags: Int) {
        if (pg.fd != fd) return
        try {
            if ((flags and EPOLLOUT) != 0) flushPg(pg)
            if (!pg.alive() || (flags and (EPOLLIN or EPOLLERR or EPOLLHUP)) == 0) return
            if (!pg.read(pgIo)) {
                fail(pg)
                return
            }
            pg.parse(this)
        } catch (t: Throwable) {
            System.err.println("loop $id: postgres connection error: $t")
            if (pg.alive()) {
                pg.lastError = t.toString()
                fail(pg)
            }
        }
    }

    private fun reconnect(pg: Pg): Boolean {
        if (pg.alive()) return true
        if (connectFailed && System.nanoTime() - lastConnectFailure < RECONNECT_NANOS) return false
        try {
            pg.connect(pgIo)
        } catch (e: IOException) {
            pg.lastError = e.message ?: e.toString()
            connectFailed = true
            lastConnectFailure = System.nanoTime()
            System.err.println("loop $id: postgres: ${pg.lastError}")
            return false
        }
        connectFailed = false
        sys.epollCtl(ep, EPOLL_CTL_ADD, pg.fd, EPOLLIN, TAG_PG + pg.tag)
        return true
    }

    private fun fail(pg: Pg) {
        System.err.println("loop $id: postgres connection lost: ${pg.lastError}")
        val lost = pg.close()
        for (c in lost) finish(pg, c, "08006", pg.lastError)
    }

    override fun row(pg: Pg, c: Conn, b: ByteArray, at: Int) {
        when (c.op) {
            OP_FEED, OP_POST -> {
                if (c.rows++ > 0) pg.body.put(','.code)
                post(pg.body, b, at + 2)
            }
            OP_CREATE -> {
                c.id = i64(b, at + 6)
                c.ts = i64(b, at + 18)
                c.rows = 1
            }
            else -> {}
        }
    }

    /** A like's CommandComplete: "INSERT 0 1" for a new like, "INSERT 0 0" when it was already there. */
    override fun command(c: Conn, b: ByteArray, at: Int, len: Int) {
        if (c.op != OP_LIKE) return
        var v = 0
        var m = 1
        var k = at + len
        while (k > at && b[k - 1] >= '0'.code.toByte() && b[k - 1] <= '9'.code.toByte()) {
            v += (b[--k] - '0'.code.toByte()) * m
            m *= 10
        }
        c.rows = v
    }

    override fun read(pg: Pg, c: Conn) {
        finish(pg, c, null, null)
        pg.body.len = 0
    }

    override fun done(pg: Pg, unit: Array<Conn?>, n: Int, code: String?, message: String?) {
        if (pg.reader) {
            // Postgres skipped everything after the failed read up to the Sync: those go out again.
            pg.body.len = 0
            finish(pg, unit[0]!!, code, message)
            for (i in 1 until n) retryRead(pg, unit[i]!!, code, message)
            return
        }
        if (code != null && n > 1) {
            // One failed write rolled back its batch: every write goes again in its own transaction.
            for (i in 0 until n) solo.add(unit[i]!!)
            return
        }
        for (i in 0 until n) finish(pg, unit[i]!!, code, message)
        pg.body.len = 0
    }

    private fun retryRead(failed: Pg, c: Conn, code: String?, message: String?) {
        if (c.closed) {
            c.waiting = false
            return
        }
        val pg = packReader()
        if (pg == null) {
            finish(failed, c, code, message)
            return
        }
        c.rows = 0
        pg.pack(c)
        markDirty(pg)
    }

    private fun finish(pg: Pg, c: Conn, code: String?, message: String?) {
        c.waiting = false
        if (c.closed) {
            c.text = null
            c.user = null
            return
        }
        out.pos = 0
        out.close = c.closeAfterWrite
        val op = c.op
        if (op == OP_PING) {
            if (code == null) healthy() else degraded(message)
        } else if (op == OP_LIKE && code == "23503") {
            out.respond(S404, E_NO_POST)
        } else if (code != null || (op == OP_CREATE && c.rows == 0)) {
            System.err.println("loop $id: postgres: $message ($code)")
            out.respond(S500, E_INTERNAL)
        } else {
            when (op) {
                OP_FEED -> respond(c, S200, FEED_OPEN, pg.body, FEED_CLOSE)
                OP_POST -> {
                    if (c.rows == 0) out.respond(S404, E_NO_POST)
                    else respond(c, S200, POST_OPEN, pg.body, POST_CLOSE)
                }
                OP_CREATE -> {
                    val s = scratch
                    val text = c.text!!
                    val user = c.user!!
                    s.len = 0
                    s.put(P_ID)
                    s.num(c.id)
                    s.put(P_BODY)
                    s.escaped(text, 0, text.size)
                    s.put(P_CREATED)
                    s.timestamp(c.ts)
                    s.put(P_AUTHOR)
                    s.escaped(user, 0, user.size)
                    s.put(P_LIKES)
                    s.put('0'.code)
                    s.put('}'.code)
                    respond(c, S201, POST_OPEN, s, POST_CLOSE)
                }
                OP_LIKE -> {
                    val prefix = if (c.rows > 0) LIKED_NEW else LIKED_AGAIN
                    out.head(if (c.rows > 0) S201 else S200, prefix.size + digitCount(c.id) + 1)
                    out.put(prefix)
                    out.putLong(c.id)
                    out.put(CLOSE_BRACE)
                }
                else -> out.respond(S500, E_INTERNAL)
            }
        }
        c.text = null
        c.user = null
        flush(c)
        // A reply that waited this long let the delayed-ACK timer fire, which turns quick ACKs back on.
        if (nowMillis - c.since >= LATE_ACK_MILLIS && !c.closed) sys.setInt(c.fd, IPPROTO_TCP, TCP_QUICKACK, 0)
        resume(c)
    }

    private fun respond(c: Conn, status: ByteArray, open: ByteArray, body: Buf, close: ByteArray) {
        val len = open.size + body.len + close.size
        val gz = gzip
        if (gz != null && c.gzip && len >= gzipMin) {
            gz.begin()
            gz.add(open, 0, open.size)
            gz.add(body.a, 0, body.len)
            gz.add(close, 0, close.size)
            val n = gz.end()
            out.head(status, n, true)
            out.put(gz.buf, 0, n)
            return
        }
        out.head(status, len)
        out.put(open)
        out.put(body.a, 0, body.len)
        out.put(close)
    }

    private fun healthy() {
        val up = (System.nanoTime() - startNanos) / 1_000_000_000L
        out.head(S200, HEALTH_OK.size + digitCount(up) + 1)
        out.put(HEALTH_OK)
        out.putLong(up)
        out.put(CLOSE_BRACE)
    }

    private fun degraded(error: String?) {
        val body = "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":${quote(error.toString())}}"
            .toByteArray(Charsets.UTF_8)
        out.respond(S503, body)
    }
}
