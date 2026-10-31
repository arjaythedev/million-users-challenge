package feed.stats

import feed.platform.Monitor
import feed.platform.locked
import feed.platform.nanoTime
import feed.platform.gcInfo
import feed.platform.readProcFile
import feed.platform.rssBytes
import feed.platform.startThread
import feed.platform.threadScratch
import feed.tune.BatchTuner
import feed.tune.Epoch
import feed.tune.LoopTuning
import kotlin.jvm.JvmField

/**
 * Built-in instrumentation: per-thread counters and log-bucket latency histograms (plain longs, written only by their
 * thread, no atomics on the hot path), summed racily by one printer thread every STATS_INTERVAL_S seconds into one
 * line on stdout. The numbers are for finding what to optimize (rig-load.sh keeps the server log next to its samples).
 */
class ThreadStats {
    @JvmField val c = LongArray(C_COUNT)
    /** Response time per endpoint (dispatch -> response handed to the socket), [BUCKETS] buckets each. */
    @JvmField val lat = LongArray(EP_COUNT * BUCKETS)
    /** Postgres time per job (enqueued -> reply read): reads (SELECTs) and writes (INSERTs, incl. group commit). */
    @JvmField val db = LongArray(BUCKETS)
    @JvmField val dbw = LongArray(BUCKETS)
}

/**
 * One event loop's own counters, written only by its thread and read racily by the printer: how often it woke up, how
 * many ready descriptors those wake-ups carried and how long it was busy handling them rather than waiting.
 */
class LoopMeter {
    @JvmField var wakes = 0L
    @JvmField var events = 0L
    @JvmField var busyNs = 0L
    // the printer's snapshot at the previous line
    @JvmField var pWakes = 0L
    @JvmField var pEvents = 0L
    @JvmField var pBusyNs = 0L
}

// endpoints
const val EP_HEALTH = 0
const val EP_FEED = 1
const val EP_POST = 2
const val EP_CREATE = 3
const val EP_LIKE = 4
const val EP_OTHER = 5
const val EP_COUNT = 6
private val EP_NAMES = arrayOf("health", "feed", "post", "create", "like", "other")

// counters
const val C_ACCEPT = 0
const val C_CLOSE = 1
const val C_S2XX = 2
const val C_S4XX = 3
const val C_S5XX = 4
const val C_GZ_IN = 5        // bytes before compression
const val C_GZ_OUT = 6       // bytes after
const val C_GZ_NS = 7        // time spent compressing
const val C_BODY_OUT = 8     // response body bytes sent (after compression)
const val C_PARTIAL_WRITE = 9
const val C_DB_BATCH = 10    // round trips (read lanes)
const val C_DB_JOBS = 11     // statements executed (read lanes)
const val C_WR_BATCH = 12    // group commits
const val C_WR_JOBS = 13     // writes in them
const val C_WR_RETRY = 14    // group commits that failed and were retried one by one
const val C_SHED = 15        // requests refused because a DB queue was full
const val C_DB_ERR = 16
const val C_GZ_N = 17        // compressed responses
const val C_DROPPED = 18      // queued reads skipped because the client had gone
const val C_COUNT = 19

// indexes into the vector Proc.net() returns (feed.stats.Sys)
const val NET_TCP_IN = 0
const val NET_TCP_OUT = 1
const val NET_TCP_RETRANS = 2
const val NET_TCP_RTO = 3
const val NET_LISTEN_DROPS = 4
const val NET_BACKLOG_DROP = 5
const val NET_RCVQ_DROP = 6
const val NET_SOFTNET_DROP = 7
const val NET_SOFTNET_SQUEEZE = 8
const val NET_NIC_RX_PKTS = 9
const val NET_NIC_RX_DROP = 10
const val NET_NIC_TX_PKTS = 11
const val NET_NIC_TX_DROP = 12
const val NET_NIC_RX_BYTES = 13
const val NET_NIC_TX_BYTES = 14
const val NET_COUNT = 15

/** Postgres round trips this slow count as stalled; a window whose server p99 is this slow prints a "knee:" line. */
private const val SLOW_US = 150_000L
private const val KNEE_P99_US = 150_000L

/** Log buckets: 4 per power of two of microseconds (~19% wide), up to ~2^27 us (134 s). */
const val BUCKETS = 112

fun bucketOf(nanos: Long): Int {
    val us = nanos / 1000
    if (us < 4) return us.toInt().coerceAtLeast(0)
    val msb = 63 - us.countLeadingZeroBits()
    val b = msb * 4 + ((us shr (msb - 2)).toInt() and 3)
    return if (b >= BUCKETS) BUCKETS - 1 else b
}

/** Upper bound of a bucket in microseconds. */
private fun bucketUs(b: Int): Long {
    if (b < 16) return b.toLong() + 1
    val msb = b / 4
    return (4L + (b and 3) + 1) shl (msb - 2)
}

