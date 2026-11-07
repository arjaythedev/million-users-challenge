package feed.platform

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/** Runs on both targets (the JVM round trip with GZIPInputStream is in jvmTest). */
class GzipSmokeTest {
    @Test
    fun headerAndSize() {
        val src = ("{\"posts\":[" + (1..20).joinToString(",") { "{\"id\":$it,\"body\":\"keepalive latency pool\"}" } + "]}")
            .encodeToByteArray()
        val g = Gzip(1)
        val dst = ByteArray(16 * 1024)
        repeat(2) {
            val n = g.compress(src, 0, src.size, dst, 0)
            assertTrue(n in 18 until src.size / 2, "compressed size $n of ${src.size}")
            assertEquals(0x1f, dst[0].toInt() and 0xff)
            assertEquals(0x8b, dst[1].toInt() and 0xff)
            // ISIZE trailer: the uncompressed length, little-endian
            val isize = (dst[n - 4].toInt() and 0xff) or ((dst[n - 3].toInt() and 0xff) shl 8) or
                ((dst[n - 2].toInt() and 0xff) shl 16) or ((dst[n - 1].toInt() and 0xff) shl 24)
            assertEquals(src.size, isize)
        }
        assertEquals(-1, g.compress(src, 0, src.size, ByteArray(10), 0))
    }
}
