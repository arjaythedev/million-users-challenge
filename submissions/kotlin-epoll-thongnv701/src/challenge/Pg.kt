package challenge

import java.io.IOException
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.net.InetAddress
import java.net.URI
import java.net.UnknownHostException
import java.security.GeneralSecurityException
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.HexFormat
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

const val OP_FEED: Byte = 0
const val OP_POST: Byte = 1
const val OP_CREATE: Byte = 2
const val OP_LIKE: Byte = 3
const val OP_PING: Byte = 4

// The SPEC's statements, prepared once per connection as "0".."4" (the op).
private const val POST_SQL = "SELECT p.id, p.body, p.created_at, u.username," +
    " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count" +
    " FROM posts p JOIN users u ON u.id = p.user_id"
private val SQL = arrayOf(
    "$POST_SQL ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
    "$POST_SQL WHERE p.id = $1",
    "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
    "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
    "SELECT 1",
)
private const val INT8 = 20
private const val TEXT = 25
private val PARAM_TYPES = arrayOf(intArrayOf(), intArrayOf(INT8), intArrayOf(INT8, TEXT), intArrayOf(INT8, INT8), intArrayOf())
private const val CONNECT_TIMEOUT_MILLIS = 3000

// Post rows: id and like_count as text (fewer bytes, copied straight into the JSON), the rest binary.
private val POST_RESULT_FORMATS = byteArrayOf(0, 5, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0)

class PgConfig(@JvmField val host: String, @JvmField val port: Int, @JvmField val user: String,
               @JvmField val password: String, @JvmField val database: String) {
    companion object {
        fun parse(url: String): PgConfig {
            val u = URI.create(url)
            if (u.scheme != "postgres" && u.scheme != "postgresql") {
                throw IllegalArgumentException("DATABASE_URL must be postgres://user:password@host:port/database")
            }
            var user = System.getProperty("user.name")
            var password = ""
            val info = u.rawUserInfo
            if (info != null) {
                val c = info.indexOf(':')
                user = decode(if (c < 0) info else info.substring(0, c))
                if (c >= 0) password = decode(info.substring(c + 1))
            }
            var host = u.host ?: "localhost"
            if (host.startsWith("[")) host = host.substring(1, host.length - 1)
            val path = u.rawPath
            val db = if (path == null || path.length <= 1) user else decode(path.substring(1))
            return PgConfig(host, if (u.port < 0) 5432 else u.port, user, password, db)
        }

        private fun decode(s: String): String {
            val out = ByteArray(s.length)
            var n = 0
            var k = 0
            while (k < s.length) {
                val c = s[k]
                if (c == '%' && k + 2 < s.length) {
                    out[n++] = Integer.parseInt(s, k + 1, k + 3, 16).toByte()
                    k += 2
                } else {
                    out[n++] = c.code.toByte()
                }
                k++
            }
            return String(out, 0, n, Charsets.UTF_8)
        }
    }
}

interface PgHandler {
    fun row(pg: Pg, c: Conn, b: ByteArray, at: Int)

    fun command(c: Conn, b: ByteArray, at: Int, len: Int)

    fun read(pg: Pg, c: Conn)

    fun done(pg: Pg, unit: Array<Conn?>, n: Int, code: String?, message: String?)
}

/**
 * One Postgres connection on the v3 wire protocol. Startup, login and prepare are blocking; afterwards the socket
 * is non-blocking and driven by the loop. Requests wait in a ring in the order their replies will come.
 */
class Pg(@JvmField val tag: Int, @JvmField val reader: Boolean, private val cfg: PgConfig, private val sys: Sys) {
    @JvmField var sentMillis = 0
    @JvmField var fd = -1
    @JvmField val inb = Buf(1 shl 16)
    @JvmField val out = Buf(1 shl 16)
    private var outFrom = 0
    @JvmField var dirty = false
    @JvmField var wantWrite = false
    @JvmField val body = Buf(1 shl 16)
    @JvmField var lastError: String = "not connected"

    private var ring = arrayOfNulls<Conn>(64)
    private var head = 0
    private var size = 0
    private var cur = 0
    private var open = 0
    private var unit = arrayOfNulls<Conn>(64)
    private var code: String? = null
    private var message: String? = null

    fun inFlight(): Int = size

    fun packSize(): Int = open

    fun alive(): Boolean = fd >= 0

    /** One request in its own implicit transaction. */
    fun query(c: Conn) {
        bind(c)
        sync()
        c.last = true
        push(c)
    }

    /** One read joining the open pack; closePack puts the pack's single Sync after it. */
    fun pack(c: Conn) {
        bind(c)
        c.last = false
        push(c)
        open++
    }

    fun closePack() {
        if (open == 0) return
        ring[(head + size - 1) and (ring.size - 1)]!!.last = true
        sync()
        open = 0
    }