object Stats {
    private val lock = Monitor()
    private val all = ArrayList<ThreadStats>()

    private val loops = ArrayList<LoopMeter>()

    /** An event loop registers its meter once, when it is created. */
    fun registerLoop(): LoopMeter = LoopMeter().also { m -> lock.locked { loops.add(m) } }

    /** This thread's stats (registered on first use). */
    fun local(): ThreadStats {
        val s = threadScratch()
        s.stats?.let { return it }
        val t = ThreadStats()
        lock.locked { all.add(t) }
        s.stats = t
        return t
    }

    /** Gauges the printer shows next to the counters (set once at startup by Main). */
    var queueDepth: () -> String = { "-" }

    /** The Postgres link as the kernel sees it: see [feed.db.Db.linkInfo] (empty = not available). */
    var linkInfo: () -> LongArray = { LongArray(0) }
    private var prevRetrans = -1L

    /** Set by [line] when the interval looked like a knee (slow tail, slow Postgres round trips, shedding or 5xx); the printer prints it. */
    private var kneeLine: String? = null

    /** The sums over all threads at one moment (racy, which is fine for statistics). */
    private class Snap(val c: LongArray, val lat: LongArray, val db: LongArray, val dbw: LongArray, val t: Long)

    private fun collect(): Snap {
        val c = LongArray(C_COUNT)
        val lat = LongArray(EP_COUNT * BUCKETS)
        val db = LongArray(BUCKETS)
        val dbw = LongArray(BUCKETS)
        val threads = lock.locked { all.toTypedArray() }
        for (t in threads) {
            for (i in c.indices) c[i] += t.c[i]
            for (i in lat.indices) lat[i] += t.lat[i]
            for (i in db.indices) db[i] += t.db[i]
            for (i in dbw.indices) dbw[i] += t.dbw[i]
        }
        return Snap(c, lat, db, dbw, nanoTime())
    }

    /** The tuner, if any, and the CPUs it measures (the ones this process may run on). */
    private var tuner: BatchTuner? = null
    private var allowedCpus = IntArray(0)
    private var prevBusy = -1L
    private var prevTotal = -1L

    /**
     * Starts the printer thread: it wakes every [tuneEpochS] seconds while a [tuner] is given (else every [intervalS]),
     * lets the tuner look at the epoch, and prints one line every [intervalS] seconds ([intervalS] <= 0: never).
     */
    fun start(intervalS: Int, tuneEpochS: Int = 0, tuner: BatchTuner? = null) {
        val epochS = if (tuner != null && tuneEpochS > 0) tuneEpochS else intervalS
        if (epochS <= 0) return
        this.tuner = tuner
        if (tuner != null) allowedCpus = Proc.cpusAllowed(readProcFile("/proc/self/status"))
        val printEvery = if (intervalS > 0) maxOf(1, intervalS / epochS) else Int.MAX_VALUE
        startThread("stats") {
            val first = collect()
            var printPrev = first
            var epochPrev = first
            var n = 0
            if (tuner != null) cpuTicks() // the first reading is the baseline
            while (true) {
                sleepNanos(epochS * 1_000_000_000L)
                val cur = collect()
                if (tuner != null) {
                    try {
                        tuneStep(tuner, epochPrev, cur)
                    } catch (e: Throwable) { // never leave the loops on a probe's setting because the tuner failed
                        println("tune: failed ($e): back to the prior ${tuner.priorUs} us")
                        LoopTuning.sleepUs.store(tuner.priorUs)
                    }
                }
                epochPrev = cur
                if (++n % printEvery == 0) {
                    try {
                        println(line(cur.t - first.t, (cur.t - printPrev.t) / 1e9, cur.c, printPrev.c, cur.lat, printPrev.lat, cur.db, printPrev.db, cur.dbw, printPrev.dbw))
                        kneeLine?.let { println(it) }
                    } catch (e: Throwable) {
                        println("stats: could not build the line: $e")
                    }
                    printPrev = cur
                }
            }
        }
    }

    /** Busy and total ticks of the allowed CPUs since the previous call; null the first time or if /proc/stat cannot be read. */
    private fun cpuTicks(): LongArray? {
        val st = Proc.stat(readProcFile("/proc/stat")) ?: return null
        val b = Proc.busyOf(st, allowedCpus)
        val t = Proc.totalOf(st, allowedCpus)
        val d = if (prevBusy < 0 || prevTotal < 0) null else longArrayOf(b - prevBusy, t - prevTotal)
        prevBusy = b
        prevTotal = t
        return d
    }

