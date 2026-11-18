package challenge

/** One client connection. Idle, it is this object and its slot in the loop's fd table: no buffers. */
class Conn(@JvmField val fd: Int) {
    @JvmField var left: ByteArray? = null
    @JvmField var pending: ByteArray? = null
    @JvmField var pendingFrom = 0
    @JvmField var closeAfterWrite = false
    @JvmField var closed = false
    @JvmField var gzip = false

    @JvmField var waiting = false
    @JvmField var since = 0
    @JvmField var op: Byte = 0
    @JvmField var last = false
    @JvmField var rows = 0
    @JvmField var id = 0L
    @JvmField var userId = 0L
    @JvmField var ts = 0L
    @JvmField var text: ByteArray? = null
    @JvmField var user: ByteArray? = null
}
