package main

import (
	"bytes"
	"context"
	"errors"
	"log"
	"net"
	"runtime"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

const (
	kindFree uint8 = iota
	kindClient
	kindListener
	kindPG
)

const (
	flagBusy uint8 = 1 << iota // a statement is in flight; later pipelined requests wait in inbuf
	flagCloseAfterWrite
	flagWantOut // EPOLLOUT registered
)

// slot is the per-descriptor state (8 pointer-free bytes).
// Because it holds no pointers, the Go garbage collector never scans the table,
// regardless of how many connections are open.
type slot struct {
	gen   uint32 // bumped on close, so a late result never reaches a reused descriptor
	loop  uint16
	kind  uint8
	flags uint8
}

var slots []slot

const (
	readBufSize    = 64 << 10
	gzipMinSize    = 1024
	maxEvents      = 1024
	lightIteration = 32
	lateResponse   = int64(25 * time.Millisecond)
)

type Worker struct {
	id          int
	epollFd     int
	authChecker *AuthChecker
	connStr     string
	started     time.Time
	base        time.Time
	now         int64 // monotonic nanoseconds since base, updated per iteration
	window      time.Duration

	events []unix.EpollEvent
	rbuf   []byte
	inbuf  map[int32][]byte
	outbuf map[int32][]byte
	resp   []byte
	errBuf []byte
	gz     deflater
	gzOut  []byte

	readers  []*pgc
	writers  []*pgc
	byFd     map[int32]*pgc
	cur      *pgc
	curW     *pgc
	curReads int
	rrRead   int
	rrWrite  int
	dirty    []*pgc
	revived  chan revived
	ndead    int
	cols     [][]byte

	// Adaptive CPU <-> Postgres Balancer state (zero-lock, per-loop)
	cpuPermil   int64 // EWMA of loop CPU utilization in [0..1000]
	ewmaEvents  int   // EWMA of ready events per pass
	dynReadCap  int   // dynamic max statements per PG read connection in a pass
	fastGzip    bool  // true when loop is under high CPU pressure (>82%): switches deflate to fast fixed Huffman
	lastPassEnd int64
}

func newWorker(id int, authChecker *AuthChecker, connStr string, nr, nw int, window time.Duration, started time.Time) (*Worker, error) {
	epfd, err := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
	if err != nil {
		return nil, err
	}
	w := &Worker{
		id:          id,
		epollFd:     epfd,
		authChecker: authChecker,
		connStr:     connStr,
		started:     started,
		base:        time.Now(),
		window:      window,
		events:      make([]unix.EpollEvent, maxEvents),
		rbuf:        make([]byte, readBufSize),
		inbuf:       make(map[int32][]byte),
		outbuf:      make(map[int32][]byte),
		resp:        make([]byte, 0, 16384),
		errBuf:      make([]byte, 0, 512),
		gzOut:       make([]byte, 0, 8192),
		byFd:        make(map[int32]*pgc, nr+nw),
		dirty:       make([]*pgc, 0, nr+nw),
		revived:     make(chan revived, 64),
		cols:        make([][]byte, 0, 8),
		ewmaEvents:  16,
		dynReadCap:  32,
	}
	for i := 0; i < nr+nw; i++ {
		write := i >= nr
		fd, err := connectPG(context.Background(), connStr)
		if err != nil {
			return nil, err
		}
		c := &pgc{
			write: write,
			out:   make([]byte, 0, 16384),
			in:    make([]byte, 0, 65536),
			body:  make([]byte, 0, 8192),
		}
		if write {
			c.idx = len(w.writers)
			w.writers = append(w.writers, c)
		} else {
			c.idx = len(w.readers)
			w.readers = append(w.readers, c)
		}
		if err := w.attachPG(c, fd); err != nil {
			return nil, err
		}
	}
	return w, nil
}

func resolveIPv4(host string) ([4]byte, error) {
	var out [4]byte
	ip := net.ParseIP(host)
	if ip == nil {
		addr, err := net.ResolveIPAddr("ip4", host)
		if err != nil {
			return out, err
		}
		ip = addr.IP
	}
	ip4 := ip.To4()
	if ip4 == nil {
		return out, errors.New("HOST must be an IPv4 address")
	}
	copy(out[:], ip4)
	return out, nil
}

func (l *Worker) listen(ip [4]byte, port int) error {
	fd, err := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		return err
	}
	_ = unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEADDR, 1)
	if err := unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEPORT, 1); err != nil {
		unix.Close(fd)
		return err
	}
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1)
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_DEFER_ACCEPT, 1)
	if err := unix.Bind(fd, &unix.SockaddrInet4{Port: port, Addr: ip}); err != nil {
		unix.Close(fd)
		return err
	}
	if err := unix.Listen(fd, 65535); err != nil {
		unix.Close(fd)
		return err
	}
	// Delayed-ACK mode, inherited by accepted sockets: a request's ACK rides on its response
	// instead of going out as a standalone packet. Set after listen(), which resets it.
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_QUICKACK, 0)
	slots[fd] = slot{loop: uint16(l.id), kind: kindListener}
	return unix.EpollCtl(l.epollFd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(fd)})
}

