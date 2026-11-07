package challenge

import java.net.InetAddress

private const val MAX_DB_CONNECTIONS = 512

private fun env(name: String, fallback: String): String {
    val v = System.getenv(name)
    return if (v.isNullOrEmpty()) fallback else v
}

private fun required(name: String): String {
    val v = System.getenv(name)
    if (v.isNullOrEmpty()) throw IllegalStateException("$name is not set")
    return v
}

fun main() {
    val start = System.nanoTime()
    val secret = required("JWT_SECRET")
    val db = PgConfig.parse(required("DATABASE_URL"))
    val host = InetAddress.getByName(env("HOST", "0.0.0.0"))
    val extra = env("EXTRA_PORT", "")
    val port = env("PORT", "3000").toInt()
    val ports = if (extra.isEmpty()) intArrayOf(port) else intArrayOf(port, extra.toInt())
    val loops = env("LOOPS", Runtime.getRuntime().availableProcessors().toString()).toInt()
    val pool = env("DB_POOL", "64").toInt()
    val writers = maxOf(1, env("DB_WRITERS", "4").toInt())
    val readers = maxOf(1, minOf((pool + loops - 1) / loops, MAX_DB_CONNECTIONS / loops - writers))
    if (loops * (readers + writers) > MAX_DB_CONNECTIONS) throw IllegalStateException("too many loops for 512 connections")

    val gzipLevel = env("GZIP_LEVEL", "1").toInt()
    val gzipMin = env("GZIP_MIN", "1024").toInt()
    val cfg = LoopConfig(
        readers, writers, env("PACK_MAX", "64").toInt(), gzipLevel, gzipMin,
        env("BATCH_MIN", "128").toInt(), env("BATCH_MICROS", "4000").toLong() * 1000,
    )

    val ls = Array(loops) { i -> Loop(i, db, cfg, Jwt(secret), host, ports, start) }
    System.err.println(
        "listening on ${host.hostAddress} ports ${ports.contentToString()}: $loops loops, each with $readers read and " +
            "$writers write connections to ${db.host}:${db.port}; gzip " +
            (if (gzipLevel > 0) "from $gzipMin bytes" else "off") + "; batching " +
            (if (cfg.batchNanos > 0) "under ${cfg.batchMin} events, ${cfg.batchNanos / 1000} us" else "off"),
    )
    val threads = Array(loops) { i ->
        val t = Thread(ls[i], "loop-$i")
        t.setUncaughtExceptionHandler { th, e ->
            System.err.println("${th.name} died: $e")
            e.printStackTrace()
            Runtime.getRuntime().halt(1)
        }
        t.start()
        t
    }
    for (t in threads) t.join()
}
