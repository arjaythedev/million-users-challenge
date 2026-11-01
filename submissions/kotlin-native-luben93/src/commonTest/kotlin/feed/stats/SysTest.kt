package feed.stats

import feed.platform.readProcFile
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

class SysTest {
    private val stat = """
        cpu  100 0 50 800 10 5 20 3 0 0
        cpu0 60 0 20 400 5 2 15 1 0 0
        cpu1 40 0 30 400 5 3 5 2 0 0
        intr 123456 1 2 3
        ctxt 987654
        btime 1
    """.trimIndent()

    @Test
    fun procStat() {
        val s = assertNotNull(Proc.stat(stat))
        assertEquals(178L, s.all.busy) // user + nice + system + irq + softirq + steal
        assertEquals(988L, s.all.total) // + idle + iowait
        assertEquals(2, s.cpus.size)
        assertEquals(98L, s.cpus[0].busy) // 60 + 0 + 20 + 2 + 15 + 1
        assertEquals(987654L, s.ctxt)
        assertEquals(123456L, s.intr)
        assertNull(Proc.stat("nothing useful\n"))
        assertNull(Proc.stat("cpu  1 2 3\n")) // too few fields
    }

    @Test
    fun cpusAllowed() {
        assertEquals(listOf(0, 1, 2, 3, 8, 10, 11), Proc.cpusAllowed("Name:\tfeed\nCpus_allowed:\tf0f\nCpus_allowed_list:\t0-3,8,10-11\nMems_allowed:\t1\n").toList())
        assertEquals(listOf(5), Proc.cpusAllowed("Cpus_allowed_list:\t5\n").toList())
        assertEquals(0, Proc.cpusAllowed("Name:\tfeed\n").size)
        assertEquals(listOf(1), Proc.cpusAllowed("Cpus_allowed_list:\t1,x,7-3\n").toList()) // junk and backwards ranges are skipped
        val s = assertNotNull(Proc.stat(stat))
        assertEquals(98L + 80L, Proc.busyOf(s, intArrayOf(0, 1)))
        assertEquals(98L, Proc.busyOf(s, intArrayOf(0)))
        assertEquals(178L, Proc.busyOf(s, IntArray(0)))
    }

    @Test
    fun softirqs() {
        val t = "                    CPU0       CPU1\n          HI:          0          0\n      NET_TX:         10         20\n      NET_RX:       1000       3000\n"
        assertEquals(listOf(1000L, 3000L), Proc.softirqRow(t, "NET_RX").toList())
        assertEquals(listOf(10L, 20L), Proc.softirqRow(t, "NET_TX").toList())
        assertEquals(0, Proc.softirqRow(t, "BLOCK").size)
        assertEquals(0, Proc.softirqRow("", "NET_RX").size)
    }

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
    fun netCounters() {
        val snmp = "Ip: Forwarding DefaultTTL\nIp: 1 64\n" +
            "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs\n" +
            "Tcp: 1 200 120000 -1 10 20 0 0 5 1000 2000 7 0\n"
        val netstat = "TcpExt: SyncookiesSent ListenOverflows ListenDrops TCPTimeouts TCPBacklogDrop TCPRcvQDrop\nTcpExt: 0 3 4 5 6 7\nIpExt: InNoRoutes\nIpExt: 0\n"
        val softnet = "0000a1b2 00000001 00000002 0 0 0 0 0 0 0 0 0 0\n0000c3d4 0000000a 00000003 0 0 0 0 0 0 0 0 0 0\n"
        val dev = "Inter-|   Receive |  Transmit\n face |bytes packets errs drop fifo frame compressed multicast|bytes packets errs drop fifo colls carrier compressed\n" +
            "    lo: 5000 50 0 0 0 0 0 0 5000 50 0 0 0 0 0 0\n  eth0: 1000 10 0 2 0 0 0 0 3000 30 0 4 0 0 0 0\n"
        val n = Proc.net(snmp, netstat, softnet, dev)
        assertEquals(1000L, n[NET_TCP_IN])
        assertEquals(2000L, n[NET_TCP_OUT])
        assertEquals(7L, n[NET_TCP_RETRANS])
        assertEquals(5L, n[NET_TCP_RTO])
        assertEquals(4L, n[NET_LISTEN_DROPS])
        assertEquals(6L, n[NET_BACKLOG_DROP])
        assertEquals(7L, n[NET_RCVQ_DROP])
        assertEquals(11L, n[NET_SOFTNET_DROP]) // 1 + 0xa
        assertEquals(5L, n[NET_SOFTNET_SQUEEZE]) // 2 + 3
        assertEquals(1000L, n[NET_NIC_RX_BYTES])
        assertEquals(10L, n[NET_NIC_RX_PKTS])
        assertEquals(2L, n[NET_NIC_RX_DROP])
        assertEquals(3000L, n[NET_NIC_TX_BYTES])
        assertEquals(30L, n[NET_NIC_TX_PKTS])
        assertEquals(4L, n[NET_NIC_TX_DROP])
        assertTrue(Proc.net("", "", "", "").all { it == 0L })
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
        if (readProcFile("/proc/stat").isEmpty()) return // not Linux: nothing to sample
        val s = SysSampler()
        assertEquals("", s.blocks(1.0, 100)) // the first call only records a baseline
        var x = 0L
        for (i in 0 until 20_000_000) x += i // a little CPU between the samples
        val out = s.blocks(1.0, 100)
        assertTrue(x != 0L)
        assertTrue(out.contains(" net[tcp="), out)
        assertTrue(out.contains(" cpu[box="), out)
        assertTrue(out.contains(" thr[all="), out)
    }
}
