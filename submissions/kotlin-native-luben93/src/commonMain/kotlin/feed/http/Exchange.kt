package feed.http

import feed.json.JsonWriter
import feed.platform.Gzip
import feed.platform.Monitor
import feed.platform.nanoTime
import feed.platform.threadScratch
import feed.stats.C_BODY_OUT
import feed.stats.C_GZ_IN
import feed.stats.C_GZ_N
import feed.stats.C_GZ_NS
import feed.stats.C_GZ_OUT
import feed.stats.C_S2XX
import feed.stats.C_S4XX
import feed.stats.C_S5XX
import feed.stats.Stats
import feed.stats.BUCKETS
import feed.stats.bucketOf
import feed.util.ByteBuf
import kotlin.concurrent.atomics.AtomicInt
import kotlin.coroutines.Continuation
import kotlin.coroutines.CoroutineContext
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.jvm.JvmField

/**
 * Bytes in front of every response body where [putHead] writes the status line and headers (right-aligned), so
 * header + body leave in ONE write. Worst case is ~156 bytes (longest reason, 10-digit length, Connection and
 * Content-Encoding headers).
 */
internal const val RESERVE = 192

/** Response compression settings (Main sets them from GZIP_LEVEL / GZIP_MIN_BYTES before serving). */
object HttpConfig {
    /** zlib level 1-9; 0 turns compression off. */
    var gzipLevel = 1
    /** Bodies shorter than this go out plain (a post is ~150-330 bytes, where gzip saves little). */
    var gzipMinBytes = 1024
}

private const val OUT_INITIAL = 8192
/**
 * Pooled response buffers that grew past this (rare huge comment pages) are dropped instead of kept: the pool then
 * retains at most POOL x 64 KiB, however large the biggest responses are.
 */
private const val MAX_POOLED = 64 * 1024

internal val EMPTY = ByteArray(0)

/** A response buffer and the JSON writer over it, pooled together. */
internal class Out {
    @JvmField val buf = ByteBuf(OUT_INITIAL)
    @JvmField val json = JsonWriter(buf)
}

/**
 * Response buffers, shared by all loops and the threads that answer (taken on the loop, returned by whoever
 * finished the write). A locked array stack: two short critical sections per request, no node allocation, no ABA.
 */
internal class OutPool(cap: Int) {
    private val lock = Monitor()
    private val items = arrayOfNulls<Out>(cap)
    private var size = 0

    fun get(): Out {
        lock.lock()
        var o: Out? = null
        if (size > 0) {
            o = items[--size]
            items[size] = null
        }
        lock.unlock()
        val r = o ?: Out()
        r.buf.len = RESERVE
        return r
    }

    fun put(o: Out) {
        if (o.buf.buf.size > MAX_POOLED) return
        lock.lock()
        if (size < items.size) items[size++] = o
        lock.unlock()
    }
}

/**
 * The [Exchange] of one request, and the completion of its handler coroutine (no extra object per request).
 * Head fields are filled by [parseHead] on the loop; [res] is attached right before the handler starts and
 * detached by the one answer that wins [answered].
 */
internal class Ex(@JvmField val engine: EngineCore, @JvmField val cs: ConnState) : Exchange(), Continuation<Unit> {
    override var method: Method = Method.OTHER
    override var path: String = ""
    override var query: String? = null
    override var authorization: String? = null
    override var body: ByteArray = EMPTY

    /** [KEEP], [CLOSE] or [KEEP_10]. */
    @JvmField var connMode = KEEP
    // Body framing, parse time only.
    @JvmField var bodyLen = 0
    @JvmField var chunked = false
    @JvmField var expect = false
    /** The request sent Accept-Encoding: gzip. */
    @JvmField var acceptGzip = false
    /** nanoTime when the handler started (response time stats). */
    @JvmField var t0 = 0L

    /** The response buffer, taken on first use by the handler (see [EngineCore.takeOut]); null until then. */
    @JvmField var res: Out? = null
    private var jsonReady = false
    private val answered = AtomicInt(0)

    /** The client closed the connection: nobody will read the answer (queued reads for it are dropped unrun). */
    override val abandoned: Boolean get() = cs.st.load() == CLOSED

    private fun buf(): Out {
        if (answered.load() != 0) error("exchange already answered")
        return res ?: engine.takeOut().also { res = it }
    }

