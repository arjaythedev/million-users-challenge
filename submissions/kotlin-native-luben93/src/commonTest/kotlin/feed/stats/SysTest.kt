package feed.stats

import feed.platform.processCpuNanos
import feed.platform.readProcFile
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

class SysTest {
    @Test
    fun threadStat() {
        val t = assertNotNull(Proc.threadStat("1234 (http-3) S 1 1234 1234 0 -1 4194560 100 0 0 0 55 33 0 0 20 0 8 0 1000 1 2 3"))
        assertEquals("http-3", t.name)
        assertEquals(55L, t.utime)
        assertEquals(33L, t.stime)
        val w = assertNotNull(Proc.threadStat("77 (weird (name) x) R 1 77 77 0 -1 4194560 1 0 0 0 7 9 0 0 20 0 1 0 5"))
        assertEquals("weird (name) x", w.name)
        assertEquals(7L, w.utime)
        assertEquals(9L, w.stime)
        assertNull(Proc.threadStat("garbage"))
        assertNull(Proc.threadStat("1 (x) S 1 2 3"))
    }

    @Test
    fun roles() {
        assertEquals("http", Proc.role("http-3"))
        assertEquals("pg-read", Proc.role("pg-read-12"))
        assertEquals("pg-write", Proc.role("pg-write-0"))
        assertEquals("stats", Proc.role("stats"))
        assertEquals("gc-log", Proc.role("gc-log"))
        assertEquals("other", Proc.role("feed.kexe"))
        assertEquals("other", Proc.role("12"))
        assertEquals("other", Proc.role(""))
        assertEquals("other", Proc.role("-"))
    }

    @Test
    fun formatting() {
        assertEquals("0.0", fmt1(0.04))
        assertEquals("12.3", fmt1(12.34))
        assertEquals("1.50", fmt2(1.5))
        assertEquals("0.07", fmt2(0.07))
        assertEquals("12.00", fmt2(12.0))
    }

    @Test
    fun samplerOnThisMachine() {
        if (readProcFile("/proc/self/stat").isEmpty()) return // not Linux: nothing to sample
        val s = SysSampler()
        assertEquals("", s.blocks(1.0)) // the first call only records a baseline
        var x = 0L
        for (i in 0 until 50_000_000) x += i // a little CPU between the samples
        val out = s.blocks(1.0)
        assertTrue(x != 0L)
        assertTrue(out.contains(" thr[all="), out)
        assertTrue(!out.contains("net[") && !out.contains("cpu[box"), out) // nothing box-wide is read any more
    }

    @Test
    fun processCpuTimeAdvancesWithWork() {
        val a = processCpuNanos()
        var x = 0L
        for (i in 0 until 100_000_000) x += i xor (i shr 3)
        val b = processCpuNanos()
        assertTrue(x != 0L)
        assertTrue(b > a, "process CPU time must grow while we compute: $a -> $b")
    }
}
