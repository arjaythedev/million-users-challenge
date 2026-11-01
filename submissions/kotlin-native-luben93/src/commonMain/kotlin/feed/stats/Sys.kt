package feed.stats

import feed.platform.listDir
import feed.platform.readProcFile
import kotlin.jvm.JvmField

/**
 * The Linux /proc files the stats line reads, parsed from text so tests can feed fixtures: kernel network counters,
 * CPU time per CPU, softirq counts per CPU and CPU time per thread. Anything missing or malformed reads as zero.
 */
internal object Proc {
    /** /proc/net/snmp and /proc/net/netstat: a header line and a value line per protocol ("Tcp: A B", "Tcp: 1 2"). */
    fun paired(text: String, prefix: String): Map<String, Long> {
        val lines = text.lineSequence().filter { it.startsWith(prefix) }.toList()
        if (lines.size < 2) return emptyMap()
        val names = lines[0].split(' ').drop(1)
        val vals = lines[1].split(' ').drop(1)
        val m = HashMap<String, Long>()
        for (i in names.indices) vals.getOrNull(i)?.toLongOrNull()?.let { m[names[i]] = it }
        return m
    }

    /** Cumulative kernel network counters, indexed by the NET_* constants in Stats.kt. */
    fun net(snmp: String, netstat: String, softnet: String, dev: String): LongArray {
        val r = LongArray(NET_COUNT)
        val tcp = paired(snmp, "Tcp:")
        r[NET_TCP_IN] = tcp["InSegs"] ?: 0
        r[NET_TCP_OUT] = tcp["OutSegs"] ?: 0
        r[NET_TCP_RETRANS] = tcp["RetransSegs"] ?: 0
        val ext = paired(netstat, "TcpExt:")
        r[NET_TCP_RTO] = ext["TCPTimeouts"] ?: 0
        r[NET_LISTEN_DROPS] = ext["ListenDrops"] ?: 0
        r[NET_BACKLOG_DROP] = ext["TCPBacklogDrop"] ?: 0
        r[NET_RCVQ_DROP] = ext["TCPRcvQDrop"] ?: 0
        for (line in softnet.lineSequence()) { // per CPU, hex: processed dropped time_squeeze ...
            val f = line.split(' ').filter { it.isNotEmpty() }
            if (f.size < 3) continue
            r[NET_SOFTNET_DROP] += f[1].toLongOrNull(16) ?: 0
            r[NET_SOFTNET_SQUEEZE] += f[2].toLongOrNull(16) ?: 0
        }
        for (line in dev.lineSequence().drop(2)) { // "  eth0: rx_bytes rx_packets rx_errs rx_drop ... tx_bytes tx_packets tx_errs tx_drop ..."
            val colon = line.indexOf(':')
            if (colon < 0 || line.substring(0, colon).trim() == "lo") continue
            val f = line.substring(colon + 1).split(' ').filter { it.isNotEmpty() }.map { it.toLongOrNull() ?: 0 }
            if (f.size < 12) continue
            r[NET_NIC_RX_BYTES] += f[0]; r[NET_NIC_RX_PKTS] += f[1]; r[NET_NIC_RX_DROP] += f[3]
            r[NET_NIC_TX_BYTES] += f[8]; r[NET_NIC_TX_PKTS] += f[9]; r[NET_NIC_TX_DROP] += f[11]
        }
        return r
    }

    /** One "cpu" line of /proc/stat, in clock ticks (USER_HZ, 100 per second). */
    class Cpu(
        @JvmField val id: Int, // N of "cpuN"; -1 for the "cpu" line that sums them all
        @JvmField val user: Long, @JvmField val nice: Long, @JvmField val system: Long, @JvmField val idle: Long,
        @JvmField val iowait: Long, @JvmField val irq: Long, @JvmField val softirq: Long, @JvmField val steal: Long,
    ) {
        val busy get() = user + nice + system + irq + softirq + steal
        val total get() = busy + idle + iowait
    }

    class Stat(@JvmField val all: Cpu, @JvmField val cpus: List<Cpu>, @JvmField val ctxt: Long, @JvmField val intr: Long)