    override val out: ByteBuf
        get() = buf().buf

    override val json: JsonWriter
        get() {
            val o = buf()
            if (!jsonReady) { // a pooled writer may hold nesting state from an abandoned body (sendError)
                jsonReady = true
                val len = o.buf.len
                o.json.reset()
                o.buf.len = len // whatever reset() does to out, the body starts after the reserved header area
            }
            return o.json
        }

    override fun send(status: Int) {
        val o = claim() ?: return
        respond(o, status, if (hasBody(status)) o.buf.len - RESERVE else -1)
    }

    override fun sendEmpty(status: Int) {
        val o = claim() ?: return
        o.buf.len = RESERVE
        respond(o, status, if (hasBody(status)) 0 else -1)
    }

    override fun sendError(status: Int, message: String) {
        val o = claim() ?: return
        o.buf.len = RESERVE
        putError(o.buf, message)
        respond(o, status, o.buf.len - RESERVE)
    }

    /** First answer wins; later ones (and answers after the engine's 500) are ignored. */
    private fun claim(): Out? {
        if (!answered.compareAndSet(0, 1)) return null
        val o = res ?: engine.takeOut()
        res = null
        return o
    }

    private fun respond(o: Out, status: Int, bodyLen0: Int) {
        val st = Stats.local()
        var bodyLen = bodyLen0
        var gz = false
        if (acceptGzip && bodyLen >= HttpConfig.gzipMinBytes && HttpConfig.gzipLevel > 0) {
            val n = gzipBody(o.buf, bodyLen, st.c)
            if (n >= 0) {
                bodyLen = n
                gz = true
            }
        }
        val start = putHead(o.buf.buf, status, bodyLen, connMode, gz)
        st.c[if (status < 400) C_S2XX else if (status < 500) C_S4XX else C_S5XX]++
        if (bodyLen > 0) st.c[C_BODY_OUT] += bodyLen.toLong()
        if (t0 != 0L) st.lat[endpoint * BUCKETS + bucketOf(nanoTime() - t0)]++
        engine.write(cs, o, start, if (bodyLen < 0) RESERVE else o.buf.len, connMode == CLOSE)
    }

    override val context: CoroutineContext get() = EmptyCoroutineContext

    /** The handler returned or threw: answer 500 unless it answered. */
    override fun resumeWith(result: Result<Unit>) {
        val e = result.exceptionOrNull()
        if (e != null) logHandlerError(e)
        if (answered.load() == 0) sendError(500, "internal server error")
    }
}

/**
 * Compresses out[RESERVE, RESERVE+len) in place (via this thread's scratch buffer); returns the new body length, or
 * -1 to send it plain (compression failed or did not shrink it).
 */
private fun gzipBody(out: ByteBuf, len: Int, c: LongArray): Int {
    val s = threadScratch()
    val g = s.gzip ?: Gzip(HttpConfig.gzipLevel).also { s.gzip = it }
    var tmp = s.gzBuf
    val worst = len + (len ushr 3) + 64 // what the small-response encoder needs for incompressible input
    if (tmp == null || tmp.size < worst) {
        tmp = ByteArray(maxOf(worst, 74 * 1024)) // 74 KB covers a 64 KB body
        s.gzBuf = tmp
    }
    val t = nanoTime()
    val n = g.compress(out.buf, RESERVE, len, tmp, 0)
    c[C_GZ_NS] += nanoTime() - t
    if (n < 0 || n >= len) return -1
    c[C_GZ_N]++
    c[C_GZ_IN] += len.toLong()
    c[C_GZ_OUT] += n.toLong()
    tmp.copyInto(out.buf, RESERVE, 0, n)
    out.len = RESERVE + n
    return n
}

private fun hasBody(status: Int) = status >= 200 && status != 204 && status != 304

private val errorCount = AtomicInt(0)

private fun logHandlerError(e: Throwable) {
    // full traces for the first few (debugging), one line after that (no log flood under load)
    if (errorCount.addAndFetch(1) <= 10) println("http: handler failed: ${e.stackTraceToString()}")
    else println("http: handler failed: $e")
}

private val ERR_PREFIX = "{\"error\":\"".encodeToByteArray()
private val ERR_SUFFIX = "\"}".encodeToByteArray()
private val HEX = "0123456789abcdef".encodeToByteArray()