    /** Writes under one Sync: one implicit transaction, one commit. */
    fun batch(cs: Array<Conn?>, n: Int) {
        for (i in 0 until n) {
            val c = cs[i]!!
            bind(c)
            c.last = i == n - 1
            push(c)
        }
        sync()
    }

    private fun bind(c: Conn) {
        val o = out
        o.put('B'.code)
        val lenAt = o.len
        o.i32(0)
        o.put(0)
        o.put('0'.code + c.op)
        o.put(0)
        o.i16(1)
        o.i16(1)
        when (c.op) {
            OP_POST -> { o.i16(1); o.i32(8); o.i64(c.id) }
            OP_CREATE -> { val t = c.text!!; o.i16(2); o.i32(8); o.i64(c.userId); o.i32(t.size); o.put(t) }
            OP_LIKE -> { o.i16(2); o.i32(8); o.i64(c.userId); o.i32(8); o.i64(c.id) }
            else -> o.i16(0)
        }
        if (c.op == OP_FEED || c.op == OP_POST) {
            o.put(POST_RESULT_FORMATS)
        } else {
            o.i16(1)
            o.i16(1)
        }
        o.setI32(lenAt, o.len - lenAt)
        o.put('E'.code)
        o.i32(9)
        o.put(0)
        o.i32(0)
    }

    private fun sync() {
        out.put('S'.code)
        out.i32(4)
    }

    private fun push(c: Conn) {
        if (size == ring.size) {
            val r = arrayOfNulls<Conn>(size * 2)
            for (i in 0 until size) r[i] = ring[(head + i) and (ring.size - 1)]
            ring = r
            head = 0
        }
        ring[(head + size++) and (ring.size - 1)] = c
    }

    fun read(io: MemorySegment): Boolean {
        val r = sys.read(fd, io, io.byteSize())
        if (r == 0L) {
            lastError = "connection closed by the database"
            return false
        }
        if (r < 0) {
            val e = sys.errno()
            if (e == EAGAIN || e == EINTR) return true
            lastError = "read: errno $e"
            return false
        }
        val n = r.toInt()
        inb.ensure(n)
        MemorySegment.copy(io, JAVA_BYTE, 0L, inb.a, inb.len, n)
        inb.len += n
        return true
    }

    fun flush(io: MemorySegment): Boolean {
        while (outFrom < out.len) {
            val n = minOf((out.len - outFrom).toLong(), io.byteSize()).toInt()
            MemorySegment.copy(out.a, outFrom, io, JAVA_BYTE, 0L, n)
            val w = sys.write(fd, io, n.toLong())
            if (w < 0) {
                val e = sys.errno()
                if (e == EINTR) continue
                if (e == EAGAIN) break
                lastError = "write: errno $e"
                return false
            }
            outFrom += w.toInt()
            if (w < n) break
        }
        if (outFrom == out.len) {
            out.len = 0
            outFrom = 0
        }
        return true
    }

    fun flushed(): Boolean = out.len == 0

    fun parse(h: PgHandler) {
        val b = inb.a
        var p = 0
        val end = inb.len
        while (end - p >= 5) {
            val n = i32(b, p + 1)
            if (end - p < 1 + n) break
            val type = b[p].toInt()
            val at = p + 5
            val len = n - 4
            p += 1 + n
            when (type) {
                'D'.code -> if (cur < size) h.row(this, ring[(head + cur) and (ring.size - 1)]!!, b, at)
                'C'.code -> {
                    if (reader) {
                        if (size > 0) h.read(this, pop())
                    } else if (cur < size) {
                        h.command(ring[(head + cur++) and (ring.size - 1)]!!, b, at, len - 1)
                    }
                }
                'E'.code -> {
                    val f = errorFields(b, at, at + len)
                    if (size == 0) {
                        System.err.println("postgres: ${f[1]} (${f[0]})")
                    } else if (code == null) {
                        code = f[0]
                        message = f[1]
                    }
                }
                'Z'.code -> if (size > 0 && (!reader || code != null)) done(h)
                else -> {}
            }
            if (fd < 0) return
        }
        System.arraycopy(b, p, b, 0, end - p)
        inb.len = end - p
    }

    private fun pop(): Conn {
        val c = ring[head]!!
        ring[head] = null
        head = (head + 1) and (ring.size - 1)
        size--
        return c
    }

    private fun done(h: PgHandler) {
        var n = 0
        while (size > 0) {
            val c = pop()
            if (n == unit.size) unit = unit.copyOf(n * 2)
            unit[n++] = c
            if (c.last) break
        }
        val code = this.code
        val message = this.message
        this.code = null
        this.message = null
        cur = 0
        h.done(this, unit, n, code, message)
        unit.fill(null, 0, n)
    }

