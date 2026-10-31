package feed.platform

import feed.http.HttpConn
import feed.http.HttpEngine
import feed.stats.Stats
import feed.tune.LoopTuning
import kotlinx.cinterop.IntVar
import kotlinx.cinterop.Pinned
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.allocArray
import kotlinx.cinterop.convert
import kotlinx.cinterop.get
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.nativeHeap
import kotlinx.cinterop.pin
import kotlinx.cinterop.pointed
import kotlinx.cinterop.ptr
import kotlinx.cinterop.sizeOf
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.linux.EFD_NONBLOCK
import platform.linux.EPOLLERR
import platform.linux.EPOLLHUP
import platform.linux.EPOLLIN
import platform.linux.EPOLLOUT
import platform.linux.EPOLLET
import platform.linux.EPOLL_CTL_ADD
import platform.linux.EPOLL_CTL_DEL
import platform.linux.EPOLL_CTL_MOD
import platform.linux.epoll_create1
import platform.linux.epoll_ctl
import platform.linux.epoll_event
import platform.linux.epoll_wait
import platform.linux.eventfd
import platform.posix.AF_INET
import platform.posix.AI_PASSIVE
import platform.posix.EAGAIN
import platform.posix.EINTR
import platform.posix.EMFILE
import platform.posix.ENFILE
import platform.posix.ENOBUFS
import platform.posix.ENOMEM
import platform.posix.F_GETFL
import platform.posix.F_SETFL
import platform.posix.IPPROTO_TCP
import platform.posix.MSG_NOSIGNAL
import platform.posix.O_NONBLOCK
import platform.posix.SHUT_RDWR
import platform.posix.SOCK_STREAM
import platform.posix.SOL_SOCKET
import platform.posix.SO_REUSEADDR
import platform.posix.TCP_NODELAY
import platform.posix.accept
import platform.posix.addrinfo
import platform.posix.bind
import platform.posix.close
import platform.posix.errno
import platform.posix.fcntl
import platform.posix.freeaddrinfo
import platform.posix.getaddrinfo
import platform.posix.listen
import platform.posix.recv
import platform.posix.send
import platform.posix.setsockopt
import platform.posix.shutdown
import platform.posix.socket
import platform.posix.strerror
import kotlinx.cinterop.toKString
import kotlin.concurrent.AtomicInt
import kotlin.concurrent.AtomicReference

private const val SO_REUSEPORT = 15     // asm-generic/socket.h
private const val MSG_DONTWAIT = 0x40
private const val EVENTS = 512
private const val READ_CHUNK = 64 * 1024
private const val MAX_READ_ROUNDS = 8 // full chunks read from one connection per event before it is re-queued
private const val MAX_ACCEPT_FAILURES = 64 // consecutive failed accepts (not EAGAIN) before the loop moves on
/** Set in an epoll event's data for the listeners and the wakeup eventfd (low 32 bits: the fd); connections carry their slot. */
private const val SPECIAL = 0x1_0000_0000UL

actual fun startHttpServer(host: String, ports: IntArray, loops: Int, engine: HttpEngine) {
    val n = maxOf(1, loops)
    // every loop listens on every port (its own SO_REUSEPORT socket per port): the kernel spreads accepts over loops
    val fds = Array(n) { IntArray(ports.size) { k -> listenSocket(host, ports[k], reusePort = true) } }
    for (i in 0 until n) {
        val loop = EpollLoop(i, fds[i], engine)
        startThread("http-$i") { loop.run() }
    }
}

private fun listenSocket(host: String, port: Int, reusePort: Boolean): Int = memScoped {
    val hints = alloc<addrinfo>()
    hints.ai_family = AF_INET
    hints.ai_socktype = SOCK_STREAM
    hints.ai_flags = AI_PASSIVE
    val res = alloc<kotlinx.cinterop.CPointerVar<addrinfo>>()
    val bindHost = if (host == "0.0.0.0" || host.isEmpty()) null else host
    val rc = getaddrinfo(bindHost, port.toString(), hints.ptr, res.ptr)
    check(rc == 0) { "getaddrinfo $host:$port: $rc" }
    val ai = res.value!!.pointed
    val fd = socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol)
    check(fd >= 0) { "socket: ${errText()}" }
    val one = alloc<IntVar>().apply { value = 1 }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, one.ptr, sizeOf<IntVar>().convert())
    if (reusePort) {
        check(setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, one.ptr, sizeOf<IntVar>().convert()) == 0) {
            "SO_REUSEPORT: ${errText()}"
        }
    }
    setThinStream(fd) // inherited by the accepted sockets
    check(bind(fd, ai.ai_addr, ai.ai_addrlen) == 0) { "bind $host:$port: ${errText()}" }
    freeaddrinfo(res.value)
    check(listen(fd, 65535) == 0) { "listen: ${errText()}" }
    setNonBlocking(fd)
    fd
}

