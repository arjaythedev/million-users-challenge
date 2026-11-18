package main

import (
	"errors"
	"log"
	"net"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

// The server is one event loop per core. A loop is a thread that owns
// everything it touches: its listening sockets (SO_REUSEPORT gives each loop
// its own accept queue), the client connections it accepted, and its own
// PostgreSQL sessions. A request is read, parsed, queued for the database,
// answered and written without leaving that thread, so there are no locks,
// channels or scheduler hand-offs between a request and its response.

const (
	idleTimeout    = 180 * time.Second
	readTimeout    = 15 * time.Second
	maxHeaderBytes = 16 << 10
	maxBufferBytes = 36 << 10

	eventBatch  = 1024
	readChunk   = 64 << 10
	acceptBurst = 256
)

// Connection state lives in a table indexed by file descriptor instead of one
// heap object per connection: a million idle connections are a few dozen MiB
// of pointer-free memory that the garbage collector never scans.
type conn struct {
	// gen counts the connections this descriptor number has carried. Queued
	// work and deadlines name a connection by descriptor and generation, so
	// they cannot touch a later connection that reuses the number.
	gen   uint32
	flags uint16
	owner uint16
	// slot is the wheel second the connection is filed under, or 0.
	slot     int64
	deadline int64
}

const (
	flagOpen uint16 = 1 << iota
	// flagBusy: a request is being answered; the next is not read or parsed.
	flagBusy
	// flagReadable: the socket signalled input while the connection was busy.
	flagReadable
	// flagReading: part of a request has arrived and the read deadline runs.
	flagReading
	flagContinued
	flagGzip
	flagClose
	// flagBuffered: unparsed input is held in loop.partial.
	flagBuffered
	// flagFlushing: response bytes the socket did not take are in loop.unsent.
	flagFlushing
)

const (
	chunkBits      = 12
	maxDescriptors = 8 << 20
)

type connChunk [1 << chunkBits]conn

var connTable [maxDescriptors >> chunkBits]atomic.Pointer[connChunk]

// connAt returns the state of an accepted descriptor.
func connAt(fd int32) *conn {
	return &connTable[fd>>chunkBits].Load()[fd&(1<<chunkBits-1)]
}

func connSlot(fd int32) *conn {
	entry := &connTable[fd>>chunkBits]
	chunk := entry.Load()
	if chunk == nil {
		chunk = new(connChunk)
		if !entry.CompareAndSwap(nil, chunk) {
			chunk = entry.Load()
		}
	}
	return &chunk[fd&(1<<chunkBits-1)]
}

// What an epoll event refers to, kept in the event's user data beside the
// descriptor: the kind in the top bits, and below it the connection
// generation (clients) or the session index (database).
const (
	kindClient = iota
	kindListener
	kindDatabase
	kindWake
	kindShift = 28
	kindMask  = 1<<kindShift - 1
)

type loop struct {
	id     int
	app    *application
	epfd   int
	wakeFD int
	events []unix.EpollEvent
	// now is the monotonic time of the current pass.
	now int64

	listeners []int
	open      int
	// serving is the descriptor whose input is being parsed, or -1.
	serving int32

	// Scratch reused by every request of the loop.
	req     request
	input   []byte
	body    []byte
	packed  []byte
	wire    []byte
	partial map[int32][]byte
	unsent  map[int32][]byte
	wheel   wheel

	sessions []*session
	reads    jobQueue
	writes   jobQueue
	// When the loop last started a read batch and a write batch.
	readsSent, writesSent int64

	// LOOP_TRACE measurements since the last report.
	traced         int64
	slowPass       int64
	slowPassEvents int
	slowTrip       int64

	inboxLock sync.Mutex
	inbox     []dialResult
	stopping  bool
	stopAt    int64
}

var clockBase = time.Now()

// monotonic returns nanoseconds on the monotonic clock.
func monotonic() int64 { return int64(time.Since(clockBase)) }

func rawRead(fd int, p []byte) (int, unix.Errno) {
	n, _, errno := unix.RawSyscall(unix.SYS_READ, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)))
	if errno != 0 {
		return -1, errno
	}
	return int(n), 0
}

