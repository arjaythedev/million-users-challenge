package feed.stats

import feed.platform.listDir
import feed.platform.readProcFile
import kotlin.jvm.JvmField

/**
 * The per-thread CPU the stats line shows, parsed from /proc/self/task/<tid>/stat text so tests can feed fixtures. Only this
 * process's own files are read (nothing box-wide: no /proc/stat, /proc/net or /proc/softirqs). Anything missing or
 * malformed reads as zero.
 */
internal object Proc {
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
 * What the stats line says about the process between two lines: CPU per thread role (thr[...]). Used by the printer
 * thread only. The number a controller needs afterwards is left in a public field.
 */
internal class SysSampler {
    private var primed = false
    private val thr0 = HashMap<String, LongArray>() // tid -> [utime, stime]

    /** CPU cores this process used over the last interval (all threads, user + system); 0 = unknown. */
    @JvmField var procCores = 0.0

    /** The block (with a leading space), or "" before the first interval has passed. */
    fun blocks(secs: Double): String {
        val threads = readThreads()
        val sb = StringBuilder(160)
        if (primed) threadBlock(sb, secs, threads)
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
        procCores = (user.sum() + sys.sum()) / ticks
        sb.append(" thr[all=").append(fmt2(procCores))
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