    fun stat(text: String): Stat? {
        var all: Cpu? = null
        val cpus = ArrayList<Cpu>()
        var ctxt = 0L
        var intr = 0L
        for (line in text.lineSequence()) {
            if (line.startsWith("cpu")) {
                val f = line.split(' ').filter { it.isNotEmpty() }
                if (f.size < 9) continue // the label and the eight counters from user to steal
                val n = LongArray(8) { f[it + 1].toLongOrNull() ?: 0 }
                val id = if (f[0] == "cpu") -1 else f[0].substring(3).toIntOrNull() ?: continue
                val c = Cpu(id, n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7])
                if (id < 0) all = c else cpus.add(c)
            } else if (line.startsWith("ctxt ")) {
                ctxt = line.substring(5).trim().toLongOrNull() ?: 0
            } else if (line.startsWith("intr ")) {
                intr = line.substring(5).trim().substringBefore(' ').toLongOrNull() ?: 0
            }
        }
        return all?.let { Stat(it, cpus, ctxt, intr) }
    }

    /** The CPU numbers in "Cpus_allowed_list:" of /proc/self/status ("0-3,8,10-11"); empty if the line is missing. */
    fun cpusAllowed(status: String): IntArray {
        val line = status.lineSequence().firstOrNull { it.startsWith("Cpus_allowed_list:") } ?: return IntArray(0)
        val out = ArrayList<Int>()
        for (part in line.substringAfter(':').trim().split(',')) {
            val a = part.substringBefore('-').trim().toIntOrNull() ?: continue
            val b = if (part.contains('-')) part.substringAfter('-').trim().toIntOrNull() ?: continue else a
            if (b < a || b - a > 65_535) continue
            for (c in a..b) out.add(c)
        }
        return out.toIntArray()
    }

    /** Busy ticks of the CPUs in [allowed] (all CPUs if it is empty). */
    fun busyOf(st: Stat, allowed: IntArray): Long =
        if (allowed.isEmpty()) st.all.busy else st.cpus.filter { it.id in allowed }.sumOf { it.busy }

    /** All ticks (busy, idle, iowait) of the CPUs in [allowed] (all CPUs if it is empty). */
    fun totalOf(st: Stat, allowed: IntArray): Long =
        if (allowed.isEmpty()) st.all.total else st.cpus.filter { it.id in allowed }.sumOf { it.total }

    /** The per-CPU counts of one /proc/softirqs row ("NET_RX:  12  34 ..."), empty if there is none. */
    fun softirqRow(text: String, name: String): LongArray {
        for (line in text.lineSequence()) {
            val t = line.trim()
            if (t.startsWith("$name:")) {
                return t.substring(name.length + 1).split(' ').filter { it.isNotEmpty() }.map { it.toLongOrNull() ?: 0 }.toLongArray()
            }
        }
        return LongArray(0)
    }

    /** A thread's name and CPU time in ticks, from /proc/<pid>/task/<tid>/stat ("tid (name) S ppid ... utime stime ..."). */
    class Thr(@JvmField val name: String, @JvmField val utime: Long, @JvmField val stime: Long)

    fun threadStat(text: String): Thr? {
        val open = text.indexOf('(')
        val close = text.lastIndexOf(')') // the name may hold spaces and parentheses: the last ')' ends it
        if (open < 0 || close < open) return null
        val f = text.substring(close + 1).trim().split(' ') // f[0] is field 3 (the state), so utime (14) is f[11], stime (15) f[12]
        if (f.size < 13) return null
        val u = f[11].toLongOrNull() ?: return null
        val s = f[12].toLongOrNull() ?: return null
        return Thr(text.substring(open + 1, close), u, s)
    }

    /** The role a thread's name stands for: "http-3" -> "http", "pg-read-12" -> "pg-read"; everything unknown is "other". */
    fun role(name: String): String {
        var e = name.length
        while (e > 0 && name[e - 1] in '0'..'9') e--
        if (e in 1 until name.length && name[e - 1] == '-') e--
        return when (val r = name.substring(0, e)) {
            "http", "pg-read", "pg-write", "stats", "gc-log" -> r
            else -> "other"
        }
    }
}

private val ROLES = arrayOf("http", "pg-read", "pg-write", "stats", "gc-log", "other")

/**
 * What the stats line says about the box between two lines: kernel network counters (net[...]), CPU of the whole box
 * with its busiest CPU, softirq and steal shares (cpu[...]) and CPU per thread role (thr[...]). Used by the printer
 * thread only. The numbers a controller needs afterwards are left in the public fields.
 */
