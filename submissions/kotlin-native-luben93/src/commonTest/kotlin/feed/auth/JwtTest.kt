package feed.auth

import feed.http.Exchange
import feed.http.Method
import feed.json.JsonWriter
import feed.platform.HmacSha256
import feed.platform.currentTimeMillis
import feed.util.ByteBuf
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class JwtTest {
    private val secret = "million-users-challenge"

    private class Ex(override val authorization: String?) : Exchange() {
        override val method = Method.POST
        override val path = "/posts"
        override val query: String? = null
        override val body = ByteArray(0)
        override val out = ByteBuf(64)
        override val json = JsonWriter(out)
        override fun send(status: Int) {}
        override fun sendEmpty(status: Int) {}
        override fun sendError(status: Int, message: String) {}
    }

    private fun b64(b: ByteArray): String {
        val a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        val sb = StringBuilder()
        var i = 0
        while (i < b.size) {
            val n = minOf(3, b.size - i)
            var v = 0
            for (k in 0 until 3) v = (v shl 8) or (if (k < n) b[i + k].toInt() and 0xFF else 0)
            for (k in 0..n) sb.append(a[(v shr (18 - 6 * k)) and 63])
            i += 3
        }
        return sb.toString()
    }

    private fun sign(payload: String, header: String = """{"alg":"HS256","typ":"JWT"}""", key: String = secret): String {
        val hp = b64(header.encodeToByteArray()) + "." + b64(payload.encodeToByteArray())
        val mac = ByteArray(32)
        val d = hp.encodeToByteArray()
        HmacSha256(key.encodeToByteArray()).mac(d, 0, d.size, mac, 0)
        return hp + "." + b64(mac)
    }

    private val now get() = currentTimeMillis() / 1000

    private fun auth(header: String?): Pair<String?, Ex> {
        Jwt.init(secret)
        val ex = Ex(header)
        return Jwt.authenticate(ex) to ex
    }

    @Test
    fun valid() {
        val (err, ex) = auth("Bearer " + sign("""{"sub":"1","username":"golden_ember_1","iat":$now,"exp":${now + 3600}}"""))
        assertNull(err)
        assertEquals(1L, ex.viewer)
        assertEquals("golden_ember_1", ex.username)
    }

    @Test
    fun rejections() {
        assertEquals(Jwt.MISSING, auth(null).first)
        assertEquals(Jwt.MISSING, auth("Basic abc").first)
        assertEquals(Jwt.INVALID, auth("Bearer abc").first)
        val good = sign("""{"sub":"1","username":"u","exp":${now + 3600}}""")
        assertEquals(Jwt.INVALID, auth("Bearer " + good.substringBeforeLast('.') + ".c2lnbmF0dXJlLWlzLW5vdC12YWxpZC1hdC1hbGwtYXQtYWxsLTEyMzQ").first)
        assertEquals(Jwt.INVALID, auth("Bearer " + sign("""{"sub":"1","username":"u","exp":${now + 3600}}""", key = "other")).first)
        val none = b64("""{"alg":"none","typ":"JWT"}""".encodeToByteArray()) + "." + good.split('.')[1] + "."
        assertEquals(Jwt.INVALID, auth("Bearer $none").first)
        assertEquals(Jwt.INVALID, auth("Bearer " + sign("""{"sub":"1","username":"u","exp":${now - 3600}}""")).first)
        assertEquals(Jwt.INVALID, auth("Bearer " + sign("""{"sub":"1","username":"u"}""")).first)
        assertEquals(Jwt.PAYLOAD, auth("Bearer " + sign("""{"sub":"abc","username":"u","exp":${now + 3600}}""")).first)
        assertEquals(Jwt.PAYLOAD, auth("Bearer " + sign("""{"sub":"1","exp":${now + 3600}}""")).first)
        assertEquals(Jwt.PAYLOAD, auth("Bearer " + sign("""{"sub":1,"username":"u","exp":${now + 3600}}""")).first)
    }

    @Test
    fun ids() {
        for (bad in listOf("0", "-1", "abc", "1.5", "", "+1", "1 ")) assertEquals(-1L, positiveId(bad), bad)
        assertEquals(500000L, positiveId("500000"))
    }
}
