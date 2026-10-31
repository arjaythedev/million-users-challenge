package feed

import feed.app.Api
import feed.auth.Jwt
import feed.db.Db
import feed.db.DbConfig
import feed.http.HttpConfig
import feed.http.HttpEngine
import feed.platform.cpuCount
import feed.platform.getenv
import feed.platform.loopBatchingSupported
import feed.platform.parkForever
import feed.platform.platformInit
import feed.platform.startHttpServer
import feed.stats.Stats
import feed.tune.BatchTuner
import feed.tune.LoopTuning

/**
 * The feed API server. Config from the environment (SPEC.md runtime contract): DATABASE_URL, JWT_SECRET, HOST, PORT,
 * EXTRA_PORT. Process knobs, all optional, defaults sized from the CPUs this process may run on (sched_getaffinity;
 * the server never pins its threads): HTTP_THREADS, DB_READ_CONNS, DB_WRITE_CONNS, DB_READ_QUEUE_MAX, DB_WRITE_QUEUE_MAX, DB_OPTIONS,
 * GZIP_LEVEL (0 = off), GZIP_MIN_BYTES, STATS_INTERVAL_S (0 = off), LOOP_TUNE (auto, on, off or microseconds), TUNE_EPOCH_S,
 * TUNE_COMFORT_MS, TUNE_MARGIN_PCT.
 */
fun main() {
    val platform = platformInit()
    val dbUrl = getenv("DATABASE_URL") ?: error("DATABASE_URL is required")
    val secret = getenv("JWT_SECRET") ?: error("JWT_SECRET is required")
    val host = getenv("HOST") ?: "127.0.0.1"
    val port = envInt("PORT", 3000)
    val extraPort = getenv("EXTRA_PORT")?.takeIf { it.isNotBlank() }?.toInt()

    val cpus = cpuCount()
    val httpThreads = envInt("HTTP_THREADS", cpus)
    // at most 48: on a replica of the official boxes (PR #56) 48 read connections held a million users at 16 cores where
    // 128 did not: more connections spread the same results over more, smaller packets, and the Postgres link lost more
    val readConns = envInt("DB_READ_CONNS", (cpus * 4).coerceIn(4, 48))
    // at least 4: the multicore lab's 2 write connections sat ~100% busy waiting on the WAL flush and set p99
    val writeConns = envInt("DB_WRITE_CONNS", (cpus / 2).coerceIn(4, 8))
    require(readConns + writeConns <= 512) { "at most 512 Postgres connections (rule 5)" }
    Db.readQueueMax = envInt("DB_READ_QUEUE_MAX", Db.readQueueMax)
    Db.writeQueueMax = envInt("DB_WRITE_QUEUE_MAX", Db.writeQueueMax)
    Db.readGatherMicros = envInt("DB_GATHER_US", Db.readGatherMicros)
    Db.readGatherMin = envInt("DB_GATHER_MIN", Db.readGatherMin)
    Db.writeGatherMicros = envInt("DB_WRITE_GATHER_US", Db.writeGatherMicros)
    Db.writeGatherMin = envInt("DB_WRITE_GATHER_MIN", Db.writeGatherMin)
    Db.readSyncEach = getenv("DB_READ_SYNC")?.trim()?.lowercase() == "each"
    HttpConfig.gzipLevel = envInt("GZIP_LEVEL", 1)
    HttpConfig.gzipMinBytes = envInt("GZIP_MIN_BYTES", 1024)
    // off by default (nobody reads the log of a scored run); the lab scripts set STATS_INTERVAL_S=10. The tuner still runs (TUNE_EPOCH_S)
    // and prints its own decisions.
    val statsInterval = envInt("STATS_INTERVAL_S", 0)
    val options = getenv("DB_OPTIONS") ?: "-c plan_cache_mode=force_generic_plan"

    Jwt.init(secret)
    Db.start(DbConfig.fromUrl(dbUrl, options), readConns, writeConns)
    Stats.queueDepth = Db::queueDepth
    Stats.linkInfo = Db::linkInfo
    // Event-loop wake-up batching: LOOP_TUNE=auto (default) starts at the prior (2 ms) and lets BatchTuner probe off/1/2/4 ms
    // against the CPU per request while the server runs (only while it is comfortable, and an alternative must be TUNE_MARGIN_PCT
    // cheaper); "off" or "0" = never sleep; "on" = fixed 2 ms; a number = fixed us.
    val loopTune = (getenv("LOOP_TUNE")?.trim()?.lowercase()?.takeIf { it.isNotEmpty() }) ?: "auto"
    val prior = 2000
    val tuner = if (loopTune == "auto" && loopBatchingSupported && statsInterval + envInt("TUNE_EPOCH_S", 5) > 0) {
        BatchTuner(
            intArrayOf(prior, 0, 1000, 4000, 6000, 8000), comfortP99Us = envInt("TUNE_COMFORT_MS", 100) * 1000L,
            adoptMargin = envInt("TUNE_MARGIN_PCT", 8) / 100.0, log = { println(it) },
        )
    } else null
    LoopTuning.sleepUs.store(
        if (!loopBatchingSupported) 0 else when (loopTune) {
            "auto", "on" -> prior
            "off" -> 0
            else -> loopTune.toIntOrNull()?.also { require(it in 0..100_000) { "LOOP_TUNE=$loopTune: 0..100000 us" } }
                ?: error("invalid LOOP_TUNE=$loopTune (auto, on, off or microseconds)")
        },
    )
    Stats.start(statsInterval, envInt("TUNE_EPOCH_S", 5), tuner)
    val engine = HttpEngine { ex -> Api.handle(ex) }
    startHttpServer(host, if (extraPort != null) intArrayOf(port, extraPort) else intArrayOf(port), httpThreads, engine)
    println(
        "feed: listening on $host:$port${extraPort?.let { ",$it" } ?: ""} (cpus=$cpus http=$httpThreads " +
            "db=$readConns+$writeConns gzip=${HttpConfig.gzipLevel}/${HttpConfig.gzipMinBytes}B stats=${statsInterval}s; $platform)",
    )
    parkForever()
}

internal fun envInt(name: String, default: Int): Int =
    getenv(name)?.takeIf { it.isNotBlank() }?.let { v -> v.toIntOrNull() ?: error("invalid $name=$v") } ?: default