internal class SysSampler {
    private var primed = false
    private var net0 = LongArray(0)
    private var stat0: Proc.Stat? = null
    private var soft0 = LongArray(0)
    private val thr0 = HashMap<String, LongArray>() // tid -> [utime, stime]

    /** Box CPU (all non-idle ticks) per request in microseconds over the last interval; 0 = unknown. */
    @JvmField var boxUsPerReq = 0.0
    /** The busy share of all CPUs and of the busiest one over the last interval, 0..1. */
    @JvmField var boxBusy = 0.0
    @JvmField var hotBusy = 0.0
    /** Packets the kernel dropped over the last interval (listen, backlog, receive queue, softnet, NIC rx and tx). */
    @JvmField var drops = 0L
    /** TCP segments retransmitted and retransmission timeouts over the last interval. */
    @JvmField var tcpRetrans = 0L
    @JvmField var tcpRto = 0L
    /** The busiest CPU over the last interval (-1 = unknown) with its softirq share, and the CPU taking most NET_RX softirqs. */
    @JvmField var hotCpu = -1
    @JvmField var hotSoft = 0.0
    @JvmField var rxCpu = -1
    @JvmField var rxShare = 0.0

    private fun pct(x: Double) = (x * 100).toLong().coerceIn(0, 100).toString() + "%"
    private fun k(v: Double) = fmt1(v / 1000.0) + "k"

    /** The three blocks (each with a leading space), or "" before the first interval has passed. */
    fun blocks(secs: Double, requests: Long): String {
        val net = Proc.net(
            readProcFile("/proc/net/snmp"), readProcFile("/proc/net/netstat"),
            readProcFile("/proc/net/softnet_stat"), readProcFile("/proc/net/dev"),
        )
        val stat = Proc.stat(readProcFile("/proc/stat"))
        val soft = Proc.softirqRow(readProcFile("/proc/softirqs"), "NET_RX")
        val threads = readThreads()
        val p = stat0
        val sb = StringBuilder(480)
        if (primed && net.isNotEmpty() && net0.size == net.size) {
            val dn = LongArray(net.size) { net[it] - net0[it] }
            drops = dn[NET_LISTEN_DROPS] + dn[NET_BACKLOG_DROP] + dn[NET_RCVQ_DROP] + dn[NET_SOFTNET_DROP] + dn[NET_NIC_RX_DROP] + dn[NET_NIC_TX_DROP]
            tcpRetrans = dn[NET_TCP_RETRANS]
            tcpRto = dn[NET_TCP_RTO]
            // what the kernel did on this box meanwhile: segments per second, retransmits and timeouts, every kind of
            // drop, and the NIC's own packet and byte rates (loopback excluded)
            sb.append(" net[tcp=").append(k(dn[NET_TCP_IN] / secs)).append('/').append(k(dn[NET_TCP_OUT] / secs))
                .append(" retrans=").append(dn[NET_TCP_RETRANS]).append(" rto=").append(dn[NET_TCP_RTO])
                .append(" drop(listen=").append(dn[NET_LISTEN_DROPS]).append(" backlog=").append(dn[NET_BACKLOG_DROP])
                .append(" rcvq=").append(dn[NET_RCVQ_DROP]).append(" softnet=").append(dn[NET_SOFTNET_DROP])
                .append(" nic_rx=").append(dn[NET_NIC_RX_DROP]).append(" nic_tx=").append(dn[NET_NIC_TX_DROP])
                .append(") squeeze=").append(dn[NET_SOFTNET_SQUEEZE])
                .append(" nic=").append(k(dn[NET_NIC_RX_PKTS] / secs)).append('/').append(k(dn[NET_NIC_TX_PKTS] / secs))
                .append(' ').append(fmt1(dn[NET_NIC_RX_BYTES] / secs / 1_048_576)).append('/')
                .append(fmt1(dn[NET_NIC_TX_BYTES] / secs / 1_048_576)).append("MB/s]")
        }
        if (primed && stat != null && p != null) cpuBlock(sb, secs, requests, stat, p, soft)
        if (primed) threadBlock(sb, secs, threads)
        net0 = net
        stat0 = stat
        soft0 = soft
        thr0.clear()
        for ((tid, t) in threads) thr0[tid] = longArrayOf(t.utime, t.stime)
        primed = true
        return sb.toString()
    }