// Raw system calls on non-blocking descriptors: they never block, so they skip
// the Go runtime scheduler's entersyscall/exitsyscall bookkeeping.
func rawRead(fd int, p []byte) (int, unix.Errno) {
	n, _, e := unix.RawSyscall(unix.SYS_READ, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)))
	return int(n), e
}

func rawWrite(fd int, p []byte) (int, unix.Errno) {
	n, _, e := unix.RawSyscall(unix.SYS_WRITE, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)))
	return int(n), e
}

// setThinTimeouts keeps retransmission timeouts linear (one RTO each, up to 6 times)
// instead of doubling while a connection has fewer than 4 packets in flight.
func setThinTimeouts(fd int) {
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_THIN_LINEAR_TIMEOUTS, 1)
}

func (l *Worker) run() {
	runtime.LockOSThread()
	for {
		l.serve()
	}
}

func (l *Worker) serve() {
	defer func() {
		if p := recover(); p != nil {
			log.Printf("[panic] loop %d: %v", l.id, p)
		}
	}()
	baseNs := l.window.Nanoseconds()
	for {
		timeout := -1
		if l.ndead > 0 {
			timeout = 200
			l.collectRevived()
		}
		n, err := unix.EpollWait(l.epollFd, l.events, timeout)
		if err != nil {
			if err == unix.EINTR {
				continue
			}
			log.Printf("[loop %d] epoll_wait: %v", l.id, err)
			continue
		}
		passStart := int64(time.Since(l.base))
		l.now = passStart
		l.ewmaEvents = (l.ewmaEvents*7 + n) >> 3

		// Balancer Step 1: Drain Postgres sockets FIRST so ready responses go out
		// immediately (cutting p95/p99 tail latency) and c.inflight counts are exact
		// before we deal new client requests across Postgres connections.
		clientEvents := 0
		for i := 0; i < n; i++ {
			ev := &l.events[i]
			fd := int(ev.Fd)
			kind := slots[fd].kind
			if kind == kindPG {
				c := l.byFd[int32(fd)]
				if ev.Events&unix.EPOLLOUT != 0 {
					l.flushPG(c)
				}
				if ev.Events&(unix.EPOLLIN|unix.EPOLLHUP|unix.EPOLLERR) != 0 && !c.dead {
					l.readPG(c)
				}
			} else if kind == kindClient {
				clientEvents++
			}
		}

		// Balancer Step 2: Deal Slicing (max 16 reads per connection per Sync) so bursts
		// fan out across multiple PostgreSQL backends in parallel instead of queuing 64 on one core.
		if clientEvents <= 8 {
			l.dynReadCap = 8
		} else {
			l.dynReadCap = 16
		}

		// Balancer Step 3: Process client requests and listeners.
		listenerEvents := 0
		for i := 0; i < n; i++ {
			ev := &l.events[i]
			fd := int(ev.Fd)
			switch slots[fd].kind {
			case kindClient:
				if ev.Events&(unix.EPOLLIN|unix.EPOLLRDHUP|unix.EPOLLHUP|unix.EPOLLERR) != 0 {
					l.readClient(fd)
				}
				if ev.Events&unix.EPOLLOUT != 0 && slots[fd].kind == kindClient {
					l.flushClient(fd)
				}
			case kindListener:
				listenerEvents++
				l.accept(fd)
			}
		}

		l.flushBatches()

		// Balancer Step 4: Closed-loop CPU utilization meter & adaptive batch pacing.
		passEnd := int64(time.Since(l.base))
		workNs := passEnd - passStart
		if l.lastPassEnd > 0 {
			cycleNs := passEnd - l.lastPassEnd
			if cycleNs > 0 {
				instPermil := min(1000, (workNs*1000)/cycleNs)
				l.cpuPermil = (l.cpuPermil*7 + instPermil) >> 3
			}
		}
		l.lastPassEnd = passEnd

		// Activate fast Gzip valve when loop CPU exceeds 40% or batch is >= 16 events,
		// preventing the core from hitting 100% saturation from dynamic Huffman sorting.
		l.fastGzip = l.cpuPermil > 400 || n >= 16

		// Adaptive batching window: keep the full 2ms coalescing window under normal load
		// so Postgres statements batch into fewer Sync transactions, never sleep when new
		// TCP connections are arriving on the listener, and taper down smoothly when loop
		// CPU utilization exceeds 80% (disabling completely at 96%).
		if n > 0 && n < lightIteration && listenerEvents == 0 && baseNs > 0 && l.cpuPermil < 960 {
			targetNs := baseNs
			if l.cpuPermil > 800 {
				targetNs = (baseNs * (960 - l.cpuPermil)) / 160
			}
			if left := targetNs - workNs; left > 50_000 {
				ts := unix.Timespec{Nsec: left}
				_ = unix.Nanosleep(&ts, nil)
				l.lastPassEnd += left
			}
		}
	}
}