private fun setNonBlocking(fd: Int) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) or O_NONBLOCK)
}

private fun errText(e: Int = errno): String = strerror(e)?.toKString() ?: "errno $e"

/**
 * One event-loop thread: its own epoll set, its own SO_REUSEPORT listener, an eventfd for cross-thread wakeups.
 * Edge-triggered: a readiness event reads until a short read (at most MAX_READ_ROUNDS chunks), so a busy connection
 * can't starve the others.
 */
private class EpollLoop(val index: Int, val listenFds: IntArray, val engine: HttpEngine) {
    val epfd = epoll_create1(0).also { check(it >= 0) { "epoll_create1: ${errText()}" } }
    private val wakeFd = eventfd(0, EFD_NONBLOCK.toInt()).also { check(it >= 0) { "eventfd: ${errText()}" } }
    private val events = nativeHeap.allocArray<epoll_event>(EVENTS)
    private val scratch = ByteArray(READ_CHUNK)
    private val pinned: Pinned<ByteArray> = scratch.pin()
    private val wakeBuf = ByteArray(8).pin()
    private val wakeOne = ByteArray(8).also { it[0] = 1 }.pin() // eventfd counter += 1 (little-endian u64)

    /**
     * This loop's connections by slot, a dense per-loop index carried in the epoll event's data. Not by fd: fds are
     * process-wide, so an fd-indexed table would grow every loop's array to the whole process's fd range (at 1M
     * connections: N loops x ~8 MB of mostly null slots, all scanned by every GC mark).
     */
    private var slots = arrayOfNulls<NativeConn>(1024)
    private var freeSlots = IntArray(1024)
    private var nFree = 0
    private var nextSlot = 0

    /** Wake-ups, events and busy time for the stats line (written only by this loop's thread). */
    private val meter = Stats.registerLoop()

    /** Tasks posted from other threads, as an immutable stack swapped atomically (MPSC). */
    private val tasks = AtomicReference<TaskNode?>(null)
    /** 1 while the loop may be blocked in epoll_wait (a poster must write the eventfd). */
    private val sleeping = AtomicInt(0)
    private var acceptPausedUntil = 0L

    init {
        for (lfd in listenFds) ctl(EPOLL_CTL_ADD, lfd, (EPOLLIN or EPOLLET).toUInt())
        ctl(EPOLL_CTL_ADD, wakeFd, (EPOLLIN or EPOLLET).toUInt())
    }

    /** [tag] goes into the event's data: a connection's slot, or by default SPECIAL | fd for the listeners and wakeFd. */
    fun ctl(op: Int, fd: Int, ev: UInt, tag: ULong = SPECIAL or fd.toULong()): Int = memScoped {
        val e = alloc<epoll_event>()
        e.events = ev
        e.data.u64 = tag
        epoll_ctl(epfd, op, fd, e.ptr)
    }

    private fun allocSlot(): Int {
        if (nFree > 0) return freeSlots[--nFree]
        val s = nextSlot++
        if (s >= slots.size) {
            slots = slots.copyOf(slots.size * 2)
            freeSlots = freeSlots.copyOf(slots.size) // can hold every slot, so freeing never overflows
        }
        return s
    }