    /** Closes the connection and returns the requests that were waiting on it. */
    fun close(): Array<Conn> {
        if (fd >= 0) closeFd(fd)
        fd = -1
        val lost = Array(size) { i ->
            val k = (head + i) and (ring.size - 1)
            val c = ring[k]!!
            ring[k] = null
            c
        }
        head = 0
        size = 0
        cur = 0
        open = 0
        code = null
        message = null
        inb.len = 0
        out.len = 0
        outFrom = 0
        body.len = 0
        wantWrite = false
        return lost
    }

    fun connect(io: MemorySegment) {
        var f = -1
        val addresses = try {
            InetAddress.getAllByName(cfg.host)
        } catch (e: UnknownHostException) {
            throw IOException("unknown host ${cfg.host}", e)
        }
        for (a in addresses) {
            f = sys.connect(a, cfg.port, CONNECT_TIMEOUT_MILLIS)
            if (f >= 0) break
        }
        if (f < 0) throw IOException("connect ${cfg.host}:${cfg.port}: errno ${-f}")
        fd = f
        try {
            startup(io)
            prepare(io)
            sys.setNonBlocking(fd)
        } catch (e: IOException) {
            close()
            throw e
        } catch (e: GeneralSecurityException) {
            close()
            throw IOException(e.toString(), e)
        } catch (e: RuntimeException) {
            close()
            throw IOException(e.toString(), e)
        }
    }

    private fun startup(io: MemorySegment) {
        out.i32(0)
        out.i32(196608)
        out.cstr("user")
        out.put(utf8z(cfg.user))
        out.cstr("database")
        out.put(utf8z(cfg.database))
        out.cstr("client_encoding")
        out.cstr("UTF8")
        out.put(0)
        out.setI32(0, out.len)
        sendBlocking(io)
        var scram: Scram? = null
        var p = 0
        while (true) {
            p = nextBlocking(io, p)
            val type = inb.a[msgAt - 5].toInt()
            if (type == 'E'.code) throw IOException(errorText(msgAt, msgEnd))
            if (type == 'Z'.code) break
            if (type != 'R'.code) continue
            val auth = i32(inb.a, msgAt)
            val data = String(inb.a, msgAt + 4, msgEnd - msgAt - 4, Charsets.UTF_8)
            when (auth) {
                0 -> {}
                3 -> password(io, utf8z(cfg.password))
                5 -> {
                    val md5 = MessageDigest.getInstance("MD5")
                    val hex = HexFormat.of()
                    val inner = hex.formatHex(md5.digest((cfg.password + cfg.user).toByteArray(Charsets.UTF_8)))
                    md5.update(inner.toByteArray(Charsets.US_ASCII))
                    md5.update(inb.a, msgAt + 4, 4)
                    password(io, utf8z("md5" + hex.formatHex(md5.digest())))
                }
                10 -> {
                    if (!data.split('\u0000').contains("SCRAM-SHA-256")) {
                        throw IOException("the database offers no supported SASL mechanism: $data")
                    }
                    val s = Scram(cfg.password)
                    scram = s
                    val first = s.first()
                    out.put('p'.code)
                    out.i32(4 + 14 + 4 + first.size)
                    out.cstr("SCRAM-SHA-256")
                    out.i32(first.size)
                    out.put(first)
                    sendBlocking(io)
                }
                11 -> {
                    val s = scram ?: throw IOException("SASL continue without SASL")
                    val last = s.next(data)
                    out.put('p'.code)
                    out.i32(4 + last.size)
                    out.put(last)
                    sendBlocking(io)
                }
                12 -> if (scram == null || !scram.verify(data)) throw IOException("bad SCRAM server signature")
                else -> throw IOException("unsupported authentication method $auth")
            }
        }
        inb.len = 0
    }

    private fun password(io: MemorySegment, z: ByteArray) {
        out.put('p'.code)
        out.i32(4 + z.size)
        out.put(z)
        sendBlocking(io)
    }

    private fun prepare(io: MemorySegment) {
        for (s in SQL.indices) {
            out.put('P'.code)
            val lenAt = out.len
            out.i32(0)
            out.cstr(s.toString())
            out.cstr(SQL[s])
            out.i16(PARAM_TYPES[s].size)
            for (t in PARAM_TYPES[s]) out.i32(t)
            out.setI32(lenAt, out.len - lenAt)
        }
        sync()
        sendBlocking(io)
        var error: String? = null
        var p = 0
        while (true) {
            p = nextBlocking(io, p)
            val type = inb.a[msgAt - 5].toInt()
            if (type == 'E'.code && error == null) error = errorText(msgAt, msgEnd)
            if (type == 'Z'.code) break
        }
        inb.len = 0
        if (error != null) throw IOException("prepare: $error")
    }

    private var msgAt = 0
    private var msgEnd = 0