    private fun tuneStep(t: BatchTuner, a: Snap, b: Snap) {
        val all = LongArray(BUCKETS)
        var reqs = 0L
        for (e in 0 until EP_COUNT) {
            for (k in 0 until BUCKETS) {
                val v = b.lat[e * BUCKETS + k] - a.lat[e * BUCKETS + k]
                all[k] += v
                reqs += v
            }
        }
        val secs = (b.t - a.t) / 1e9
        val p99 = if (reqs > 0) bucketUs(pct(all, 0.99)) else 0L
        val ticks = cpuTicks()
        val cost = if (ticks != null && ticks[0] >= 0 && reqs > 0) ticks[0] * 10_000.0 / reqs else 0.0 // a tick is 10 ms
        val busy = if (ticks != null && ticks[1] > 0) (ticks[0].toDouble() / ticks[1]).coerceIn(0.0, 1.0) else -1.0
        LoopTuning.sleepUs.store(t.onEpoch(Epoch(secs, reqs, cost, p99, b.c[C_SHED] - a.c[C_SHED], busy)))
    }

    /** What the printer says about the box and the process between two lines (printer thread only). */
    private val sys = SysSampler()
    private var prevGcEpoch = -1L

    private fun line(
        up: Long, secs: Double, c: LongArray, pc: LongArray, lat: LongArray, pl: LongArray, db: LongArray, pd: LongArray,
        dbw: LongArray, pdw: LongArray,
    ): String {
        fun d(i: Int) = c[i] - pc[i]
        fun rate(i: Int) = (d(i) / secs).toLong()
        val sb = StringBuilder(256)
        sb.append("stats t=").append(up / 1_000_000_000).append("s conns=").append(c[C_ACCEPT] - c[C_CLOSE])
        sb.append(" acc/s=").append(rate(C_ACCEPT))
        val all = LongArray(BUCKETS)
        var reqs = 0L
        sb.append(" req/s=")
        val parts = ArrayList<String>()
        for (e in 0 until EP_COUNT) {
            var n = 0L
            for (b in 0 until BUCKETS) {
                val v = lat[e * BUCKETS + b] - pl[e * BUCKETS + b]
                all[b] += v; n += v
            }
            reqs += n
            if (n > 0) parts.add("${EP_NAMES[e]}:${(n / secs).toLong()}")
        }
        sb.append((reqs / secs).toLong()).append(" [").append(parts.joinToString(" ")).append(']')
        sb.append(" p50=").append(ms(pct(all, 0.50))).append(" p99=").append(ms(pct(all, 0.99)))
        val dbd = LongArray(BUCKETS) { db[it] - pd[it] }
        sb.append(" dbr_p50=").append(ms(pct(dbd, 0.50))).append(" dbr_p99=").append(ms(pct(dbd, 0.99)))
        var slow = 0L // read jobs that took Postgres 150 ms or more: a lost segment costs at least the 200 ms minimum RTO
        for (b in 0 until BUCKETS) if (bucketUs(b) >= SLOW_US) slow += dbd[b]
        sb.append(" dbr_slow=").append(slow)
        val dbwd = LongArray(BUCKETS) { dbw[it] - pdw[it] }
        sb.append(" dbw_p50=").append(ms(pct(dbwd, 0.50))).append(" dbw_p99=").append(ms(pct(dbwd, 0.99)))
        var slowW = 0L // write jobs (group commits included) that took 150 ms or more from queue to reply
        for (b in 0 until BUCKETS) if (bucketUs(b) >= SLOW_US) slowW += dbwd[b]
        sb.append(" dbw_slow=").append(slowW)
        val batches = d(C_DB_BATCH)
        sb.append(" db_jobs/rt=").append(if (batches > 0) fmt1(d(C_DB_JOBS).toDouble() / batches) else "-")
        val wb = d(C_WR_BATCH)
        sb.append(" wr_jobs/commit=").append(if (wb > 0) fmt1(d(C_WR_JOBS).toDouble() / wb) else "-")
        sb.append(" dbq=").append(queueDepth())
        sb.append(" 4xx=").append(d(C_S4XX)).append(" 5xx=").append(d(C_S5XX)).append(" shed=").append(d(C_SHED))
        sb.append(" dropped=").append(d(C_DROPPED)).append(" dberr=").append(d(C_DB_ERR)).append(" retry=").append(d(C_WR_RETRY))
        val gin = d(C_GZ_IN)
        if (gin > 0) {
            sb.append(" gz=").append(d(C_GZ_OUT) * 100 / gin).append('%')
            sb.append(" gz_us=").append(fmt1(d(C_GZ_NS) / 1000.0 / maxOf(1L, d(C_GZ_N))))
        }
        sb.append(" out=").append((d(C_BODY_OUT) / secs / 1_048_576).let(::fmt1)).append("MB/s")
        sb.append(" partial_w=").append(d(C_PARTIAL_WRITE))
        sb.append(" rss=").append(rssBytes() / 1_048_576).append("MB")
        val gc = gcInfo()
        if (gc.isNotEmpty()) {
            sb.append(" gc[n=").append(if (prevGcEpoch < 0) 0 else gc[0] - prevGcEpoch).append(" pause=").append(fmt1(gc[1] / 1000.0))
                .append("ms marked=").append(fmt1(gc[2] / 1e6)).append("M heap=").append(gc[3]).append("MiB]")
            prevGcEpoch = gc[0]
        }
        val lm = lock.locked { loops.toTypedArray() }
        var loopBusyMax = 0.0
        if (lm.isNotEmpty()) {
            var wakes = 0L
            var events = 0L
            var busySum = 0L
            var busyMax = 0L
            for (m in lm) {
                val w = m.wakes
                val e = m.events
                val b = m.busyNs
                wakes += w - m.pWakes
                events += e - m.pEvents
                busySum += b - m.pBusyNs
                busyMax = maxOf(busyMax, b - m.pBusyNs)
                m.pWakes = w; m.pEvents = e; m.pBusyNs = b
            }
            val wall = secs * 1e9
            loopBusyMax = busyMax / wall
            sb.append(" loop[n=").append(lm.size).append(" wake=").append(fmt1(wakes / secs / 1000.0)).append("k/s ev/wake=")
                .append(if (wakes > 0) fmt1(events.toDouble() / wakes) else "-")
                .append(" busy=").append((busySum * 100 / (wall * lm.size)).toLong()).append("%(max ")
                .append((busyMax * 100 / wall).toLong()).append("%)]")
        }
        tuner?.let { sb.append(' ').append(it.status()) }
        val link = linkInfo()
        var pgRetrans = 0.0
        if (link.size >= 6 && link[0] > 0) {
            val dr = if (prevRetrans >= 0 && link[3] >= prevRetrans) link[3] - prevRetrans else 0L // a reconnect restarts a socket's count
            prevRetrans = link[3]
            pgRetrans = dr / secs
            sb.append(" pg[n=").append(link[0]).append(" rtt=").append(fmt1(link[1].toDouble() / link[0] / 1000.0))
                .append("ms rto_max=").append(fmt1(link[2] / 1000.0)).append("ms retrans/s=").append(fmt1(pgRetrans))
                .append(" in_backoff=").append(link[4]).append(" unacked=").append(link[5]).append(']')
        }
        sb.append(sys.blocks(secs, reqs))
        val p99b = pct(all, 0.99)
        kneeLine = if (reqs > 0 && (bucketUs(p99b) >= KNEE_P99_US || slow > 0 || slowW > 0 || d(C_SHED) > 0 || d(C_S5XX) > 0)) {
            // short and self-contained, so a log excerpt of it alone says what the box was doing at the knee
            StringBuilder(320).append("knee: t=").append(up / 1_000_000_000).append("s req/s=").append((reqs / secs).toLong())
                .append(" p50=").append(ms(pct(all, 0.50))).append(" p99=").append(ms(p99b))
                .append(" dbr_p99=").append(ms(pct(dbd, 0.99))).append(" dbr_slow=").append(slow).append(" dbw_slow=").append(slowW)
                .append(" shed=").append(d(C_SHED)).append(" 5xx=").append(d(C_S5XX))
                .append(" pg_retrans/s=").append(fmt1(pgRetrans)).append(" tcp_retrans=").append(sys.tcpRetrans)
                .append(" rto=").append(sys.tcpRto).append(" drops=").append(sys.drops)
                .append(" hot=cpu").append(sys.hotCpu).append(':').append((sys.hotBusy * 100).toLong())
                .append("%(soft ").append((sys.hotSoft * 100).toLong()).append("%) netrx=cpu").append(sys.rxCpu).append(':')
                .append((sys.rxShare * 100).toLong()).append("% box=").append((sys.boxBusy * 100).toLong())
                .append("% loop_max=").append((loopBusyMax * 100).toLong()).append("% conns=").append(c[C_ACCEPT] - c[C_CLOSE])
                .toString()
        } else null
        return sb.toString()
    }

    private fun pct(h: LongArray, p: Double): Int {
        val n = h.sum()
        if (n == 0L) return -1
        val want = kotlin.math.ceil(p * n).toLong()
        var acc = 0L
        for (b in h.indices) {
            acc += h[b]
            if (acc >= want) return b
        }
        return h.size - 1
    }

    private fun ms(b: Int) = if (b < 0) "-" else fmt1(bucketUs(b) / 1000.0) + "ms"
    private val sleeper = Monitor()
    private fun sleepNanos(n: Long) = sleeper.locked {
        val end = nanoTime() + n
        while (true) {
            val left = end - nanoTime()
            if (left <= 0) break
            sleeper.awaitNanos(left)
        }
    }
}