    private fun readThreads(): Map<String, Proc.Thr> {
        val out = HashMap<String, Proc.Thr>()
        for (tid in listDir("/proc/self/task")) {
            Proc.threadStat(readProcFile("/proc/self/task/$tid/stat"))?.let { out[tid] = it }
        }
        return out
    }

    private fun cpuBlock(sb: StringBuilder, secs: Double, requests: Long, st: Proc.Stat, p: Proc.Stat, soft: LongArray) {
        val dTotal = (st.all.total - p.all.total).coerceAtLeast(1).toDouble()
        val dBusy = (st.all.busy - p.all.busy).coerceAtLeast(0)
        boxBusy = dBusy / dTotal
        boxUsPerReq = if (requests > 0) dBusy * 10_000.0 / requests else 0.0 // a tick is 10 ms
        var hot = -1
        var hotFrac = -1.0
        var hotSoft = 0.0
        if (st.cpus.size == p.cpus.size) {
            for (i in st.cpus.indices) {
                val t = st.cpus[i].total - p.cpus[i].total
                if (t <= 0) continue
                val f = (st.cpus[i].busy - p.cpus[i].busy).toDouble() / t
                if (f > hotFrac) {
                    hotFrac = f
                    hot = st.cpus[i].id
                    hotSoft = (st.cpus[i].softirq - p.cpus[i].softirq).toDouble() / t
                }
            }
        }
        hotBusy = if (hot >= 0) hotFrac else 0.0
        hotCpu = hot
        this.hotSoft = if (hot >= 0) hotSoft else 0.0
        var rxCpu = -1
        var rxShare = 0.0
        if (soft.isNotEmpty() && soft.size == soft0.size) {
            var sum = 0L
            var best = 0L
            for (i in soft.indices) {
                val d = soft[i] - soft0[i]
                sum += d
                if (d > best) { best = d; rxCpu = i }
            }
            rxShare = if (sum > 0) best.toDouble() / sum else 0.0
            if (sum <= 0) rxCpu = -1
        }
        this.rxCpu = rxCpu
        this.rxShare = rxShare
        sb.append(" cpu[box=").append(pct(boxBusy))
            .append(" soft=").append(pct((st.all.softirq - p.all.softirq) / dTotal))
            .append(" steal=").append(pct((st.all.steal - p.all.steal) / dTotal))
            .append(" iowait=").append(pct((st.all.iowait - p.all.iowait) / dTotal))
        if (hot >= 0) sb.append(" hot=cpu").append(hot).append(':').append(pct(hotFrac)).append("(soft ").append(pct(hotSoft)).append(')')
        if (rxCpu >= 0) sb.append(" netrx=cpu").append(rxCpu).append(':').append(pct(rxShare))
        sb.append(" ctxt=").append(k((st.ctxt - p.ctxt) / secs)).append("/s intr=").append(k((st.intr - p.intr) / secs))
            .append("/s box_us/req=").append(if (requests > 0) fmt1(boxUsPerReq) else "-").append(']')
    }

    private fun threadBlock(sb: StringBuilder, secs: Double, threads: Map<String, Proc.Thr>) {
        if (threads.isEmpty()) return
        val user = DoubleArray(ROLES.size)
        val sys = DoubleArray(ROLES.size)
        for ((tid, t) in threads) {
            val prev = thr0[tid]
            val r = ROLES.indexOf(Proc.role(t.name))
            user[r] += (t.utime - (prev?.get(0) ?: 0)).coerceAtLeast(0).toDouble()
            sys[r] += (t.stime - (prev?.get(1) ?: 0)).coerceAtLeast(0).toDouble()
        }
        val ticks = 100.0 * secs // clock ticks per core over the interval
        sb.append(" thr[all=").append(fmt2((user.sum() + sys.sum()) / ticks))
        for (i in ROLES.indices) {
            val all = user[i] + sys[i]
            if (all <= 0.0) continue
            sb.append(' ').append(ROLES[i]).append('=').append(fmt2(all / ticks)).append("(s").append((sys[i] * 100 / all).toLong()).append("%)")
        }
        sb.append(']')
    }
}

internal fun fmt1(v: Double): String {
    val x = kotlin.math.round(v * 10).toLong()
    return "${x / 10}.${kotlin.math.abs(x % 10)}"
}

internal fun fmt2(v: Double): String {
    val x = kotlin.math.round(v * 100).toLong()
    val frac = kotlin.math.abs(x % 100)
    return "${x / 100}.${if (frac < 10) "0" else ""}$frac"
}
