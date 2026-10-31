package feed.platform

// The only platform-specific surface. Everything else (HTTP parsing, the Postgres protocol, batching, JSON, JWT,
// bcrypt, handlers) is common Kotlin and compiles unchanged for the JVM and for Kotlin/Native (linuxX64).

/**
 * Called first thing in main. Native: ignores SIGPIPE (a write to a closed socket must not kill the process) and
 * raises the RLIMIT_NOFILE soft limit to the hard limit (tens of thousands of client sockets; the JVM does this
 * itself via -XX:+MaxFDLimit). Returns a one-line description for the startup log.
 */
expect fun platformInit(): String

/** Monotonic clock in nanoseconds. */
expect fun nanoTime(): Long

/** Wall clock, milliseconds since the Unix epoch. */
expect fun currentTimeMillis(): Long

/** Fills [dst] with cryptographically secure random bytes (bcrypt salts, SCRAM nonces). */
expect fun secureRandomBytes(dst: ByteArray)

expect fun getenv(name: String): String?

/** CPUs this process may run on (affinity-aware where the platform allows). */
expect fun cpuCount(): Int

/**
 * Starts a platform (OS) thread running [body]. [nice] > 0 lowers this thread's scheduling priority (Linux nice
 * value, per thread) where the platform allows it; bcrypt workers use it so logins never take CPU from requests.
 */
expect fun startThread(name: String, nice: Int = 0, body: () -> Unit)

/** Blocks the calling thread forever (main thread after startup). */
expect fun parkForever(): Nothing

/** A mutex with one condition, like a Java monitor. Not reentrant. */
expect class Monitor() {
    fun lock()
    fun unlock()
    /** Must hold the lock. Releases it while waiting; spurious wakeups are possible. */
    fun await()
    /** Must hold the lock. Waits at most [nanos]; spurious wakeups are possible. */
    fun awaitNanos(nanos: Long)
    fun signal()
    fun signalAll()
}

inline fun <T> Monitor.locked(block: () -> T): T {
    lock()
    try {
        return block()
    } finally {
        unlock()
    }
}

/** A blocking TCP client socket with TCP_NODELAY (the Postgres connections; one reader thread each). */
expect class BlockingSocket {
    /** Reads up to [len] bytes; blocks until at least one byte arrives. Returns -1 at EOF. Throws on error. */
    fun read(b: ByteArray, off: Int, len: Int): Int

    /** Writes all [len] bytes, blocking as needed. Throws on error. */
    fun writeFully(b: ByteArray, off: Int, len: Int)

    fun close()

    /**
     * What the kernel knows about this TCP connection (TCP_INFO): [smoothed rtt us, rto us, retransmits in the current
     * backoff, segments retransmitted in all, unacked segments, send cwnd]. Empty where unavailable (JVM variant) or closed.
     */
    fun tcpInfo(): LongArray

    companion object {
        fun connect(host: String, port: Int): BlockingSocket
    }
}

/**
 * HMAC-SHA256 with a fixed key. An instance is NOT thread-safe: use one per thread (see [threadScratch]).
 * JVM: javax.crypto (SHA-NI/AVX2 intrinsics). Native: [feed.crypto.HmacSha256Impl] (pure Kotlin).
 */
expect class HmacSha256(key: ByteArray) {
    /** Writes the 32-byte MAC of data[off, off+len) to out[outOff, outOff+32). */
    fun mac(data: ByteArray, off: Int, len: Int, out: ByteArray, outOff: Int)
}

/** SHA-256 digest (one-shot; SCRAM only, not on hot paths). */
expect fun sha256(data: ByteArray): ByteArray

/** Per-thread scratch objects (one per OS thread, created lazily). */
class Scratch {
    /** HMAC keyed with JWT_SECRET; set by [feed.auth.Jwt]. */
    var jwtMac: HmacSha256? = null
    val mac = ByteArray(32)
    val tmp = ByteArray(4096)
    /** This thread's compressor and its output buffer, created on the first gzip response it sends. */
    var gzip: Gzip? = null
    var gzBuf: ByteArray? = null
    /** This thread's counters and histograms ([feed.stats.Stats]), registered on first use. */
    var stats: feed.stats.ThreadStats? = null
}

expect fun threadScratch(): Scratch

/**
 * One-shot gzip (RFC 1952) compressor with reusable state. NOT thread-safe: one per thread (see [Scratch.gzip]).
 * Native: zlib (Kotlin/Native's platform.zlib, the system libz). JVM: java.util.zip.Deflater + CRC32.
 */
expect class Gzip(level: Int) {
    /** Compresses src[off, off+len) into dst from [dstOff]; returns the compressed size, or -1 if dst is too small. */
    fun compress(src: ByteArray, off: Int, len: Int, dst: ByteArray, dstOff: Int): Int
}

/** Resident set size of this process in bytes (/proc/self/statm), 0 if unknown. Stats only. */
expect fun rssBytes(): Long

/** A small /proc or /sys file as text, "" if it cannot be read: the stats line's kernel and process counters (feed.stats.Sys). */
expect fun readProcFile(path: String): String

/** The entry names of a directory (the thread ids under /proc/self/task), empty if it cannot be read. */
expect fun listDir(path: String): List<String>

/** True where the event loops honour feed.tune.LoopTuning (the native epoll loops); the JVM variant's NIO loops do not. */
expect val loopBatchingSupported: Boolean

/** The last garbage collection as [epoch, first pause in us, objects marked, live heap after in MiB]; empty if unknown. */
expect fun gcInfo(): LongArray

/**
 * Starts the HTTP/1.1 listener(s) and [loops] event-loop threads, then returns. Each loop owns the connections it
 * accepted (SO_REUSEPORT listener per loop where available, listen backlog 65535, TCP_NODELAY on every accepted
 * socket, non-blocking I/O). The loops feed bytes to [feed.http.HttpEngine] and perform writes it requests; all
 * HTTP logic lives in common code. Accept errors (EMFILE/ENFILE) pause accepting for ~100 ms instead of spinning.
 */
expect fun startHttpServer(host: String, ports: IntArray, loops: Int, engine: feed.http.HttpEngine)