    private fun nextBlocking(io: MemorySegment, p: Int): Int {
        while (inb.len - p < 5 || inb.len - p < 1 + i32(inb.a, p + 1)) {
            val r = sys.readBlocking(fd, io, io.byteSize())
            if (r <= 0) {
                throw IOException(
                    if (r == 0L) "connection closed by the database"
                    else if (sys.errno() == EAGAIN) "timed out" else "read: errno ${sys.errno()}"
                )
            }
            val n = r.toInt()
            inb.ensure(n)
            MemorySegment.copy(io, JAVA_BYTE, 0L, inb.a, inb.len, n)
            inb.len += n
        }
        msgAt = p + 5
        msgEnd = p + 1 + i32(inb.a, p + 1)
        return msgEnd
    }

    private fun sendBlocking(io: MemorySegment) {
        var from = 0
        while (from < out.len) {
            val n = minOf((out.len - from).toLong(), io.byteSize()).toInt()
            MemorySegment.copy(out.a, from, io, JAVA_BYTE, 0L, n)
            val w = sys.writeBlocking(fd, io, n.toLong())
            if (w <= 0) throw IOException("write: errno ${sys.errno()}")
            from += w.toInt()
        }
        out.len = 0
    }

    private fun errorText(from: Int, to: Int): String {
        val f = errorFields(inb.a, from, to)
        return "${f[1]} (${f[0]})"
    }
}

/** SQLSTATE and message of an ErrorResponse. */
private fun errorFields(b: ByteArray, from: Int, to: Int): Array<String> {
    var sqlState = ""
    var msg = ""
    var k = from
    while (k < to && b[k].toInt() != 0) {
        val field = b[k++].toInt()
        var z = k
        while (z < to && b[z].toInt() != 0) z++
        if (field == 'C'.code) sqlState = String(b, k, z - k, Charsets.UTF_8)
        else if (field == 'M'.code) msg = String(b, k, z - k, Charsets.UTF_8)
        k = z + 1
    }
    return arrayOf(sqlState, msg)
}

private fun utf8z(s: String): ByteArray {
    val b = s.toByteArray(Charsets.UTF_8)
    return b.copyOf(b.size + 1)
}

/** SCRAM-SHA-256 (RFC 7677), without channel binding. */
private class Scram(password: String) {
    private val password: ByteArray = password.toByteArray(Charsets.UTF_8)
    private val nonce: String
    private val clientFirstBare: String
    private var serverSignature: ByteArray? = null

    init {
        val r = ByteArray(18)
        SecureRandom().nextBytes(r)
        nonce = Base64.getEncoder().encodeToString(r)
        clientFirstBare = "n=,r=$nonce"
    }

    fun first(): ByteArray = "n,,$clientFirstBare".toByteArray(Charsets.US_ASCII)

    fun next(serverFirst: String): ByteArray {
        var r: String? = null
        var s: String? = null
        var i = 0
        for (attr in serverFirst.split(',')) {
            if (attr.startsWith("r=")) r = attr.substring(2)
            else if (attr.startsWith("s=")) s = attr.substring(2)
            else if (attr.startsWith("i=")) i = attr.substring(2).toInt()
        }
        if (r == null || s == null || i <= 0 || !r.startsWith(nonce)) throw IOException("bad SCRAM server-first-message")
        val salted = pbkdf2(password, Base64.getDecoder().decode(s), i)
        val clientKey = hmac(salted, "Client Key".toByteArray(Charsets.US_ASCII))
        val storedKey = MessageDigest.getInstance("SHA-256").digest(clientKey)
        val withoutProof = "c=biws,r=$r"
        val auth = "$clientFirstBare,$serverFirst,$withoutProof".toByteArray(Charsets.UTF_8)
        val proof = hmac(storedKey, auth)
        for (k in proof.indices) proof[k] = (proof[k].toInt() xor clientKey[k].toInt()).toByte()
        serverSignature = hmac(hmac(salted, "Server Key".toByteArray(Charsets.US_ASCII)), auth)
        return "$withoutProof,p=${Base64.getEncoder().encodeToString(proof)}".toByteArray(Charsets.US_ASCII)
    }

    fun verify(serverFinal: String): Boolean =
        serverFinal.startsWith("v=") &&
            MessageDigest.isEqual(Base64.getDecoder().decode(serverFinal.substring(2).trim()), serverSignature)

    private fun hmac(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    private fun pbkdf2(password: ByteArray, salt: ByteArray, iterations: Int): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(password, "HmacSHA256"))
        mac.update(salt)
        var u = mac.doFinal(byteArrayOf(0, 0, 0, 1))
        val result = u.clone()
        for (k in 1 until iterations) {
            u = mac.doFinal(u)
            for (j in result.indices) result[j] = (result[j].toInt() xor u[j].toInt()).toByte()
        }
        return result
    }
}
