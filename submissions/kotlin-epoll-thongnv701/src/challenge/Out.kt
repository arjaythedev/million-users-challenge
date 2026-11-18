package challenge

import java.lang.foreign.MemorySegment
import java.nio.ByteBuffer

fun ascii(s: String): ByteArray = s.toByteArray(Charsets.US_ASCII)

fun digitCount(value: Long): Int {
    var v = value
    var n = 1
    while (v >= 10) {
        v /= 10
        n++
    }
    return n
}

private fun status(s: String): ByteArray = ascii("HTTP/1.1 $s\r\nContent-Type: application/json\r\n")

@JvmField val S200: ByteArray = status("200 OK")
@JvmField val S201: ByteArray = status("201 Created")
@JvmField val S400: ByteArray = status("400 Bad Request")
@JvmField val S401: ByteArray = status("401 Unauthorized")
@JvmField val S404: ByteArray = status("404 Not Found")
@JvmField val S413: ByteArray = status("413 Content Too Large")
@JvmField val S431: ByteArray = status("431 Request Header Fields Too Large")
@JvmField val S500: ByteArray = status("500 Internal Server Error")
@JvmField val S501: ByteArray = status("501 Not Implemented")
@JvmField val S503: ByteArray = status("503 Service Unavailable")
private val CLOSE: ByteArray = ascii("Connection: close\r\n")
private val GZIP: ByteArray = ascii("Content-Encoding: gzip\r\n")
private val LENGTH: ByteArray = ascii("Content-Length: ")
private val END: ByteArray = ascii("\r\n\r\n")

/** The loop's response buffer: native memory, so a response goes to write() without a copy. */
class Out(capacity: Int) {
    @JvmField val buf: ByteBuffer = ByteBuffer.allocateDirect(capacity)
    @JvmField val seg: MemorySegment = MemorySegment.ofBuffer(buf)
    @JvmField var pos = 0
    @JvmField var close = false

    fun free(): Int = buf.capacity() - pos

    fun head(status: ByteArray, bodyLength: Int) = head(status, bodyLength, false)

    fun head(status: ByteArray, bodyLength: Int, gzip: Boolean) {
        put(status)
        if (gzip) put(GZIP)
        if (close) put(CLOSE)
        put(LENGTH)
        putLong(bodyLength.toLong())
        put(END)
    }

    fun respond(status: ByteArray, body: ByteArray) {
        head(status, body.size)
        put(body)
    }

    fun put(a: ByteArray) {
        buf.put(pos, a)
        pos += a.size
    }

    fun put(a: ByteArray, off: Int, len: Int) {
        buf.put(pos, a, off, len)
        pos += len
    }

    fun putLong(value: Long) {
        var v = value
        val n = digitCount(v)
        var p = pos + n
        do {
            buf.put(--p, ('0'.code + (v % 10).toInt()).toByte())
            v /= 10
        } while (v != 0L)
        pos += n
    }
}
