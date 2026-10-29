package feed

import feed.app.Api
import feed.auth.Jwt
import feed.db.Db
import feed.db.DbConfig
import feed.http.HttpConfig
import feed.http.HttpEngine
import feed.platform.cpuCount
import feed.platform.getenv
import feed.platform.parkForever
import feed.platform.platformInit
import feed.platform.startHttpServer
import feed.stats.Stats

/**
 * The feed API server. Config from the environment (SPEC.md runtime contract): DATABASE_URL, JWT_SECRET, HOST, PORT,
 * EXTRA_PORT. Process knobs, all optional, defaults sized from the CPUs this process may run on (sched_getaffinity;
 * the server never pins its threads): HTTP_THREADS, DB_READ_CONNS, DB_WRITE_CONNS, DB_READ_QUEUE_MAX, DB_WRITE_QUEUE_MAX, DB_OPTIONS,
 * GZIP_LEVEL (0 = off), GZIP_MIN_BYTES, STATS_INTERVAL_S (0 = off).
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
    val readConns = envInt("DB_READ_CONNS", (cpus * 4).coerceIn(4, 64))
    // at least 4: the multicore lab's 2 write connections sat ~100% busy waiting on the WAL flush and set p99
    val writeConns = envInt("DB_WRITE_CONNS", (cpus / 2).coerceIn(4, 8))
    require(readConns + writeConns <= 512) { "at most 512 Postgres connections (rule 5)" }
    Db.readQueueMax = envInt("DB_READ_QUEUE_MAX", Db.readQueueMax)
    Db.writeQueueMax = envInt("DB_WRITE_QUEUE_MAX", Db.writeQueueMax)
    Db.readGatherMicros = envInt("DB_GATHER_US", Db.readGatherMicros)
    Db.readGatherMin = envInt("DB_GATHER_MIN", Db.readGatherMin)
    Db.writeGatherMicros = envInt("DB_WRITE_GATHER_US", Db.writeGatherMicros)
    Db.writeGatherMin = envInt("DB_WRITE_GATHER_MIN", Db.writeGatherMin)
    HttpConfig.gzipLevel = envInt("GZIP_LEVEL", 1)
    HttpConfig.gzipMinBytes = envInt("GZIP_MIN_BYTES", 1024)
    val statsInterval = envInt("STATS_INTERVAL_S", 10)
    val options = getenv("DB_OPTIONS") ?: "-c plan_cache_mode=force_generic_plan"

    Jwt.init(secret)
    Db.start(DbConfig.fromUrl(dbUrl, options), readConns, writeConns)
    Stats.queueDepth = Db::queueDepth
    Stats.start(statsInterval)
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