    fun run() {
        while (true) {
            var timeout = -1
            if (acceptPausedUntil != 0L) {
                val left = (acceptPausedUntil - nanoTime()) / 1_000_000
                if (left <= 0) {
                    acceptPausedUntil = 0
                    for (lfd in listenFds) ctl(EPOLL_CTL_MOD, lfd, (EPOLLIN or EPOLLET).toUInt())
                } else timeout = left.toInt() + 1
            }
            sleeping.value = 1 // then re-check tasks: a poster that saw sleeping == 0 relies on this check
            val n = epoll_wait(epfd, events, EVENTS, if (tasks.value != null) 0 else timeout)
            sleeping.value = 0
            val busyFrom = nanoTime()
            meter.wakes++
            if (n > 0) meter.events += n
            for (i in 0 until n) {
                val ev = events[i]
                val tag = ev.data.u64
                val flags = ev.events
                if (tag and SPECIAL != 0UL) {
                    val fd = tag.toInt() // the low 32 bits: a listener or the wakeup eventfd
                    if (fd == wakeFd) platform.posix.read(wakeFd, wakeBuf.addressOf(0), 8u) else acceptAll(fd)
                } else {
                    val slot = tag.toInt()
                    val c = if (slot < slots.size) slots[slot] else null
                    if (c != null) {
                        try {
                            handle(c, flags)
                        } catch (e: Throwable) { // never let one connection kill the loop and all its sockets
                            println("http-$index: connection error: $e")
                            c.close()
                        }
                    }
                }
            }
            val ran = runTasks()
            meter.busyNs += nanoTime() - busyFrom
            // Wake-up batching (LoopTuning): after a light wake-up, let work pile up instead of polling again at once. A poster
            // sees sleeping == 0 meanwhile and skips the eventfd write, so completions that arrive now cost no wake-up.
            val us = LoopTuning.sleepUs.load()
            if (us > 0 && n + ran < LoopTuning.MIN_EVENTS) pause(us)
        }
    }

    private val pauseSpec = nativeHeap.alloc<platform.posix.timespec>()

    private fun pause(us: Int) {
        pauseSpec.tv_sec = (us / 1_000_000).convert()
        pauseSpec.tv_nsec = ((us % 1_000_000) * 1000L).convert()
        platform.posix.nanosleep(pauseSpec.ptr, null)
    }

    private fun handle(c: NativeConn, flags: UInt) {
        if (flags and EPOLLOUT.toUInt() != 0u) {
            ctl(EPOLL_CTL_MOD, c.fd, (EPOLLIN or EPOLLET).toUInt(), c.slot.toULong())
            if (!c.isClosing()) engine.onWritable(c)
        }
        if (flags and (EPOLLIN.toUInt() or EPOLLHUP.toUInt() or EPOLLERR.toUInt()) != 0u) {
            // Edge-triggered: a read that fills the chunk may leave more in the socket and no new edge will come, so
            // keep reading until a short read or EAGAIN. Bounded, so a busy connection can't starve the others; past
            // the bound the connection is re-queued on this loop.
            var rounds = 0
            while (!c.isClosing()) {
                val r = recv(c.fd, pinned.addressOf(0), READ_CHUNK.convert(), 0)
                if (r > 0) {
                    engine.onData(c, scratch, 0, r.toInt())
                    if (r < READ_CHUNK) return
                    if (++rounds == MAX_READ_ROUNDS) {
                        post { if (!c.isClosing()) handle(c, EPOLLIN.toUInt()) }
                        return
                    }
                } else {
                    if (r == 0L) finishClose(c)
                    else {
                        val e = errno
                        if (e != EAGAIN && e != EINTR) finishClose(c)
                    }
                    return
                }
            }
        }
    }

