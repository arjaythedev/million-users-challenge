package challenge

import java.lang.foreign.Arena
import java.lang.foreign.FunctionDescriptor
import java.lang.foreign.Linker
import java.lang.foreign.MemoryLayout
import java.lang.foreign.MemorySegment
import java.lang.foreign.ValueLayout.ADDRESS
import java.lang.foreign.ValueLayout.JAVA_BYTE
import java.lang.foreign.ValueLayout.JAVA_INT
import java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_LONG
import java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED
import java.lang.foreign.ValueLayout.JAVA_SHORT
import java.lang.invoke.MethodHandle
import java.net.Inet4Address
import java.net.Inet6Address
import java.net.InetAddress
import java.nio.ByteOrder

const val AF_INET = 2
const val AF_INET6 = 10
const val SOCK_STREAM = 1
const val SOCK_NONBLOCK = 0x800
const val SOCK_CLOEXEC = 0x80000
const val SOL_SOCKET = 1
const val SO_REUSEADDR = 2
const val SO_REUSEPORT = 15
const val SO_RCVTIMEO = 20
const val SO_SNDTIMEO = 21
const val IPPROTO_TCP = 6
const val TCP_NODELAY = 1
const val TCP_DEFER_ACCEPT = 9
const val TCP_QUICKACK = 12
const val F_SETFL = 4
const val O_NONBLOCK = 0x800
const val EPOLL_CLOEXEC = 0x80000
const val EPOLL_CTL_ADD = 1
const val EPOLL_CTL_MOD = 3
const val EPOLLIN = 0x1
const val EPOLLOUT = 0x4
const val EPOLLERR = 0x8
const val EPOLLHUP = 0x10
const val EINTR = 4
const val EAGAIN = 11
const val ECONNABORTED = 103

// struct epoll_event is packed on x86_64 (12 bytes, data at 4) and naturally aligned elsewhere (16 bytes, data at 8).
private val X86: Boolean = when (System.getProperty("os.arch")) {
    "amd64", "x86_64" -> true
    else -> false
}
@JvmField val EVENT_SIZE: Int = if (X86) 12 else 16
private val EVENT_DATA: Long = if (X86) 4L else 8L

private val LINKER: Linker = Linker.nativeLinker()
private val CRITICAL: Linker.Option = Linker.Option.critical(false)
private val ERRNO: Linker.Option = Linker.Option.captureCallState("errno")
private val STATE_LAYOUT: MemoryLayout = Linker.Option.captureStateLayout()
private val ERRNO_OFFSET: Long = STATE_LAYOUT.byteOffset(MemoryLayout.PathElement.groupElement("errno"))

private fun fn(name: String, fd: FunctionDescriptor, vararg options: Linker.Option): MethodHandle {
    val f = LINKER.defaultLookup().find(name).orElseThrow { UnsatisfiedLinkError(name) }
    return LINKER.downcallHandle(f, fd, *options)
}

private val SOCKET: MethodHandle = fn("socket", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT), ERRNO)
private val SETSOCKOPT: MethodHandle =
    fn("setsockopt", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), CRITICAL)
private val BIND: MethodHandle = fn("bind", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), ERRNO)
private val LISTEN: MethodHandle = fn("listen", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT), ERRNO)
private val CONNECT: MethodHandle = fn("connect", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), ERRNO)
private val FCNTL: MethodHandle =
    fn("fcntl", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT), Linker.Option.firstVariadicArg(2), ERRNO)
private val ACCEPT4: MethodHandle =
    fn("accept4", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT), CRITICAL, ERRNO)
private val EPOLL_CREATE1: MethodHandle = fn("epoll_create1", FunctionDescriptor.of(JAVA_INT, JAVA_INT), ERRNO)
private val EPOLL_CTL: MethodHandle =
    fn("epoll_ctl", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS), CRITICAL)
private val EPOLL_WAIT: MethodHandle =
    fn("epoll_wait", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), ERRNO)
private val READ: MethodHandle = fn("read", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG), CRITICAL, ERRNO)
private val WRITE: MethodHandle = fn("write", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG), CRITICAL, ERRNO)
private val READ_BLOCKING: MethodHandle = fn("read", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG), ERRNO)
private val WRITE_BLOCKING: MethodHandle = fn("write", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG), ERRNO)
private val CLOSE: MethodHandle = fn("close", FunctionDescriptor.of(JAVA_INT, JAVA_INT), CRITICAL)
private val GETSOCKOPT: MethodHandle =
    fn("getsockopt", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS), CRITICAL)

fun eventFd(events: MemorySegment, i: Int): Int = events.get(JAVA_INT_UNALIGNED, i.toLong() * EVENT_SIZE + EVENT_DATA)

fun eventTag(events: MemorySegment, i: Int): Int = events.get(JAVA_INT_UNALIGNED, i.toLong() * EVENT_SIZE + EVENT_DATA + 4)

fun eventFlags(events: MemorySegment, i: Int): Int = events.get(JAVA_INT_UNALIGNED, i.toLong() * EVENT_SIZE)

fun closeFd(fd: Int) {
    CLOSE.invokeExact(fd) as Int
}

/** libc through FFM, one per loop thread: the errno capture and the scratch memory are not shared. */
class Sys {
    private val state: MemorySegment = Arena.global().allocate(STATE_LAYOUT)
    private val scratch: MemorySegment = Arena.global().allocate(64, 8)

    fun errno(): Int = state.get(JAVA_INT, ERRNO_OFFSET)

