package feed.db

import feed.pg.PgConn
import feed.pg.PgError
import feed.pg.PgIoException
import feed.pg.PgStatement
import feed.pg.Result
import feed.pg.beginBind
import feed.pg.endBindExecute
import feed.pg.sync
import feed.platform.Monitor
import feed.platform.locked
import feed.platform.nanoTime
import feed.platform.startThread
import feed.stats.C_DB_BATCH
import feed.http.Exchange
import feed.stats.C_DB_ERR
import feed.stats.C_DROPPED
import feed.stats.C_DB_JOBS
import feed.stats.C_SHED
import feed.stats.C_WR_BATCH
import feed.stats.C_WR_JOBS
import feed.stats.C_WR_RETRY
import feed.stats.Stats
import feed.stats.bucketOf
import feed.util.ByteBuf
import kotlin.concurrent.atomics.AtomicInt
import kotlin.coroutines.Continuation
import kotlin.coroutines.intrinsics.COROUTINE_SUSPENDED
import kotlin.coroutines.intrinsics.suspendCoroutineUninterceptedOrReturn
import kotlin.coroutines.resume
import kotlin.jvm.JvmField

/**
 * Postgres access. Every request runs exactly ONE of the SPEC.md statements (rule 3), prepared once per connection,
 * binary results. Two kinds of lanes, each one connection served by its own thread:
 *
 * - read lanes (SELECT, and /health's SELECT 1): take everything queued (up to [MAX_BATCH]), send it in one write as
 *   Bind/Execute/Sync per request (pipelining: one round trip for the batch, each request still its own statement
 *   and its own implicit transaction), read the replies in order, resume each handler on this thread.
 * - write lanes (the two INSERTs): group commit. A batch of queued writes goes out as BEGIN; w1; ...; wn; COMMIT;
 *   Sync in one round trip, and handlers resume only after COMMIT's ReadyForQuery (rule 8: committed before we
 *   respond). If anything in the batch fails (a like on a missing post: 23503), the transaction is rolled back and
 *   each write is re-run on its own, so one bad request never fails the others.
 *
 * Queues are bounded ([queueMax]): beyond that a request fails at once (500) instead of piling up without limit; the
 * scorer never restarts a wedged server, so shedding beats stalling.
 */
object Db {
    // statement ids
    const val FEED = 0
    const val POST = 1
    const val INSERT_POST = 2
    const val INSERT_LIKE = 3
    const val PING = 4
    private const val BEGIN = 5
    private const val COMMIT = 6
    private const val ROLLBACK = 7

    private const val POST_SELECT = "SELECT p.id, p.body, p.created_at, u.username,\n" +
        "       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count\n" +
        "  FROM posts p JOIN users u ON u.id = p.user_id"