    private fun acceptAll(listenFd: Int) {
        var failures = 0
        while (true) {
            val fd = accept(listenFd, null, null)
            if (fd < 0) {
                val e = errno
                if (e == EMFILE || e == ENFILE || e == ENOBUFS || e == ENOMEM) {
                    // out of descriptors: stop accepting for 100 ms instead of spinning on a readable listener
                    for (lfd in listenFds) ctl(EPOLL_CTL_MOD, lfd, 0u)
                    acceptPausedUntil = nanoTime() + 100_000_000L
                    println("http-$index: accept: ${errText(e)}; pausing accepts for 100 ms")
                    return
                }
                if (e == EAGAIN) return // drained
                // EINTR, ECONNABORTED, EPROTO...: the listener is edge-triggered, so returning would strand the rest of
                // the backlog until the next client arrives. Try again, a bounded number of times in a row.
                if (++failures > MAX_ACCEPT_FAILURES) {
                    println("http-$index: accept: ${errText(e)}; giving up for now")
                    return
                }
                continue
            }
            failures = 0
            setNonBlocking(fd)
            memScoped {
                val one = alloc<IntVar>().apply { value = 1 }
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, one.ptr, sizeOf<IntVar>().convert())
            }
            val slot = allocSlot()
            val c = NativeConn(fd, slot, this)
            slots[slot] = c
            ctl(EPOLL_CTL_ADD, fd, (EPOLLIN or EPOLLET).toUInt(), slot.toULong())
            try {
                engine.onOpen(c)
            } catch (e: Throwable) {
                println("http-$index: onOpen error: $e")
                c.close()
            }
        }
    }

    /** On the loop thread: unregister, close the fd once no other thread is writing to it, tell the engine. */
    fun finishClose(c: NativeConn) {
        if (c.released) return
        c.markClosing()
        if (!c.tryRelease()) { // another thread is inside send(): retry after it leaves
            post { finishClose(c) }
            return
        }
        ctl(EPOLL_CTL_DEL, c.fd, 0u)
        if (c.slot < slots.size && slots[c.slot] === c) {
            slots[c.slot] = null
            freeSlots[nFree++] = c.slot
        }
        close(c.fd)
        engine.onClosed(c)
    }

    fun post(task: () -> Unit) {
        while (true) {
            val head = tasks.value
            if (tasks.compareAndSet(head, TaskNode(task, head))) break
        }
        if (sleeping.value == 1) platform.posix.write(wakeFd, wakeOne.addressOf(0), 8u)
    }

    /** Runs the tasks other threads posted; returns how many there were. */
    private fun runTasks(): Int {
        var ran = 0
        while (true) {
            var node = tasks.getAndSet(null) ?: return ran
            // the stack is newest-first: reverse to run tasks in posting order
            var rev: TaskNode? = null
            var cur: TaskNode? = node
            while (cur != null) {
                val next = cur.next
                rev = TaskNode(cur.task, rev)
                cur = next
            }
            node = rev!!
            var t: TaskNode? = node
            while (t != null) {
                ran++
                try {
                    t.task()
                } catch (e: Throwable) {
                    println("http-$index: task failed: $e")
                }
                t = t.next
            }
        }
    }

    fun armWrite(c: NativeConn) {
        // epoll_ctl is thread-safe; the EPOLLOUT event is then delivered to this loop's thread
        ctl(EPOLL_CTL_MOD, c.fd, (EPOLLIN or EPOLLOUT or EPOLLET).toUInt(), c.slot.toULong())
    }
}

private class TaskNode(val task: () -> Unit, val next: TaskNode?)

/**
 * A client socket. [io] packs a "closing" bit with the number of threads currently inside send(): the fd is only
 * closed (and its number freed for reuse by a new connection) when no writer is using it, so a late response from
 * a Postgres thread can never land on another client's socket.
 */
private class NativeConn(val fd: Int, val slot: Int, val loop: EpollLoop) : HttpConn() {
    private val io = AtomicInt(0)
    var released = false // loop thread only

    fun isClosing() = io.value and CLOSING != 0

    fun markClosing() {
        while (true) {
            val v = io.value
            if (v and CLOSING != 0 || io.compareAndSet(v, v or CLOSING)) return
        }
    }

    /** True once no writer is active (the caller then owns the close). */
    fun tryRelease(): Boolean {
        if (io.value != CLOSING) return false
        released = true
        return true
    }

    override fun tryWrite(b: ByteArray, off: Int, len: Int): Int {
        while (true) {
            val v = io.value
            if (v and CLOSING != 0) return -1
            if (io.compareAndSet(v, v + 1)) break
        }
        try {
            if (len == 0) return 0
            val w = b.usePinned { p -> send(fd, p.addressOf(off), len.convert(), MSG_NOSIGNAL or MSG_DONTWAIT) }
            if (w >= 0) return w.toInt()
            val e = errno
            return if (e == EAGAIN || e == EINTR) 0 else -1
        } finally {
            io.decrementAndGet()
        }
    }

    override fun requestWritable() {
        // epoll_ctl touches the fd and rewrites the event's slot tag, so it runs under the writer count like send():
        // the loop cannot close the fd (and accept() hand its number, with a new slot, to another client) meanwhile.
        while (true) {
            val v = io.value
            if (v and CLOSING != 0) return
            if (io.compareAndSet(v, v + 1)) break
        }
        try {
            loop.armWrite(this)
        } finally {
            io.decrementAndGet()
        }
    }

    override fun execute(task: () -> Unit) = loop.post(task)

    override fun close() {
        // shutdown() touches the fd, so it runs under the same writer count as send(): the loop cannot close the fd
        // (and accept() hand its number to another client) while we are inside. If the loop already started closing,
        // there is nothing left to do.
        while (true) {
            val v = io.value
            if (v and CLOSING != 0) return
            if (io.compareAndSet(v, v + 1)) break
        }
        try {
            shutdown(fd, SHUT_RDWR) // wakes the loop (EPOLLHUP) and stops further I/O; the loop closes the fd
        } finally {
            io.decrementAndGet()
        }
        markClosing()
        loop.post { loop.finishClose(this) }
    }

    companion object {
        const val CLOSING = 1 shl 30
    }
}
