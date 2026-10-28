package feed.stats

import feed.platform.Monitor
import feed.platform.locked
import feed.platform.nanoTime
import feed.platform.rssBytes
import feed.platform.startThread
import feed.platform.threadScratch
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

    /** Starts the printer thread; [intervalS] <= 0 disables it. */
    fun start(intervalS: Int) {
        if (intervalS <= 0) return
        startThread("stats") {
            var prevC = LongArray(C_COUNT)
            var prevLat = LongArray(EP_COUNT * BUCKETS)
            var prevDb = LongArray(BUCKETS)
            var prevDbw = LongArray(BUCKETS)
            var prevT = nanoTime()
            val t0 = prevT
            while (true) {
                sleepNanos(intervalS * 1_000_000_000L)
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
                val now = nanoTime()
                val secs = (now - prevT) / 1e9
                println(line(now - t0, secs, c, prevC, lat, prevLat, db, prevDb, dbw, prevDbw))
                prevC = c; prevLat = lat; prevDb = db; prevDbw = dbw; prevT = now
            }
        }
    }

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
        val dbwd = LongArray(BUCKETS) { dbw[it] - pdw[it] }
        sb.append(" dbw_p50=").append(ms(pct(dbwd, 0.50))).append(" dbw_p99=").append(ms(pct(dbwd, 0.99)))
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
    private fun fmt1(v: Double): String {
        val x = kotlin.math.round(v * 10).toLong()
        return "${x / 10}.${x % 10}"
    }

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