func (l *Worker) accept(lfd int) {
	for range 256 {
		nfd, _, e := unix.RawSyscall6(unix.SYS_ACCEPT4, uintptr(lfd), 0, 0, unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0, 0)
		if e != 0 {
			if e == unix.ECONNABORTED || e == unix.EINTR {
				continue
			}
			return
		}
		fd := int(nfd)
		if fd >= len(slots) {
			unix.Close(fd)
			continue
		}
		setThinTimeouts(fd)
		slots[fd] = slot{gen: slots[fd].gen, loop: uint16(l.id), kind: kindClient}
		if err := unix.EpollCtl(l.epollFd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP, Fd: int32(fd)}); err != nil {
			l.closeClient(fd)
		}
	}
}

func (l *Worker) closeClient(fd int) {
	s := &slots[fd]
	s.gen++
	s.kind, s.flags = kindFree, 0
	delete(l.inbuf, int32(fd))
	delete(l.outbuf, int32(fd))
	unix.Close(fd)
}

func (l *Worker) readClient(fd int) {
	n, e := rawRead(fd, l.rbuf)
	if e == unix.EAGAIN || e == unix.EINTR {
		return
	}
	if e != 0 || n == 0 {
		l.closeClient(fd)
		return
	}
	data := l.rbuf[:n]
	if pending, ok := l.inbuf[int32(fd)]; ok {
		data = append(pending, data...)
		l.inbuf[int32(fd)] = data
	}
	if slots[fd].flags&flagBusy != 0 {
		if _, ok := l.inbuf[int32(fd)]; !ok {
			l.inbuf[int32(fd)] = append([]byte(nil), data...)
		}
		return
	}
	l.processInput(fd, data)
}

func (l *Worker) processInput(fd int, data []byte) {
	for len(data) > 0 {
		var r ParsedRequest
		used, res := parseRequest(data, &r)
		switch res {
		case parseIncomplete:
			l.keepInput(fd, data)
			return
		case parseBad:
			l.respond(fd, 400, errBodyBadRequest, false, true)
			return
		case parseTooLarge:
			l.respond(fd, 413, errBodyTooLarge, false, true)
			return
		}
		data = data[used:]
		j := job{fd: int32(fd), gen: slots[fd].gen}
		needsDB, status, body := l.route(&r, &j)
		if !needsDB {
			l.respond(fd, status, body, r.GzipAccepted, !r.KeepAlive)
			if !r.KeepAlive || slots[fd].kind != kindClient {
				return
			}
			continue
		}
		slots[fd].flags |= flagBusy
		j.start = l.now
		l.keepInput(fd, data)
		l.dispatch(&j)
		return
	}
	delete(l.inbuf, int32(fd))
}

func (l *Worker) keepInput(fd int, data []byte) {
	if len(data) == 0 {
		delete(l.inbuf, int32(fd))
		return
	}
	l.inbuf[int32(fd)] = append([]byte(nil), data...)
}

var (
	pathHealth    = []byte("/health")
	pathFeed      = []byte("/feed")
	pathPosts     = []byte("/posts")
	prefixPosts   = []byte("/posts/")
	suffixLike    = []byte("/like")
)