func rawWrite(fd int, p []byte) (int, unix.Errno) {
	n, _, errno := unix.RawSyscall(unix.SYS_WRITE, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)))
	if errno != 0 {
		return -1, errno
	}
	return int(n), 0
}

// recv and send are read and write for sockets. They go straight to the
// socket layer and skip the generic file checks that read and write pay on
// every call.
func recv(fd int, p []byte) (int, unix.Errno) {
	n, _, errno := unix.RawSyscall6(unix.SYS_RECVFROM, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)), 0, 0, 0)
	if errno != 0 {
		return -1, errno
	}
	return int(n), 0
}

func send(fd int, p []byte) (int, unix.Errno) {
	n, _, errno := unix.RawSyscall6(unix.SYS_SENDTO, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)), unix.MSG_NOSIGNAL, 0, 0)
	if errno != 0 {
		return -1, errno
	}
	return int(n), 0
}

func newLoop(id int, app *application) (*loop, error) {
	epfd, err := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
	if err != nil {
		return nil, err
	}
	wakeFD, err := unix.Eventfd(0, unix.EFD_NONBLOCK|unix.EFD_CLOEXEC)
	if err != nil {
		return nil, err
	}
	l := &loop{
		id:      id,
		app:     app,
		epfd:    epfd,
		wakeFD:  wakeFD,
		events:  make([]unix.EpollEvent, eventBatch),
		serving: -1,
		input:   make([]byte, readChunk),
		body:    make([]byte, 0, 16<<10),
		packed:  make([]byte, 0, 16<<10),
		wire:    make([]byte, 0, 16<<10),
		partial: make(map[int32][]byte),
		unsent:  make(map[int32][]byte),
	}
	l.wheel.next = deadlineSecond(monotonic())
	if err := l.watch(wakeFD, unix.EPOLLIN, kindWake<<kindShift); err != nil {
		return nil, err
	}
	return l, nil
}

func (l *loop) watch(fd int, events uint32, data int32) error {
	return unix.EpollCtl(l.epfd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: events, Fd: int32(fd), Pad: data})
}

// listen opens this loop's own listening socket for address. Accepted sockets
// inherit TCP_NODELAY from it.
func (l *loop) listen(host, port string) error {
	address, err := net.ResolveTCPAddr("tcp", net.JoinHostPort(host, port))
	if err != nil {
		return err
	}
	family, sockaddr := unix.AF_INET, unix.Sockaddr(&unix.SockaddrInet4{Port: address.Port})
	if ip4 := address.IP.To4(); ip4 != nil {
		copy(sockaddr.(*unix.SockaddrInet4).Addr[:], ip4)
	} else if len(address.IP) != 0 {
		v6 := &unix.SockaddrInet6{Port: address.Port}
		copy(v6.Addr[:], address.IP.To16())
		if address.Zone != "" {
			if zone, err := net.InterfaceByName(address.Zone); err == nil {
				v6.ZoneId = uint32(zone.Index)
			}
		}
		family, sockaddr = unix.AF_INET6, v6
	}
	fd, err := unix.Socket(family, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, unix.IPPROTO_TCP)
	if err != nil {
		return err
	}
	for _, option := range [][2]int{{unix.SOL_SOCKET, unix.SO_REUSEADDR}, {unix.SOL_SOCKET, unix.SO_REUSEPORT}, {unix.IPPROTO_TCP, unix.TCP_NODELAY}} {
		if err := unix.SetsockoptInt(fd, option[0], option[1], 1); err != nil {
			_ = unix.Close(fd)
			return err
		}
	}
	if err := unix.Bind(fd, sockaddr); err != nil {
		_ = unix.Close(fd)
		return err
	}
	if err := unix.Listen(fd, 65535); err != nil {
		_ = unix.Close(fd)
		return err
	}
	if err := l.watch(fd, unix.EPOLLIN, kindListener<<kindShift); err != nil {
		_ = unix.Close(fd)
		return err
	}
	l.listeners = append(l.listeners, fd)
	return nil
}

// wake interrupts the loop's wait from another thread.
func (l *loop) wake() {
	one := [8]byte{1}
	_, _ = rawWrite(l.wakeFD, one[:])
}