    fun listen(host: InetAddress, port: Int, backlog: Int): Int {
        val fd = socket(host, SOCK_STREAM or SOCK_NONBLOCK or SOCK_CLOEXEC)
        setInt(fd, SOL_SOCKET, SO_REUSEADDR, 1)
        setInt(fd, SOL_SOCKET, SO_REUSEPORT, 1)
        setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1)
        setInt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, 5)
        val sa = sockaddr(host, port)
        if (BIND.invokeExact(state, fd, sa, sa.byteSize().toInt()) as Int != 0) {
            throw IllegalStateException("bind ${host.hostAddress}:$port: errno ${errno()}")
        }
        if (LISTEN.invokeExact(state, fd, backlog) as Int != 0) throw IllegalStateException("listen: errno ${errno()}")
        setInt(fd, IPPROTO_TCP, TCP_QUICKACK, 0)
        return fd
    }

    /** A blocking socket with send and receive timeouts; a negative result is -errno. */
    fun connect(host: InetAddress, port: Int, timeoutMillis: Int): Int {
        val fd = socket(host, SOCK_STREAM or SOCK_CLOEXEC)
        setTimeout(fd, SO_SNDTIMEO, timeoutMillis)
        setTimeout(fd, SO_RCVTIMEO, timeoutMillis)
        val sa = sockaddr(host, port)
        if (CONNECT.invokeExact(state, fd, sa, sa.byteSize().toInt()) as Int != 0) {
            val e = errno()
            closeFd(fd)
            return -e
        }
        setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1)
        return fd
    }

    fun setNonBlocking(fd: Int) {
        if (FCNTL.invokeExact(state, fd, F_SETFL, O_NONBLOCK) as Int != 0) throw IllegalStateException("fcntl: errno ${errno()}")
    }

    private fun socket(host: InetAddress, type: Int): Int {
        val fd = SOCKET.invokeExact(state, if (host is Inet6Address) AF_INET6 else AF_INET, type, 0) as Int
        if (fd < 0) throw IllegalStateException("socket: errno ${errno()}")
        return fd
    }

    private fun sockaddr(host: InetAddress, port: Int): MemorySegment {
        val v6 = host is Inet6Address
        val sa = scratch.asSlice(0L, if (v6) 28L else 16L).fill(0)
        sa.set(JAVA_SHORT, 0L, (if (v6) AF_INET6 else AF_INET).toShort())
        sa.set(JAVA_SHORT.withOrder(ByteOrder.BIG_ENDIAN), 2L, port.toShort())
        val addr = host.address
        MemorySegment.copy(addr, 0, sa, JAVA_BYTE, if (host is Inet4Address) 4L else 8L, addr.size)
        return sa
    }

    fun setInt(fd: Int, level: Int, name: Int, value: Int): Int {
        scratch.set(JAVA_INT, 32L, value)
        return SETSOCKOPT.invokeExact(fd, level, name, scratch.asSlice(32L, 4L), 4) as Int
    }

    private fun setTimeout(fd: Int, name: Int, millis: Int) {
        scratch.set(JAVA_LONG, 48L, (millis / 1000).toLong())
        scratch.set(JAVA_LONG, 56L, (millis % 1000) * 1000L)
        SETSOCKOPT.invokeExact(fd, SOL_SOCKET, name, scratch.asSlice(48L, 16L), 16) as Int
    }

    fun accept(fd: Int): Int =
        ACCEPT4.invokeExact(state, fd, MemorySegment.NULL, MemorySegment.NULL, SOCK_NONBLOCK or SOCK_CLOEXEC) as Int

    fun epollCreate(): Int {
        val ep = EPOLL_CREATE1.invokeExact(state, EPOLL_CLOEXEC) as Int
        if (ep < 0) throw IllegalStateException("epoll_create1: errno ${errno()}")
        return ep
    }

    fun epollCtl(ep: Int, op: Int, fd: Int, events: Int, tag: Int): Int {
        val ev = scratch.asSlice(0L, EVENT_SIZE.toLong())
        ev.set(JAVA_INT_UNALIGNED, 0L, events)
        ev.set(JAVA_LONG_UNALIGNED, EVENT_DATA, (tag.toLong() shl 32) or (fd.toLong() and 0xffffffffL))
        return EPOLL_CTL.invokeExact(ep, op, fd, ev) as Int
    }

    fun epollWait(ep: Int, events: MemorySegment, max: Int, timeoutMillis: Int): Int =
        EPOLL_WAIT.invokeExact(state, ep, events, max, timeoutMillis) as Int

    fun read(fd: Int, buf: MemorySegment, n: Long): Long = READ.invokeExact(state, fd, buf, n) as Long

    fun write(fd: Int, buf: MemorySegment, n: Long): Long = WRITE.invokeExact(state, fd, buf, n) as Long

    fun readBlocking(fd: Int, buf: MemorySegment, n: Long): Long = READ_BLOCKING.invokeExact(state, fd, buf, n) as Long

    fun writeBlocking(fd: Int, buf: MemorySegment, n: Long): Long = WRITE_BLOCKING.invokeExact(state, fd, buf, n) as Long

    fun getInt(fd: Int, level: Int, name: Int): Int {
        scratch.set(JAVA_INT, 32L, 0)
        scratch.set(JAVA_INT, 36L, 4)
        val r = GETSOCKOPT.invokeExact(fd, level, name, scratch.asSlice(32L, 4L), scratch.asSlice(36L, 4L)) as Int
        return if (r == 0) scratch.get(JAVA_INT, 32L) else -1
    }
}