    private val statements = listOf(
        PgStatement("s0", "$POST_SELECT\n ORDER BY p.created_at DESC, p.id DESC LIMIT 20", IntArray(0), "feed"),
        PgStatement("s1", "$POST_SELECT\n WHERE p.id = $1", intArrayOf(20), "post"),
        PgStatement("s2", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", intArrayOf(20, 25), "insert post"),
        PgStatement("s3", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", intArrayOf(20, 20), "insert like"),
        PgStatement("s4", "SELECT 1", IntArray(0), "ping"),
        PgStatement("s5", "BEGIN", IntArray(0), "begin"),
        PgStatement("s6", "COMMIT", IntArray(0), "commit"),
        PgStatement("s7", "ROLLBACK", IntArray(0), "rollback"),
    )
    private val names = statements.map { PgConn.cstr(it.name) }
    private val nParams = statements.map { it.oids.size }

    private const val MAX_BATCH = 128
    private const val POOL = 4096

    /**
     * Queue caps. Well above what a passing load ever queues (Little's law: 42k req/s x 0.5 s p95 is ~21k in flight
     * at the very knee), so they only shed in a real overload; below that they would add errors at the knee.
     */
    @JvmField var readQueueMax = 65_536
    @JvmField var writeQueueMax = 8_192

    /** Gather windows (see JobQueue): microseconds a lane waits for a fuller batch, and the batch size that ends the wait. 0 = take whatever is queued. Lab (4 vCPUs, 45k req/s): 2 ms / 16 cut server CPU per request ~13% and Postgres CPU ~10%
     * against no window, tails unchanged; bigger windows (4-8 ms) gained little more. */
    @JvmField var readGatherMicros = 2_000
    @JvmField var readGatherMin = 32
    @JvmField var writeGatherMicros = 2_000
    @JvmField var writeGatherMin = 4
    private lateinit var cfg: DbConfig
    private val reads = JobQueue()
    private val writes = JobQueue()
    /** DB_READ_SYNC=each gives every read its own Sync (one reply flush per statement); the default is one Sync per batch. */
    var readSyncEach = false

    /** Each lane's current connection (null while it reconnects): reads then writes. For the stats line's view of the Postgres link. */
    private val laneConns = arrayOfNulls<PgConn>(1024)
    private var nRead = 0

    /**
     * The Postgres link as the kernel sees it, summed over the lanes' sockets: [connections, sum of smoothed rtt (us), max
     * rto (us), segments retransmitted in all, connections in retransmit backoff, unacked segments].
     */
    fun linkInfo(): LongArray {
        var n = 0L
        var rtt = 0L
        var rto = 0L
        var retrans = 0L
        var backoff = 0L
        var unacked = 0L
        for (c in laneConns) {
            val t = c?.tcpInfo() ?: continue
            if (t.isEmpty()) continue
            n++
            rtt += t[0]
            rto = maxOf(rto, t[1])
            if (t[2] > 0) backoff++
            retrans += t[3]
            unacked += t[4]
        }
        return longArrayOf(n, rtt, rto, retrans, backoff, unacked)
    }

    private val pool = JobPool(POOL)
    private val connected = AtomicInt(0)
    private var lastError: String = "not connected yet"

    /** Opens [readConns] + [writeConns] lanes; each connects (and reconnects) on its own thread. */
    fun start(config: DbConfig, readConns: Int, writeConns: Int) {
        cfg = config
        nRead = readConns
        reads.configure(readGatherMicros, readGatherMin)
        writes.configure(writeGatherMicros, writeGatherMin)
        for (i in 0 until readConns) startThread("pg-read-$i") { lane(i, reads, write = false) }
        for (i in 0 until writeConns) startThread("pg-write-$i") { lane(i, writes, write = true) }
    }

    fun queueDepth(): String = "${reads.size()}/${writes.size()}"

    /**
     * A pooled job for [owner]'s request; add the parameters, then pass it to [run], which recycles it after the
     * handler resumed. A read whose owner is [Exchange.abandoned] by the time a lane takes it is dropped unrun.
     */
    fun job(stmt: Int, owner: Exchange?): Job {
        val j = pool.get()
        j.stmt = stmt
        j.owner = owner
        j.params.len = 0
        j.result.clear()
        return j
    }

    /**
     * Runs the job and returns its result. The handler resumes on the lane's thread; the result's rows point into that
     * connection's receive buffer, valid until the handler suspends again or returns (one statement per request, so
     * handlers just render and answer). A full queue or no connection at all gives an error result without waiting.
     */
    suspend fun run(j: Job): Result {
        val q = if (j.stmt == INSERT_POST || j.stmt == INSERT_LIKE) writes else reads
        if (connected.load() == 0) {
            j.result.error = PgError("08006", "database unreachable: $lastError")
            return j.result
        }
        return suspendCoroutineUninterceptedOrReturn { c ->
            j.cont = c
            j.enq = nanoTime()
            if (q.offer(j, if (q === writes) writeQueueMax else readQueueMax)) COROUTINE_SUSPENDED
            else {
                Stats.local().c[C_SHED]++
                j.cont = null
                j.result.error = PgError("53300", "server overloaded: database queue full")
                j.result
            }
        }
    }

    private fun lane(index: Int, q: JobQueue, write: Boolean) {
        val batch = arrayOfNulls<Job>(MAX_BATCH)
        var conn: PgConn? = null
        var backoff = 100L
        while (true) {
            val c = conn ?: try {
                PgConn.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database, cfg.options,
                    appName = "feed-${if (write) "w" else "r"}$index").also {
                    try {
                        it.prepare(statements)
                    } catch (e: Throwable) {
                        it.close() // a failed handshake must not leak its socket and buffers on every retry
                        throw e
                    }
                    connected.addAndFetch(1)
                    laneConns[if (write) nRead + index else index] = it
                    backoff = 100L
                }
            } catch (e: Throwable) {
                lastError = e.message ?: e.toString()
                println("db: lane ${if (write) "w" else "r"}$index connect failed: $lastError")
                sleepMs(backoff)
                backoff = minOf(backoff * 2, 2000L)
                continue
            }
            conn = c
            var n = q.take(batch)
            if (!write) n = dropAbandoned(batch, n)
            if (n == 0) continue
            try {
                if (write) writeBatch(c, batch, n) else readBatch(c, batch, n)
            } catch (e: Throwable) {
                // Anything at all (not only I/O): the connection state is unknown, so drop it and reconnect. The lane
                // thread itself must never die (the multicore lab: dead worker threads decided the score).
                lastError = e.message ?: e.toString()
                logSafe("db: lane ${if (write) "w" else "r"}$index lost its connection", e)
                connected.addAndFetch(-1)
                laneConns[if (write) nRead + index else index] = null
                c.close()
                conn = null
                for (i in 0 until n) {
                    val j = batch[i] ?: continue
                    j.result.clear()
                    j.result.error = PgError("08006", lastError)
                    finish(j)
                }
            }
            for (i in 0 until n) batch[i] = null
            c.beginReply()
        }
    }

    private fun encode(o: ByteBuf, j: Job) {
        val lp = o.beginBind(names[j.stmt], nParams[j.stmt])
        o.put(j.params.buf, 0, j.params.len)
        o.endBindExecute(lp)
    }

    private fun encodeBare(o: ByteBuf, stmt: Int) {
        val lp = o.beginBind(names[stmt], 0)
        o.endBindExecute(lp)
    }

    private fun readBatch(c: PgConn, batch: Array<Job?>, n: Int) {
        val st = Stats.local()
        st.c[C_DB_JOBS] += n.toLong()
        if (readSyncEach) {
            st.c[C_DB_BATCH]++
            val o = c.out
            o.reset()
            for (i in 0 until n) {
                encode(o, batch[i]!!)
                o.sync()
            }
            c.send()
            for (i in 0 until n) readUntilReady(c, batch[i]!!.result)
            for (i in 0 until n) {
                val j = batch[i]!!
                batch[i] = null
                finish(j)
            }
            return
        }
        // One Sync for the whole batch: Postgres sends the replies of all its statements together (a few full packets)
        // instead of flushing one small packet per statement, and runs them in one implicit transaction. Each handler
        // resumes as soon as its own rows are in (they are valid until it returns). If a read fails, Postgres skips the
        // rest of the batch until the Sync: those reads were not run, so they go out again in a new round trip.
        var from = 0
        while (from < n) {
            st.c[C_DB_BATCH]++ // a round trip: the first, and each one that re-sends the reads an error made Postgres skip
            val o = c.out
            o.reset()
            for (i in from until n) encode(o, batch[i]!!)
            o.sync()
            c.send()
            var failed = -1
            var i = from
            while (i < n) {
                val j = batch[i]!!
                if (readStatement(c, j.result)) {
                    failed = i
                    break
                }
                batch[i] = null
                finish(j)
                i++
            }
            drainToReady(c)
            if (failed < 0) return
            val fj = batch[failed]!!
            batch[failed] = null
            finish(fj)
            from = failed + 1
        }
    }

    private fun writeBatch(c: PgConn, batch: Array<Job?>, n: Int) {
        val st = Stats.local()
        if (n > 1) {
            val o = c.out
            o.reset()
            encodeBare(o, BEGIN)
            for (i in 0 until n) encode(o, batch[i]!!)
            encodeBare(o, COMMIT)
            o.sync()
            c.send()
            // BEGIN, each write, COMMIT: one result each, then one ReadyForQuery. After an error Postgres skips the
            // rest until Sync, so the results after it stay empty; the transaction status says if it committed.
            val scratch = Result()
            var failed = false
            readOne(c, scratch)
            for (i in 0 until n) if (readOne(c, batch[i]!!.result)) {
                failed = true
                break
            }
            if (!failed) readOne(c, scratch)
            val tx = drainToReady(c)
            if (!failed && scratch.error == null && tx == 'I'.code) {
                st.c[C_WR_BATCH]++
                st.c[C_WR_JOBS] += n.toLong()
                for (i in 0 until n) finish(batch[i]!!.also { batch[i] = null })
                return
            }
            // roll back if still in the failed transaction, then run each write on its own
            if (tx != 'I'.code) {
                o.reset()
                encodeBare(o, ROLLBACK)
                o.sync()
                c.send()
                drainToReady(c)
            }
            st.c[C_WR_RETRY]++
            for (i in 0 until n) batch[i]!!.result.clear()
        }
        val o = c.out
        o.reset()
        for (i in 0 until n) {
            encode(o, batch[i]!!)
            o.sync()
        }
        c.send()
        for (i in 0 until n) readUntilReady(c, batch[i]!!.result)
        st.c[C_WR_BATCH] += n.toLong()
        st.c[C_WR_JOBS] += n.toLong()
        for (i in 0 until n) finish(batch[i]!!.also { batch[i] = null })
    }

    /**
     * Reads one statement's reply (BindComplete, rows, CommandComplete or ErrorResponse) inside a group commit, up to
     * but not including ReadyForQuery. Returns true if it was an error.
     */
    private fun readOne(c: PgConn, r: Result): Boolean {
        while (true) {
            when (c.next()) {
                '2'.code, 'n'.code -> {}
                'D'.code -> r.addRow(c.rbuf, c.mstart)
                'C'.code -> { r.affected = c.tagCount(); return false }
                'I'.code -> return false
                'E'.code -> { r.error = c.error(); return true }
                'Z'.code -> { c.pushBack(); return false }
                else -> {} // NoticeResponse, ParameterStatus
            }
        }
    }

    /**
     * One read's reply in a batch that shares a Sync: BindComplete, rows, CommandComplete (false) or ErrorResponse (true).
     * A ReadyForQuery before the statement completed is a protocol desync: the connection is dropped, never answered as an
     * empty result.
     */
    private fun readStatement(c: PgConn, r: Result): Boolean {
        while (true) {
            when (c.next()) {
                '2'.code, 'n'.code -> {}
                'D'.code -> r.addRow(c.rbuf, c.mstart)
                'C'.code -> { r.affected = c.tagCount(); return false }
                'I'.code -> return false
                'E'.code -> { r.error = c.error(); return true }
                'Z'.code -> throw PgIoException("protocol: ReadyForQuery before a read completed")
                else -> {} // NoticeResponse, ParameterStatus
            }
        }
    }

    /** Skips to the next ReadyForQuery and returns its transaction status. */
    private fun drainToReady(c: PgConn): Int {
        while (true) if (c.next() == 'Z'.code) return c.txStatus()
    }

    /** One request's full reply up to and including its ReadyForQuery. */
    private fun readUntilReady(c: PgConn, r: Result) {
        while (true) {
            when (c.next()) {
                '2'.code, 'n'.code, 'I'.code -> {}
                'D'.code -> r.addRow(c.rbuf, c.mstart)
                'C'.code -> r.affected = c.tagCount()
                'E'.code -> if (r.error == null) r.error = c.error()
                'Z'.code -> return
                else -> {}
            }
        }
    }

    /** Resumes the handler (on this thread), records the DB time, recycles the job. */
    private fun finish(j: Job) {
        val st = Stats.local()
        val h = if (j.stmt == INSERT_POST || j.stmt == INSERT_LIKE) st.dbw else st.db
        h[bucketOf(nanoTime() - j.enq)]++
        if (j.result.error != null) st.c[C_DB_ERR]++
        val cont = j.cont
        j.cont = null
        j.owner = null
        try {
            cont?.resume(j.result)
        } catch (e: Throwable) { // one failing handler costs its own request, never the batch or the lane
            logSafe("db: handler failed after its statement", e)
        }
        pool.put(j)
    }

    /** Moves the reads whose client is still there to the front of the batch; returns how many. */
    private fun dropAbandoned(batch: Array<Job?>, n: Int): Int {
        var k = 0
        for (i in 0 until n) {
            val j = batch[i]!!
            batch[i] = null
            if (j.owner?.abandoned == true) {
                Stats.local().c[C_DROPPED]++
                j.cont = null // never resumed: the client is gone and its response buffer was never taken
                j.owner = null
                pool.put(j)
            } else {
                batch[k++] = j
            }
        }
        return k
    }

    private val logged = AtomicInt(0)

    /** Logging that cannot throw: full detail for the first few, one short line after that. */
    private fun logSafe(what: String, e: Throwable) {
        try {
            if (logged.addAndFetch(1) <= 10) println("$what: ${e.stackTraceToString()}") else println("$what: $e")
        } catch (_: Throwable) {
        }
    }

    private val sleeper = Monitor()
    private fun sleepMs(ms: Long) = sleeper.locked { sleeper.awaitNanos(ms * 1_000_000) }
}

/** One statement execution for one request. */
class Job {
    @JvmField var stmt = 0
    /** Binary parameter values, each Int32 length + bytes, in statement order. */
    @JvmField val params = ByteBuf(256)
    @JvmField val result = Result()
    @JvmField var cont: Continuation<Result>? = null
    @JvmField var enq = 0L
    @JvmField var owner: Exchange? = null