func (l *loop) run(done *sync.WaitGroup) {
	defer done.Done()
	runtime.LockOSThread()
	l.now = monotonic()
	for {
		n, err := unix.EpollWait(l.epfd, l.events, l.housekeeping())
		if err != nil && err != unix.EINTR {
			panic("epoll_wait: " + err.Error())
		}
		l.now = monotonic()
		for i := 0; i < n; i++ {
			event := &l.events[i]
			data := uint32(event.Pad)
			switch data >> kindShift {
			case kindClient:
				l.clientEvent(event.Fd, data&kindMask, event.Events)
			case kindDatabase:
				l.sessions[data&kindMask].event(int(event.Fd), event.Events)
			case kindListener:
				l.accept(int(event.Fd))
			case kindWake:
				l.wakeEvent()
			}
		}
		// One pass over the queues per pass over the events: the requests
		// that arrived together leave for the database together.
		l.pump()
		if l.app.trace {
			l.tracePass(n)
		}
		if l.stopping && (l.idle() || l.now >= l.stopAt) {
			return
		}
	}
}

// housekeeping runs the timers that are due and returns how long the loop may
// sleep, in milliseconds.
func (l *loop) housekeeping() int {
	if l.now >= l.wheel.next*int64(time.Second) {
		l.sweep()
	}
	wait := l.wheel.next*int64(time.Second) - l.now
	if next := l.databaseTimers(); next < wait {
		wait = next
	}
	if l.stopping && l.stopAt-l.now < wait {
		wait = l.stopAt - l.now
	}
	// Round up: waking early would only find the timer not yet due.
	milliseconds := int((wait + int64(time.Millisecond) - 1) / int64(time.Millisecond))
	if milliseconds < 1 {
		return 1
	}
	return milliseconds
}

// tracePass (LOOP_TRACE=1) logs, once a second, a loop's slowest pass over
// its events and the slowest database round trip, when either is long enough
// to explain a latency spike.
func (l *loop) tracePass(events int) {
	if took := monotonic() - l.now; took > l.slowPass {
		l.slowPass, l.slowPassEvents = took, events
	}
	if l.now-l.traced < int64(time.Second) {
		return
	}
	if l.slowPass > int64(20*time.Millisecond) || l.slowTrip > int64(50*time.Millisecond) {
		log.Printf("loop %d: slowest pass %v (%d events), slowest database round trip %v, queued reads %d writes %d",
			l.id, time.Duration(l.slowPass), l.slowPassEvents, time.Duration(l.slowTrip), l.reads.len(), l.writes.len())
	}
	l.traced, l.slowPass, l.slowTrip = l.now, 0, 0
}

func (l *loop) wakeEvent() {
	var count [8]byte
	_, _ = rawRead(l.wakeFD, count[:])
	l.inboxLock.Lock()
	results := l.inbox
	l.inbox = nil
	l.inboxLock.Unlock()
	for _, result := range results {
		l.sessions[result.index].dialed(result)
	}
	if l.app.stopping.Load() && !l.stopping {
		l.stopping, l.stopAt = true, l.now+int64(databaseTimeout)
		for _, fd := range l.listeners {
			_ = unix.Close(fd)
		}
		l.listeners = nil
	}
}

func (l *loop) accept(listener int) {
	for i := 0; i < acceptBurst; i++ {
		r, _, errno := unix.RawSyscall6(unix.SYS_ACCEPT4, uintptr(listener), 0, 0, unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0, 0)
		if errno != 0 {
			if errno == unix.EINTR || errno == unix.ECONNABORTED {
				continue
			}
			return
		}
		l.adopt(int32(r))
	}
}

// adopt takes ownership of a connected, non-blocking client socket.
func (l *loop) adopt(fd int32) {
	if fd >= maxDescriptors {
		_ = unix.Close(int(fd))
		return
	}
	c := connSlot(fd)
	c.gen++
	c.flags, c.owner, c.slot = flagOpen, uint16(l.id), 0
	c.deadline = l.now + int64(idleTimeout)
	// Edge-triggered: a busy connection is not read, and its pending input
	// must not wake the loop again on every pass.
	if l.watch(int(fd), unix.EPOLLIN|unix.EPOLLRDHUP|unix.EPOLLET, int32(kindClient<<kindShift|c.gen&kindMask)) != nil {
		c.flags = 0
		_ = unix.Close(int(fd))
		return
	}
	l.open++
	l.schedule(fd, c, c.deadline)
}

