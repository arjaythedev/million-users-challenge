package feed.platform

import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.nativeHeap
import kotlinx.cinterop.ptr
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.sizeOf
import kotlinx.cinterop.usePinned
import platform.posix.O_RDONLY
import platform.posix.close
import platform.posix.open
import platform.posix.read
import platform.posix.sysconf
import platform.posix._SC_PAGESIZE
import platform.zlib.ZLIB_VERSION
import platform.zlib.Z_DEFAULT_STRATEGY
import platform.zlib.Z_DEFLATED
import platform.zlib.Z_FINISH
import platform.zlib.Z_OK
import platform.zlib.Z_STREAM_END
import platform.zlib.deflate
import platform.zlib.deflateInit2_
import platform.zlib.deflateReset
import platform.zlib.z_stream

/** zlib deflate with a gzip wrapper (windowBits 15 + 16); the stream lives in native memory for the thread's life. */
actual class Gzip actual constructor(level: Int) {
    private val strm = nativeHeap.alloc<z_stream>()

    init {
        strm.zalloc = null
        strm.zfree = null
        strm.opaque = null
        strm.next_in = null
        strm.avail_in = 0u
        val rc = deflateInit2_(strm.ptr, level, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY, ZLIB_VERSION, sizeOf<z_stream>().toInt())
        check(rc == Z_OK) { "deflateInit2: $rc" }
    }

    actual fun compress(src: ByteArray, off: Int, len: Int, dst: ByteArray, dstOff: Int): Int {
        if (len == 0 || dstOff >= dst.size) return -1
        deflateReset(strm.ptr)
        return src.usePinned { s ->
            dst.usePinned { d ->
                strm.next_in = s.addressOf(off).reinterpret()
                strm.avail_in = len.toUInt()
                strm.next_out = d.addressOf(dstOff).reinterpret()
                strm.avail_out = (dst.size - dstOff).toUInt()
                val rc = deflate(strm.ptr, Z_FINISH)
                strm.next_in = null
                strm.next_out = null
                if (rc != Z_STREAM_END) -1 else dst.size - dstOff - strm.avail_out.toInt()
            }
        }
    }
}

private val pageSize: Long = sysconf(_SC_PAGESIZE)

actual fun rssBytes(): Long {
    val fd = open("/proc/self/statm", O_RDONLY)
    if (fd < 0) return 0
    val buf = ByteArray(128)
    val n = buf.usePinned { read(fd, it.addressOf(0), buf.size.toULong()) }.toInt()
    close(fd)
    if (n <= 0) return 0
    // "size resident shared ...": the second field, in pages
    var i = 0
    while (i < n && buf[i] != ' '.code.toByte()) i++
    i++
    var pages = 0L
    while (i < n && buf[i] in '0'.code.toByte()..'9'.code.toByte()) pages = pages * 10 + (buf[i++] - '0'.code.toByte())
    return pages * pageSize
}
