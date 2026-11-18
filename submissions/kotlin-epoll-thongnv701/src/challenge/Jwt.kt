package challenge

import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

const val JWT_OK = 0
const val JWT_MISSING = 1
const val JWT_INVALID = 2
const val JWT_BAD_PAYLOAD = 3

private val BEARER: ByteArray = ascii("Bearer ")
private val B64URL: ByteArray = ascii("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")

private fun b64index(c: Byte): Int {
    val x = c.toInt()
    if (x >= 'A'.code && x <= 'Z'.code) return x - 'A'.code
    if (x >= 'a'.code && x <= 'z'.code) return x - 'a'.code + 26
    if (x >= '0'.code && x <= '9'.code) return x - '0'.code + 52
    if (x == '-'.code) return 62
    if (x == '_'.code) return 63
    return -1
}

private fun decode(b: ByteArray, from: Int, to: Int): String {
    for (k in from until to) if (b64index(b[k]) < 0) throw Malformed.INSTANCE
    val raw = Base64.getUrlDecoder().decode(b.copyOfRange(from, to))
    return utf8(raw, 0, raw.size)
}

/** 1 to 19 ASCII digits, greater than zero; otherwise -1. */
fun positiveInt(s: CharSequence): Long {
    val n = s.length
    if (n == 0 || n > 19) return -1
    var v = 0L
    for (k in 0 until n) {
        val c = s[k]
        if (c < '0' || c > '9') return -1
        v = v * 10 + (c - '0')
        if (v < 0) return -1
    }
    return if (v > 0) v else -1
}

/** HS256 tokens, checked in full on every request. One per loop: a Mac is not thread-safe. */
class Jwt(secret: String) {
    private val mac: Mac = Mac.getInstance("HmacSHA256")
    private val digest = ByteArray(32)
    private val sig = ByteArray(43)

    @JvmField var userId = 0L
    @JvmField var username: ByteArray? = null

    init {
        mac.init(SecretKeySpec(secret.toByteArray(Charsets.UTF_8), "HmacSHA256"))
    }

    fun verify(b: ByteArray, start: Int, end: Int): Int {
        if (start < 0 || end - start < BEARER.size) return JWT_MISSING
        for (k in BEARER.indices) if (b[start + k] != BEARER[k]) return JWT_MISSING
        val t = start + BEARER.size
        var d1 = -1
        var d2 = -1
        for (k in t until end) {
            if (b[k].toInt() != '.'.code) continue
            if (d1 < 0) d1 = k
            else if (d2 < 0) d2 = k
            else return JWT_INVALID
        }
        if (d2 < 0) return JWT_INVALID
        try {
            val header = Json.parse(decode(b, t, d1))
            if ("HS256" != jsonGet(header, "alg")) return JWT_INVALID
            if (!signatureMatches(b, t, d2, end)) return JWT_INVALID
            val payload = Json.parse(decode(b, d1 + 1, d2))
            if (payload !is Map<*, *>) return JWT_INVALID
            val now = System.currentTimeMillis() / 1000
            val exp = jsonGet(payload, "exp")
            if (!(exp is Double && now < exp)) return JWT_INVALID
            val nbf = jsonGet(payload, "nbf")
            if (nbf != null && nbf !== JsonNull && !(nbf is Double && now >= nbf)) return JWT_INVALID
            val sub = jsonGet(payload, "sub")
            val name = jsonGet(payload, "username")
            val id = if (sub is String) positiveInt(sub) else -1
            if (id <= 0 || name !is String) return JWT_BAD_PAYLOAD
            userId = id
            username = name.toByteArray(Charsets.UTF_8)
            return JWT_OK
        } catch (e: Malformed) {
            return JWT_INVALID
        } catch (e: IllegalArgumentException) {
            return JWT_INVALID
        }
    }

    private fun signatureMatches(b: ByteArray, from: Int, dot: Int, end: Int): Boolean {
        mac.update(b, from, dot - from)
        mac.doFinal(digest, 0)
        var o = 0
        var k = 0
        while (k < 30) {
            val v = (digest[k].toInt() and 0xff shl 16) or (digest[k + 1].toInt() and 0xff shl 8) or (digest[k + 2].toInt() and 0xff)
            sig[o++] = B64URL[v ushr 18]
            sig[o++] = B64URL[(v ushr 12) and 63]
            sig[o++] = B64URL[(v ushr 6) and 63]
            sig[o++] = B64URL[v and 63]
            k += 3
        }
        val v = (digest[30].toInt() and 0xff shl 8) or (digest[31].toInt() and 0xff)
        sig[o++] = B64URL[v ushr 10]
        sig[o++] = B64URL[(v ushr 4) and 63]
        sig[o] = B64URL[(v shl 2) and 63]
        if (end - dot - 1 != sig.size) return false
        var diff = 0
        for (i in sig.indices) diff = diff or (sig[i].toInt() xor b[dot + 1 + i].toInt())
        return diff == 0
    }
}