func (l *loop) close(fd int32, c *conn) {
	if c.flags&flagBuffered != 0 {
		delete(l.partial, fd)
	}
	if c.flags&flagFlushing != 0 {
		delete(l.unsent, fd)
	}
	c.flags = 0
	l.open--
	// Closing the descriptor also removes it from the epoll set.
	_ = unix.Close(int(fd))
}

func (l *loop) clientEvent(fd int32, gen, events uint32) {
	c := connAt(fd)
	if c.flags&flagOpen == 0 || c.gen&kindMask != gen || int(c.owner) != l.id {
		return
	}
	if c.flags&flagFlushing != 0 {
		if events&(unix.EPOLLOUT|unix.EPOLLERR|unix.EPOLLHUP) != 0 {
			l.flush(fd, c)
		} else {
			c.flags |= flagReadable
		}
		return
	}
	if c.flags&flagBusy != 0 {
		c.flags |= flagReadable
		return
	}
	l.read(fd, c)
}

// read takes what the socket has and serves the requests in it.
func (l *loop) read(fd int32, c *conn) {
	for {
		n, errno := recv(int(fd), l.input)
		if n > 0 {
			data := l.input[:n]
			if c.flags&flagBuffered != 0 {
				data = append(l.partial[fd], data...)
				l.partial[fd] = data
			}
			l.serve(fd, c, data)
			if c.flags&flagOpen == 0 || n < len(l.input) {
				return
			}
			if c.flags&flagBusy != 0 {
				c.flags |= flagReadable
				return
			}
			continue
		}
		if n < 0 && errno == unix.EINTR {
			continue
		}
		if n < 0 && errno == unix.EAGAIN {
			return
		}
		l.close(fd, c)
		return
	}
}

var malformedResponse = []byte("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n{\"error\":\"malformed JSON body\"}")

// serve parses data, all of the connection's unparsed input, and starts each
// complete request. It stops at the first request that has to wait for the
// database and keeps what is left for when that one has been answered.
func (l *loop) serve(fd int32, c *conn, data []byte) {
	outer := l.serving
	l.serving = fd
	for len(data) > 0 && c.flags&(flagOpen|flagBusy) == flagOpen {
		req := &l.req
		used, status := parseInto(req, data)
		if status == parseFallback {
			used, status = l.parseStandard(fd, c, req, data)
		}
		if status == parseBad || status == parseIncomplete && len(data) > maxBufferBytes {
			_, _ = send(int(fd), malformedResponse)
			l.close(fd, c)
			break
		}
		if status == parseIncomplete {
			if c.flags&flagReading == 0 {
				c.flags |= flagReading
				c.deadline = l.now + int64(readTimeout)
				l.schedule(fd, c, c.deadline)
			}
			break
		}
		c.flags = c.flags&^(flagReading|flagContinued|flagGzip|flagClose) | flagBusy
		if req.gzip {
			c.flags |= flagGzip
		}
		if req.close {
			c.flags |= flagClose
		}
		c.deadline = l.now + int64(idleTimeout)
		l.handle(fd, c, req)
		data = data[used:]
	}
	l.serving = outer
	if c.flags&flagOpen == 0 {
		return
	}
	// Keep the unparsed rest: a partial request, or requests sent ahead.
	switch {
	case len(data) > 0:
		l.partial[fd] = append(l.partial[fd][:0], data...)
		c.flags |= flagBuffered
	case c.flags&flagBuffered != 0:
		delete(l.partial, fd)
		c.flags &^= flagBuffered
	}
}