/** {"error":"<message>"} (SPEC.md "Errors") with the message JSON-escaped (UTF-8 passes through). */
internal fun putError(out: ByteBuf, message: String) {
    out.put(ERR_PREFIX)
    val m = message.encodeToByteArray()
    out.ensure(m.size * 6)
    val b = out.buf
    var p = out.len
    for (c in m) {
        val x = c.toInt() and 0xFF
        when {
            x == '"'.code || x == '\\'.code -> { b[p++] = '\\'.code.toByte(); b[p++] = c }
            x == '\n'.code -> { b[p++] = '\\'.code.toByte(); b[p++] = 'n'.code.toByte() }
            x == '\r'.code -> { b[p++] = '\\'.code.toByte(); b[p++] = 'r'.code.toByte() }
            x == '\t'.code -> { b[p++] = '\\'.code.toByte(); b[p++] = 't'.code.toByte() }
            x < 0x20 -> {
                b[p++] = '\\'.code.toByte(); b[p++] = 'u'.code.toByte(); b[p++] = '0'.code.toByte()
                b[p++] = '0'.code.toByte(); b[p++] = HEX[x shr 4]; b[p++] = HEX[x and 15]
            }
            else -> b[p++] = c
        }
    }
    out.len = p
    out.put(ERR_SUFFIX)
}

private fun reason(status: Int): String = when (status) {
    100 -> "Continue"
    200 -> "OK"
    201 -> "Created"
    204 -> "No Content"
    304 -> "Not Modified"
    400 -> "Bad Request"
    401 -> "Unauthorized"
    403 -> "Forbidden"
    404 -> "Not Found"
    405 -> "Method Not Allowed"
    409 -> "Conflict"
    411 -> "Length Required"
    413 -> "Content Too Large"
    417 -> "Expectation Failed"
    422 -> "Unprocessable Entity"
    431 -> "Request Header Fields Too Large"
    500 -> "Internal Server Error"
    501 -> "Not Implemented"
    503 -> "Service Unavailable"
    else -> "Status"
}

/** Encoded "HTTP/1.1 <code> <reason>\r\n" per status, built on first use (benign race: same bytes). */
private val statusLines = arrayOfNulls<ByteArray>(600)

private fun statusLine(status: Int): ByteArray {
    val s = if (status in 100..599) status else 500
    return statusLines[s] ?: "HTTP/1.1 $s ${reason(s)}\r\n".encodeToByteArray().also { statusLines[s] = it }
}

private val CONTENT = "Content-Type: application/json\r\nContent-Length: ".encodeToByteArray()
private val CONN_CLOSE = "Connection: close\r\n".encodeToByteArray()
private val CONN_KEEP = "Connection: keep-alive\r\n".encodeToByteArray()
private val CONTENT_GZIP = "Content-Encoding: gzip\r\n".encodeToByteArray()

/**
 * Writes the status line and headers into dst so that they end exactly at [RESERVE] (where the body starts) and
 * returns their start. [bodyLen] < 0: no Content-Type/Content-Length (204, 304).
 */
internal fun putHead(dst: ByteArray, status: Int, bodyLen: Int, connMode: Int, gzip: Boolean = false): Int {
    var p = RESERVE - 2
    dst[p] = 13
    dst[p + 1] = 10
    val conn = when (connMode) {
        CLOSE -> CONN_CLOSE
        KEEP_10 -> CONN_KEEP
        else -> null
    }
    if (conn != null) {
        p -= conn.size
        conn.copyInto(dst, p)
    }
    if (bodyLen >= 0) {
        dst[--p] = 10
        dst[--p] = 13
        var v = bodyLen
        do {
            dst[--p] = ('0'.code + v % 10).toByte()
            v /= 10
        } while (v > 0)
        p -= CONTENT.size
        CONTENT.copyInto(dst, p)
        if (gzip) {
            p -= CONTENT_GZIP.size
            CONTENT_GZIP.copyInto(dst, p)
        }
    }
    val line = statusLine(status)
    p -= line.size
    line.copyInto(dst, p)
    return p
}

internal val CONTINUE_100 = "HTTP/1.1 100 Continue\r\n\r\n".encodeToByteArray()
