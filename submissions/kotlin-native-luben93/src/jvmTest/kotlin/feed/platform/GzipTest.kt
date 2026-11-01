package feed.platform

import java.io.ByteArrayInputStream
import java.util.zip.GZIPInputStream
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertTrue

class GzipTest {
    @Test
    fun roundTrip() {
        val src = ("{\"posts\":[" + (1..20).joinToString(",") { "{\"id\":$it,\"body\":\"keepalive latency pool cpu\"}" } + "]}")
            .encodeToByteArray()
        val g = Gzip(1)
        val dst = ByteArray(64 * 1024)
        for (round in 0 until 3) { // state is reused between calls
            val n = g.compress(src, 0, src.size, dst, 7)
            assertTrue(n in 1 until src.size, "compressed $n of ${src.size}")
            val back = GZIPInputStream(ByteArrayInputStream(dst, 7, n)).readBytes()
            assertContentEquals(src, back)
        }
        assertEquals(-1, g.compress(src, 0, src.size, ByteArray(20), 0)) // too small
    }
}