// handle routes a parsed request. Anything that needs the database is queued;
// the rest is answered at once. req and its body are only valid during the
// call.
func (l *loop) handle(fd int32, c *conn, req *request) {
	switch req.route {
	case routeHealth:
		l.reads.push(job{fd: fd, gen: c.gen, kind: jobHealth})
	case routeFeed:
		l.reads.push(job{fd: fd, gen: c.gen, kind: jobFeed})
	case routeGetPost:
		if !req.idOK {
			l.respond(fd, c, 400, bodyInvalidID)
			return
		}
		l.reads.push(job{fd: fd, gen: c.gen, kind: jobPost, id: req.id})
	case routeCreate:
		user, status, message := l.app.authenticate(req)
		if status != 0 {
			l.respond(fd, c, status, message)
			return
		}
		text, message := decodeBody(req.body)
		if message != nil {
			l.respond(fd, c, 400, message)
			return
		}
		l.writes.push(job{fd: fd, gen: c.gen, kind: jobCreate, user: user.id, name: user.username, text: text})
	case routeLike:
		user, status, message := l.app.authenticate(req)
		if status != 0 {
			l.respond(fd, c, status, message)
			return
		}
		if !req.idOK {
			l.respond(fd, c, 400, bodyInvalidID)
			return
		}
		l.writes.push(job{fd: fd, gen: c.gen, kind: jobLike, user: user.id, id: req.id})
	default:
		l.respond(fd, c, 404, bodyNotFound)
	}
}

// complete answers the connection a job came from, unless it has gone.
func (l *loop) complete(j *job, status int, body []byte) {
	c := connAt(j.fd)
	if c.flags&flagOpen == 0 || c.gen != j.gen || int(c.owner) != l.id {
		return
	}
	l.respond(j.fd, c, status, body)
}

// respond encodes and writes a response in one system call.
func (l *loop) respond(fd int32, c *conn, status int, body []byte) {
	closeAfter := c.flags&flagClose != 0
	if c.flags&flagGzip != 0 && len(body) >= gzipMinBytes {
		l.packed = gzipAppend(l.packed[:0], body)
		l.wire = appendResponse(l.wire[:0], status, l.packed, true, closeAfter)
	} else {
		l.wire = appendResponse(l.wire[:0], status, body, false, closeAfter)
	}
	n, errno := send(int(fd), l.wire)
	if n == len(l.wire) {
		l.sent(fd, c)
		return
	}
	if n < 0 {
		if errno != unix.EAGAIN && errno != unix.EINTR {
			l.close(fd, c)
			return
		}
		n = 0
	}
	// The socket buffer is full: keep the rest and wait until it drains.
	l.unsent[fd] = append([]byte(nil), l.wire[n:]...)
	c.flags |= flagFlushing
	l.rearm(fd, c, unix.EPOLLOUT)
}

func (l *loop) rearm(fd int32, c *conn, extra uint32) {
	event := unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP | unix.EPOLLET | extra, Fd: fd, Pad: int32(kindClient<<kindShift | c.gen&kindMask)}
	_ = unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, int(fd), &event)
}

func (l *loop) flush(fd int32, c *conn) {
	rest := l.unsent[fd]
	for len(rest) > 0 {
		n, errno := send(int(fd), rest)
		if n < 0 {
			if errno == unix.EINTR {
				continue
			}
			if errno == unix.EAGAIN {
				l.unsent[fd] = rest
				return
			}
			l.close(fd, c)
			return
		}
		rest = rest[n:]
	}
	delete(l.unsent, fd)
	c.flags &^= flagFlushing
	l.rearm(fd, c, 0)
	l.sent(fd, c)
}

// sent runs once a response is fully written: the connection closes or takes
// its next request.
func (l *loop) sent(fd int32, c *conn) {
	if c.flags&flagClose != 0 {
		l.close(fd, c)
		return
	}
	c.flags &^= flagBusy
	c.deadline = l.now + int64(idleTimeout)
	if l.serving == fd {
		// serve is parsing this connection's input and carries on itself.
		return
	}
	if c.flags&flagBuffered != 0 {
		l.serve(fd, c, l.partial[fd])
	}
	if c.flags&(flagOpen|flagBusy|flagReadable) == flagOpen|flagReadable {
		c.flags &^= flagReadable
		l.read(fd, c)
	}
}

// idle reports whether nothing is waiting on the database.
func (l *loop) idle() bool {
	if l.reads.len() > 0 || l.writes.len() > 0 {
		return false
	}
	for _, s := range l.sessions {
		if s.busy {
			return false
		}
	}
	return true
}