func (l *Worker) route(r *ParsedRequest, j *job) (needsDB bool, status int, out []byte) {
	j.gzip, j.close = r.GzipAccepted, !r.KeepAlive
	isGet := bytes.Equal(r.Method, methodGET)
	isPost := !isGet && bytes.Equal(r.Method, methodPOST)
	if !isGet && !isPost {
		return false, 404, errBodyNotFound
	}
	path := r.Path

	if bytes.Equal(path, pathFeed) {
		if !isGet {
			return false, 404, errBodyNotFound
		}
		j.stmt = stmtFeed
		return true, 0, nil
	}

	if bytes.HasPrefix(path, prefixPosts) {
		sub := path[len(prefixPosts):]
		if bytes.HasSuffix(sub, suffixLike) {
			if !isPost {
				return false, 404, errBodyNotFound
			}
			idBytes := sub[:len(sub)-len(suffixLike)]
			if len(idBytes) == 0 || bytes.IndexByte(idBytes, '/') >= 0 {
				return false, 404, errBodyNotFound
			}
			authRes := l.authChecker.Check(r.AuthHeader)
			if authRes.Status != 0 {
				l.errBuf = formatErrorJSON(l.errBuf[:0], authRes.ErrorMsg)
				return false, authRes.Status, l.errBuf
			}
			postID, ok := parseIDBytes(idBytes)
			if !ok {
				return false, 400, errBodyInvalidID
			}
			j.stmt, j.a, j.b = stmtLike, authRes.UserID, postID
			return true, 0, nil
		}
		if len(sub) == 0 || bytes.IndexByte(sub, '/') >= 0 {
			return false, 404, errBodyNotFound
		}
		if !isGet {
			return false, 404, errBodyNotFound
		}
		postID, ok := parseIDBytes(sub)
		if !ok {
			return false, 400, errBodyInvalidID
		}
		j.stmt, j.a = stmtPost, postID
		return true, 0, nil
	}

	if bytes.Equal(path, pathPosts) {
		if !isPost {
			return false, 404, errBodyNotFound
		}
		authRes := l.authChecker.Check(r.AuthHeader)
		if authRes.Status != 0 {
			l.errBuf = formatErrorJSON(l.errBuf[:0], authRes.ErrorMsg)
			return false, authRes.Status, l.errBuf
		}
		body, code, msg := parsePostBody(r.Body)
		if code != 0 {
			l.errBuf = formatErrorJSON(l.errBuf[:0], msg)
			return false, code, l.errBuf
		}
		j.stmt, j.a, j.body, j.username = stmtInsertPost, authRes.UserID, body, authRes.Username
		return true, 0, nil
	}

	if bytes.Equal(path, pathHealth) {
		if !isGet {
			return false, 404, errBodyNotFound
		}
		j.stmt = stmtHealth
		return true, 0, nil
	}

	return false, 404, errBodyNotFound
}

func (l *Worker) respond(fd int, status int, body []byte, acceptGzip, closeConn bool) {
	gzipped := false
	if acceptGzip && len(body) >= gzipMinSize {
		l.gzOut = l.gz.appendGzip(l.gzOut[:0], body, l.fastGzip)
		body, gzipped = l.gzOut, true
	}
	l.resp = appendHTTPHead(l.resp[:0], status, len(body), gzipped, closeConn)
	l.resp = append(l.resp, body...)
	l.send(fd, l.resp, closeConn)
}

func (l *Worker) send(fd int, p []byte, closeConn bool) {
	s := &slots[fd]
	if pending, ok := l.outbuf[int32(fd)]; ok {
		l.outbuf[int32(fd)] = append(pending, p...)
		if closeConn {
			s.flags |= flagCloseAfterWrite
		}
		return
	}
	for len(p) > 0 {
		n, e := rawWrite(fd, p)
		if e == unix.EINTR {
			continue
		}
		if e == unix.EAGAIN {
			break
		}
		if e != 0 {
			l.closeClient(fd)
			return
		}
		p = p[n:]
	}
	if len(p) == 0 {
		if closeConn {
			l.closeClient(fd)
		}
		return
	}
	l.outbuf[int32(fd)] = append([]byte(nil), p...)
	if closeConn {
		s.flags |= flagCloseAfterWrite
	}
	s.flags |= flagWantOut
	_ = unix.EpollCtl(l.epollFd, unix.EPOLL_CTL_MOD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP | unix.EPOLLOUT, Fd: int32(fd)})
}

func (l *Worker) flushClient(fd int) {
	p := l.outbuf[int32(fd)]
	for len(p) > 0 {
		n, e := rawWrite(fd, p)
		if e == unix.EINTR {
			continue
		}
		if e == unix.EAGAIN {
			l.outbuf[int32(fd)] = p
			return
		}
		if e != 0 {
			l.closeClient(fd)
			return
		}
		p = p[n:]
	}
	delete(l.outbuf, int32(fd))
	s := &slots[fd]
	if s.flags&flagCloseAfterWrite != 0 {
		l.closeClient(fd)
		return
	}
	s.flags &^= flagWantOut
	_ = unix.EpollCtl(l.epollFd, unix.EPOLL_CTL_MOD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP, Fd: int32(fd)})
}
