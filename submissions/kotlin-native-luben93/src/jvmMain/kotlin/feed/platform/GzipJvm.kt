package feed.platform

import java.io.File
import java.util.zip.CRC32
import java.util.zip.Deflater

/** Raw deflate (java.util.zip, zlib inside the JDK) plus the gzip header and CRC32/ISIZE trailer written by hand. */
actual class Gzip actual constructor(level: Int) {
    private val deflater = Deflater(level, true)
    private val crc = CRC32()

    actual fun compress(src: ByteArray, off: Int, len: Int, dst: ByteArray, dstOff: Int): Int {
        if (len == 0 || dst.size - dstOff < 18) return -1
        var p = dstOff
        for (b in HEADER) dst[p++] = b
        deflater.reset()
        deflater.setInput(src, off, len)
        deflater.finish()
        val limit = dst.size - 8
        while (!deflater.finished()) {
            if (p >= limit) return -1
            p += deflater.deflate(dst, p, limit - p)
        }
        crc.reset()
        crc.update(src, off, len)
        val c = crc.value
        for (i in 0 until 4) dst[p++] = (c ushr (8 * i)).toByte()
        for (i in 0 until 4) dst[p++] = (len ushr (8 * i)).toByte()
        return p - dstOff
    }

    private companion object {
        val HEADER = byteArrayOf(0x1f, 0x8b.toByte(), 8, 0, 0, 0, 0, 0, 0, 0xff.toByte())
    }
}

actual fun rssBytes(): Long = try {
    File("/proc/self/statm").readText().split(' ')[1].toLong() * 4096
} catch (_: Exception) {
    0
}
