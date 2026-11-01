package feed.auth

import feed.http.Exchange
import feed.json.JsonReader
import feed.json.JsonValue
import feed.platform.HmacSha256
import feed.platform.currentTimeMillis
import feed.platform.threadScratch

/**
 * HS256 bearer tokens (SPEC.md "Auth"): signature, alg and exp are checked on EVERY request (rule 9, nothing
 * remembered). The key is JWT_SECRET's UTF-8 bytes; each thread keeps its own HMAC instance.
 */
object Jwt {
    const val MISSING = "missing bearer token"
    const val INVALID = "invalid or expired token"
    const val PAYLOAD = "invalid token payload"

    private var key = ByteArray(0)

    fun init(secret: String) {
        key = secret.encodeToByteArray()
    }

    /**
     * Verifies the request's Authorization header. On success sets [Exchange.viewer] (the user id from `sub`) and
     * [Exchange.username] and returns null; otherwise returns the 401 message.
     */
    fun authenticate(ex: Exchange): String? {
        val h = ex.authorization ?: return MISSING
        if (!h.startsWith("Bearer ")) return MISSING
        val token = h.substring(7)
        val d1 = token.indexOf('.')
        val d2 = token.lastIndexOf('.')
        if (d1 <= 0 || d2 <= d1 || token.indexOf('.', d1 + 1) != d2) return INVALID
        val header = b64url(token, 0, d1) ?: return INVALID
        val payload = b64url(token, d1 + 1, d2) ?: return INVALID
        val sig = b64url(token, d2 + 1, token.length) ?: return INVALID
        val alg = (JsonReader.parse(header) as? JsonValue.Obj)?.get("alg") as? JsonValue.Str ?: return INVALID
        if (alg.value != "HS256" || sig.size != 32) return INVALID

        val s = threadScratch()
        val mac = s.jwtMac ?: HmacSha256(key).also { s.jwtMac = it }
        val signed = token.encodeToByteArray(0, d2)
        mac.mac(signed, 0, signed.size, s.mac, 0)
        var diff = 0
        for (i in 0 until 32) diff = diff or (s.mac[i].toInt() xor sig[i].toInt())
        if (diff != 0) return INVALID

        val claims = JsonReader.parse(payload) as? JsonValue.Obj ?: return INVALID
        val exp = (claims["exp"] as? JsonValue.Num)?.text?.toDoubleOrNull() ?: return INVALID
        if (currentTimeMillis() / 1000.0 >= exp) return INVALID
        val sub = (claims["sub"] as? JsonValue.Str)?.value ?: return PAYLOAD
        val id = positiveId(sub)
        if (id <= 0) return PAYLOAD
        val name = (claims["username"] as? JsonValue.Str)?.value ?: return PAYLOAD
        ex.viewer = id
        ex.username = name
        return null
    }

    /** base64url (no padding, '=' tolerated at the end) of s[from, to); null if malformed. */
    private fun b64url(s: String, from: Int, to: Int): ByteArray? {
        var end = to
        while (end > from && s[end - 1] == '=') end--
        val n = end - from
        if (n % 4 == 1) return null
        val out = ByteArray(n * 3 / 4)
        var acc = 0
        var bits = 0
        var o = 0
        for (i in from until end) {
            val v = when (val c = s[i]) {
                in 'A'..'Z' -> c - 'A'
                in 'a'..'z' -> c - 'a' + 26
                in '0'..'9' -> c - '0' + 52
                '-' -> 62
                '_' -> 63
                else -> return null
            }
            acc = (acc shl 6) or v
            bits += 6
            if (bits >= 8) {
                bits -= 8
                out[o++] = (acc shr bits).toByte()
            }
        }
        return out
    }
}

/** A positive decimal integer of digits only ("0", "-1", "abc", "1.5", "" -> -1); -1 on overflow too. */
fun positiveId(s: String): Long {
    if (s.isEmpty() || s.length > 18) return -1
    var v = 0L
    for (c in s) {
        if (c !in '0'..'9') return -1
        v = v * 10 + (c - '0')
    }
    return if (v > 0) v else -1
}