    fun int8(v: Long): Job { params.putIntBE(8); params.putLongBE(v); return this }
    fun text(s: String): Job {
        val p = params.len
        params.putIntBE(0)
        params.putUtf8(s)
        params.setIntBE(p, params.len - p - 4)
        return this
    }
}

/**
 * A bounded FIFO of jobs shared by the lanes of one kind; a lane takes everything waiting (up to its batch size).
 *
 * Gather window ([gatherNanos] > 0): a lane that wakes up to fewer than [minBatch] queued jobs waits for more, but never
 * longer than [gatherNanos] after the OLDEST queued job arrived, so the added latency is bounded by the window. Fuller
 * batches mean fewer round trips, writes, reads and lane wake-ups per request; the idle lanes are only woken for the
 * first job and again once [minBatch] have gathered, not for every offer.
 */
private class JobQueue {
    private val lock = Monitor()
    private var items = arrayOfNulls<Job>(1024)
    private var head = 0
    private var count = 0
    private var gatherNanos = 0L
    private var minBatch = 1

    fun configure(gatherMicros: Int, minBatch: Int) {
        this.gatherNanos = gatherMicros * 1_000L
        this.minBatch = maxOf(1, minBatch)
    }

    fun size(): Int = count

    fun offer(j: Job, max: Int): Boolean = lock.locked {
        if (count >= max) return@locked false
        if (count == items.size) {
            val n = arrayOfNulls<Job>(items.size * 2)
            for (i in 0 until count) n[i] = items[(head + i) % items.size]
            items = n
            head = 0
        }
        items[(head + count) % items.size] = j
        count++
        if (count == 1 || (gatherNanos > 0 && count == minBatch)) lock.signal()
        true
    }

