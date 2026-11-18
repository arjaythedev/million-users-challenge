package challenge

private val HEX: ByteArray = ascii("0123456789abcdef")
private const val PG_EPOCH_MILLIS = 946_684_800_000L
private const val DAY_MILLIS = 86_400_000L

fun i32(b: ByteArray, at: Int): Int =
    (b[at].toInt() and 0xff shl 24) or (b[at + 1].toInt() and 0xff shl 16) or
        (b[at + 2].toInt() and 0xff shl 8) or (b[at + 3].toInt() and 0xff)

fun i64(b: ByteArray, at: Int): Long = (i32(b, at).toLong() shl 32) or (i32(b, at + 4).toLong() and 0xffffffffL)

/** A growable byte buffer: Postgres messages, response bodies, JSON. */
class Buf(capacity: Int) {
    @JvmField var a = ByteArray(capacity)
    @JvmField var len = 0

    fun ensure(n: Int) {
        if (len + n > a.size) a = a.copyOf(maxOf(a.size * 2, len + n))
    }

    fun put(b: Int) {
        ensure(1)
        a[len++] = b.toByte()
    }

    fun put(s: ByteArray) = put(s, 0, s.size)

    fun put(s: ByteArray, off: Int, n: Int) {
        ensure(n)
        System.arraycopy(s, off, a, len, n)
        len += n
    }

    fun i16(v: Int) {
        ensure(2)
        a[len++] = (v ushr 8).toByte()
        a[len++] = v.toByte()
    }

    fun i32(v: Int) {
        ensure(4)
        setI32(len, v)
        len += 4
    }

    fun setI32(at: Int, v: Int) {
        a[at] = (v ushr 24).toByte()
        a[at + 1] = (v ushr 16).toByte()
        a[at + 2] = (v ushr 8).toByte()
        a[at + 3] = v.toByte()
    }

    fun i64(v: Long) {
        i32((v ushr 32).toInt())
        i32(v.toInt())
    }

    fun cstr(s: String) {
        ensure(s.length + 1)
        for (k in 0 until s.length) a[len++] = s[k].code.toByte()
        a[len++] = 0
    }

    fun num(value: Long) {
        var v = value
        if (v < 0) {
            put('-'.code)
            v = -v
        }
        val n = digitCount(v)
        ensure(n)
        var p = len + n
        do {
            a[--p] = ('0'.code + (v % 10).toInt()).toByte()
            v /= 10
        } while (v != 0L)
        len += n
    }

    /** JSON string contents: quote, backslash and control bytes escaped; UTF-8 passes through. */
    fun escaped(s: ByteArray, off: Int, n: Int) {
        ensure(n)
        val end = off + n
        var run = off
        for (k in off until end) {
            val c = s[k].toInt()
            if ((c >= 0x20 && c != '"'.code && c != '\\'.code) || c < 0) continue
            put(s, run, k - run)
            run = k + 1
            when (c) {
                '"'.code -> { put('\\'.code); put('"'.code) }
                '\\'.code -> { put('\\'.code); put('\\'.code) }
                '\n'.code -> { put('\\'.code); put('n'.code) }
                '\r'.code -> { put('\\'.code); put('r'.code) }
                '\t'.code -> { put('\\'.code); put('t'.code) }
                '\b'.code -> { put('\\'.code); put('b'.code) }
                0x0c -> { put('\\'.code); put('f'.code) }
                else -> {
                    ensure(6)
                    a[len++] = '\\'.code.toByte()
                    a[len++] = 'u'.code.toByte()
                    a[len++] = '0'.code.toByte()
                    a[len++] = '0'.code.toByte()
                    a[len++] = HEX[c shr 4]
                    a[len++] = HEX[c and 15]
                }
            }
        }
        put(s, run, end - run)
    }

    /** A TIMESTAMPTZ in binary (microseconds since 2000-01-01 UTC) as yyyy-MM-ddTHH:mm:ss.SSSZ, truncated. */
    fun timestamp(pgMicros: Long) {
        val millis = Math.floorDiv(pgMicros, 1000L) + PG_EPOCH_MILLIS
        val days = Math.floorDiv(millis, DAY_MILLIS)
        val ms = Math.floorMod(millis, DAY_MILLIS).toInt()
        val z = days + 719_468
        val era = Math.floorDiv(z, 146_097L)
        val doe = (z - era * 146_097).toInt()
        val yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365
        val doy = doe - (365 * yoe + yoe / 4 - yoe / 100)
        val mp = (5 * doy + 2) / 153
        val day = doy - (153 * mp + 2) / 5 + 1
        val month = if (mp < 10) mp + 3 else mp - 9
        val year = yoe + era * 400 + (if (month <= 2) 1 else 0)
        ensure(24)
        digits(year.toInt(), 4)
        a[len++] = '-'.code.toByte()
        digits(month, 2)
        a[len++] = '-'.code.toByte()
        digits(day, 2)
        a[len++] = 'T'.code.toByte()
        digits(ms / 3_600_000, 2)
        a[len++] = ':'.code.toByte()
        digits(ms / 60_000 % 60, 2)
        a[len++] = ':'.code.toByte()
        digits(ms / 1000 % 60, 2)
        a[len++] = '.'.code.toByte()
        digits(ms % 1000, 3)
        a[len++] = 'Z'.code.toByte()
    }

    private fun digits(value: Int, width: Int) {
        var v = value
        for (k in width - 1 downTo 0) {
            a[len + k] = ('0'.code + v % 10).toByte()
            v /= 10
        }
        len += width
    }
}