// Connection deadlines. An idle connection costs nothing until its deadline:
// it is filed under the second the deadline falls in, and the loop sweeps a
// second's slot once that second has passed. Requests only store a later
// deadline in the connection; the sweep files it again under the new one, so
// an active connection is examined about once per idle timeout. Only a
// deadline that moves earlier (a partial request starting the read timeout)
// files the connection immediately.

// wheelSlots is the wheel's span in seconds. It must exceed the longest
// deadline (idleTimeout) so that every scheduled second maps to its own slot.
const wheelSlots = 512

type wheelEntry struct {
	fd  int32
	gen uint32
}

type wheel struct {
	slots [wheelSlots][]wheelEntry
	spare []wheelEntry
	// next is the first second whose slot has not been swept.
	next int64
}

// deadlineSecond is the first whole second at which a deadline has passed.
func deadlineSecond(deadline int64) int64 { return deadline/int64(time.Second) + 1 }

func (l *loop) schedule(fd int32, c *conn, deadline int64) {
	second := deadlineSecond(deadline)
	if c.slot > 0 && c.slot <= second {
		return
	}
	if second < l.wheel.next {
		second = l.wheel.next
	}
	slot := &l.wheel.slots[second%wheelSlots]
	*slot = append(*slot, wheelEntry{fd, c.gen})
	c.slot = second
}

func (l *loop) sweep() {
	for second := l.wheel.next; second <= l.now/int64(time.Second); second++ {
		slot := &l.wheel.slots[second%wheelSlots]
		due := *slot
		*slot = l.wheel.spare[:0]
		l.wheel.next = second + 1
		for _, entry := range due {
			c := connAt(entry.fd)
			// A connection that closed, or was filed again under an earlier
			// second, no longer belongs to this slot.
			if c.flags&flagOpen == 0 || c.gen != entry.gen || int(c.owner) != l.id || c.slot != second {
				continue
			}
			c.slot = 0
			if c.deadline <= l.now {
				l.close(entry.fd, c)
				continue
			}
			l.schedule(entry.fd, c, c.deadline)
		}
		l.wheel.spare = due[:0]
	}
}

// serve starts the loops and blocks until the process is told to stop and the
// requests in flight have been answered.
func serve(app *application, host string, ports []string, loops int) error {
	all := make([]*loop, loops)
	for i := range all {
		l, err := newLoop(i, app)
		if err != nil {
			return err
		}
		l.sessions = app.plan.sessions(l)
		for _, port := range ports {
			if err := l.listen(host, port); err != nil {
				return errors.New("listen on " + net.JoinHostPort(host, port) + ": " + err.Error())
			}
		}
		all[i] = l
	}
	var running sync.WaitGroup
	for _, l := range all {
		running.Add(1)
		go l.run(&running)
	}
	<-app.stop
	app.stopping.Store(true)
	for _, l := range all {
		l.wake()
	}
	running.Wait()
	return nil
}

func statusLine(status int) string {
	switch status {
	case 200:
		return "HTTP/1.1 200 OK\r\n"
	case 201:
		return "HTTP/1.1 201 Created\r\n"
	case 400:
		return "HTTP/1.1 400 Bad Request\r\n"
	case 401:
		return "HTTP/1.1 401 Unauthorized\r\n"
	case 404:
		return "HTTP/1.1 404 Not Found\r\n"
	case 500:
		return "HTTP/1.1 500 Internal Server Error\r\n"
	case 503:
		return "HTTP/1.1 503 Service Unavailable\r\n"
	}
	return "HTTP/1.1 " + strconv.Itoa(status) + " Status\r\n"
}

func appendResponse(wire []byte, status int, body []byte, gzipped, closeAfter bool) []byte {
	wire = append(wire, statusLine(status)...)
	wire = append(wire, "Content-Type: application/json\r\n"...)
	if gzipped {
		wire = append(wire, "Content-Encoding: gzip\r\n"...)
	}
	if closeAfter {
		wire = append(wire, "Connection: close\r\n"...)
	}
	wire = append(wire, "Content-Length: "...)
	wire = strconv.AppendInt(wire, int64(len(body)), 10)
	wire = append(wire, "\r\n\r\n"...)
	return append(wire, body...)
}