    /** Blocks until at least one job is queued (and, with a gather window, until it is full or old enough); moves up to batch.size of them into [batch]. */
    fun take(batch: Array<Job?>): Int = lock.locked {
        while (true) {
            while (count == 0) lock.await()
            if (gatherNanos > 0 && count < minBatch) {
                val deadline = items[head]!!.enq + gatherNanos
                while (count in 1 until minBatch) {
                    val left = deadline - nanoTime()
                    if (left <= 0) break
                    lock.awaitNanos(left)
                }
                if (count == 0) continue // another lane took them while we waited
            }
            break
        }
        val n = minOf(count, batch.size)
        for (i in 0 until n) {
            batch[i] = items[head]
            items[head] = null
            head = (head + 1) % items.size
        }
        count -= n
        if (count > 0) lock.signal() // more waiting: wake another lane
        n
    }
}

private class JobPool(cap: Int) {
    private val lock = Monitor()
    private val items = arrayOfNulls<Job>(cap)
    private var size = 0

    fun get(): Job = lock.locked { if (size > 0) items[--size].also { items[size] = null } else null } ?: Job()

    fun put(j: Job) {
        if (j.params.buf.size > 4096) return
        lock.locked { if (size < items.size) items[size++] = j }
    }
}

/** DATABASE_URL: postgres://user:password@host:port/database (percent-encoded parts decoded). */
class DbConfig(val host: String, val port: Int, val user: String, val password: String, val database: String, val options: String) {
    companion object {
        fun fromUrl(url: String, options: String): DbConfig {
            val rest = url.substringAfter("://", "")
            require(rest.isNotEmpty()) { "DATABASE_URL must look like postgres://user:pass@host:port/db" }
            val at = rest.lastIndexOf('@')
            val userInfo = if (at >= 0) rest.substring(0, at) else ""
            val hostPart = if (at >= 0) rest.substring(at + 1) else rest
            val slash = hostPart.indexOf('/')
            val hostPort = if (slash >= 0) hostPart.substring(0, slash) else hostPart
            val db = if (slash >= 0) hostPart.substring(slash + 1).substringBefore('?') else ""
            val colon = hostPort.lastIndexOf(':')
            val host = if (colon > 0 && !hostPort.endsWith("]")) hostPort.substring(0, colon) else hostPort
            val port = if (colon > 0 && !hostPort.endsWith("]")) hostPort.substring(colon + 1).toInt() else 5432
            val user = decode(userInfo.substringBefore(':'))
            val pass = if (':' in userInfo) decode(userInfo.substringAfter(':')) else ""
            return DbConfig(host.removePrefix("[").removeSuffix("]"), port, user, pass, decode(db).ifEmpty { user }, options)
        }

        private fun decode(s: String): String {
            if ('%' !in s) return s
            val out = ByteBuf(s.length)
            var i = 0
            while (i < s.length) {
                val ch = s[i]
                if (ch == '%' && i + 2 < s.length) {
                    out.put(s.substring(i + 1, i + 3).toInt(16))
                    i += 3
                } else {
                    out.putUtf8(ch.toString())
                    i++
                }
            }
            return out.buf.decodeToString(0, out.len)
        }
    }
}
