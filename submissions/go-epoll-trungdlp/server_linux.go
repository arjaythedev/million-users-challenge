//go:build linux

package main

import (
	"context"
	"encoding/binary"
	"errors"
	"log"
	"net"
	"os"
	"runtime"
	"strconv"
	"syscall"
	"time"
	"unsafe"

	"github.com/jackc/pgx/v5/pgconn"
	"golang.org/x/sys/unix"
)

const (
	kindFree = iota
	kindClient
	kindListener
	kindPG
)

const (
	flagBusy = 1 << iota // a statement is in flight; later requests wait in inbuf
	flagCloseAfterWrite
	flagWantOut // EPOLLOUT registered
)

// slot is the per-descriptor state. It holds no pointers, so the garbage collector never
// scans the table, however many connections are open.
type slot struct {
	gen   uint32 // bumped on close, so a late result never reaches a reused descriptor
	loop  uint16
	kind  uint8
	flags uint8
}

var slots []slot

// debugHook, when set (debug builds), runs after every loop iteration.
var debugHook func(l *loop)

const (
	readBufSize = 64 << 10
	gzipMinSize = 1024
	maxEvents   = 1024
	// After an iteration with fewer events than this, a loop pauses for batchWindow:
	// the next iteration then serves everything that arrived meanwhile, and the reads
	// among it share one Postgres round trip.
	lightIteration = 32
	batchWindow    = 2 * time.Millisecond
	// Per loop: 64 connections on 16 cores. More connections per loop measured worse:
	// smaller batches cost Postgres and the server more CPU per request.
	pgReadersPerLoop = 3
	pgWritersPerLoop = 1
)

type pgItem struct {
	sync bool
	j    job
}

// pgc is a Postgres connection owned by one loop after pgconn connected it.
type pgc struct {
	fd       int
	idx      int
	write    bool
	dead     bool
	inBatch  bool // reads queued without their Sync yet
	dirty    bool
	wantOut  bool
	out      []byte
	in       []byte
	q        []pgItem
	qhead    int
	inflight int
	rows     int
	body     []byte // the response being built for the job at the head of q
}

type loop struct {
	id       int
	epfd     int
	a        *app
	window   time.Duration
	windowTS unix.Timespec
	events   []unix.EpollEvent
	rbuf     []byte
	inbuf    map[int32][]byte
	outbuf   map[int32][]byte
	resp     []byte
	gz       deflater
	gzOut    []byte
	readers  []*pgc
	writers  []*pgc
	byFd     map[int32]*pgc
	cur      *pgc
	dirty    []*pgc
	revived  chan revived
	ndead    int
	cols     [][]byte
	connStr  string
}

type revived struct {
	fd    int
	write bool
	idx   int
}

func main() {
	if os.Getenv("DATABASE_URL") == "" || os.Getenv("JWT_SECRET") == "" {
		log.Fatal("DATABASE_URL and JWT_SECRET are required")
	}
	host := envOr("HOST", "127.0.0.1")
	ports := []string{envOr("PORT", "3000")}
	if extra := os.Getenv("EXTRA_PORT"); extra != "" {
		ports = append(ports, extra)
	}
	ip, err := resolveIPv4(host)
	if err != nil {
		log.Fatal(err)
	}
	var lim unix.Rlimit
	if err := unix.Getrlimit(unix.RLIMIT_NOFILE, &lim); err == nil && lim.Cur < lim.Max {
		lim.Cur = lim.Max
		_ = unix.Setrlimit(unix.RLIMIT_NOFILE, &lim)
	}
	_ = unix.Getrlimit(unix.RLIMIT_NOFILE, &lim)
	// The table's pages are only touched where descriptors are, so this costs real memory
	// only for open connections.
	slots = make([]slot, min(lim.Cur, 1<<24))

	a := newApp([]byte(os.Getenv("JWT_SECRET")))
	nloops := runtime.GOMAXPROCS(0)
	readersPerLoop, writersPerLoop, window := pgReadersPerLoop, pgWritersPerLoop, batchWindow
	loops := make([]*loop, nloops)
	for i := range loops {
		l, err := newLoop(i, a, os.Getenv("DATABASE_URL"), readersPerLoop, writersPerLoop, window)
		if err != nil {
			log.Fatal(err)
		}
		for _, port := range ports {
			p, err := strconv.Atoi(port)
			if err != nil {
				log.Fatalf("bad port %q", port)
			}
			if err := l.listen(ip, p); err != nil {
				log.Fatal(err)
			}
		}
		loops[i] = l
	}
	for _, l := range loops {
		go l.run()
	}
	log.Printf("listening on %s:%v, %d loops, %d+%d postgres connections per loop",
		host, ports, nloops, readersPerLoop, writersPerLoop)
	select {}
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

func newLoop(id int, a *app, connStr string, nr, nw int, window time.Duration) (*loop, error) {
	epfd, err := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
	if err != nil {
		return nil, err
	}
	l := &loop{
		id: id, epfd: epfd, a: a, window: window, windowTS: unix.NsecToTimespec(window.Nanoseconds()),
		events: make([]unix.EpollEvent, maxEvents), rbuf: make([]byte, readBufSize),
		inbuf: map[int32][]byte{}, outbuf: map[int32][]byte{},
		byFd: map[int32]*pgc{}, revived: make(chan revived, 64), connStr: connStr,
		cols: make([][]byte, 0, 8),
	}
	for i := range nr + nw {
		write := i >= nr
		fd, err := connectPG(context.Background(), connStr)
		if err != nil {
			return nil, err
		}
		c := &pgc{write: write}
		if write {
			c.idx = len(l.writers)
			l.writers = append(l.writers, c)
		} else {
			c.idx = len(l.readers)
			l.readers = append(l.readers, c)
		}
		if err := l.attachPG(c, fd); err != nil {
			return nil, err
		}
	}
	return l, nil
}

// connectPG connects, authenticates and prepares the statements with pgconn, then takes
// the socket over: the loop speaks the extended protocol on it directly.
func connectPG(ctx context.Context, connStr string) (int, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	conn, err := pgconn.Connect(ctx, connStr)
	if err != nil {
		return -1, err
	}
	for i, sql := range stmtSQL {
		if _, err := conn.Prepare(ctx, "s"+string(rune('0'+i)), sql, nil); err != nil {
			conn.Close(context.Background())
			return -1, err
		}
	}
	if err := conn.SyncConn(ctx); err != nil {
		conn.Close(context.Background())
		return -1, err
	}
	hc, err := conn.Hijack()
	if err != nil {
		return -1, err
	}
	defer hc.Conn.Close()
	if hc.TLSConfig != nil {
		return -1, errors.New("postgres connection uses TLS; use sslmode=disable")
	}
	sc, ok := hc.Conn.(syscall.Conn)
	if !ok {
		return -1, errors.New("postgres connection has no descriptor")
	}
	raw, err := sc.SyscallConn()
	if err != nil {
		return -1, err
	}
	fd := -1
	var dupErr error
	if err := raw.Control(func(f uintptr) { fd, dupErr = unix.Dup(int(f)) }); err != nil {
		return -1, err
	}
	if dupErr != nil {
		return -1, dupErr
	}
	unix.CloseOnExec(fd)
	if err := unix.SetNonblock(fd, true); err != nil {
		unix.Close(fd)
		return -1, err
	}
	return fd, nil
}

func (l *loop) attachPG(c *pgc, fd int) error {
	if fd >= len(slots) {
		unix.Close(fd)
		return errors.New("descriptor table full")
	}
	c.fd, c.dead = fd, false
	c.out, c.in, c.q, c.qhead, c.inflight = c.out[:0], c.in[:0], c.q[:0], 0, 0
	c.inBatch, c.dirty, c.wantOut = false, false, false
	slots[fd] = slot{gen: slots[fd].gen, loop: uint16(l.id), kind: kindPG}
	l.byFd[int32(fd)] = c
	return unix.EpollCtl(l.epfd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(fd)})
}

func (l *loop) listen(ip [4]byte, port int) error {
	fd, err := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		return err
	}
	_ = unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEADDR, 1)
	if err := unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEPORT, 1); err != nil {
		return err
	}
	if err := unix.Bind(fd, &unix.SockaddrInet4{Port: port, Addr: ip}); err != nil {
		return err
	}
	if err := unix.Listen(fd, 65535); err != nil {
		return err
	}
	slots[fd] = slot{loop: uint16(l.id), kind: kindListener}
	return unix.EpollCtl(l.epfd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(fd)})
}

// run never returns. The service is not restarted if it crashes, so a panic while
// handling events is logged and the loop carries on with the next iteration.
func (l *loop) run() {
	runtime.LockOSThread()
	for {
		l.serve()
	}
}

func (l *loop) serve() {
	defer func() {
		if p := recover(); p != nil {
			// l.dirty is kept: the next iteration flushes what was queued.
			log.Printf("[panic] loop %d: %v", l.id, p)
		}
	}()
	for {
		timeout := -1
		if l.ndead > 0 {
			timeout = 200
			l.collectRevived()
		}
		n, err := unix.EpollWait(l.epfd, l.events, timeout)
		if err != nil {
			if err == unix.EINTR {
				continue
			}
			log.Fatalf("epoll_wait: %v", err)
		}
		for i := range n {
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
				l.accept(fd)
			case kindPG:
				c := l.byFd[int32(fd)]
				if ev.Events&unix.EPOLLOUT != 0 {
					l.flushPG(c)
				}
				if ev.Events&(unix.EPOLLIN|unix.EPOLLHUP|unix.EPOLLERR) != 0 && !c.dead {
					l.readPG(c)
				}
			}
		}
		l.flushBatches()
		if debugHook != nil {
			debugHook(l)
		}
		if n > 0 && n < lightIteration && l.window > 0 {
			// nanosleep on the loop's own thread: time.Sleep would park the goroutine
			// and need another thread to wake the locked one.
			_ = unix.Nanosleep(&l.windowTS, nil)
		}
	}
}

// Raw system calls on non-blocking descriptors: they never block, so they skip the
// scheduler's entersyscall/exitsyscall.
func rawRead(fd int, p []byte) (int, unix.Errno) {
	n, _, e := unix.RawSyscall(unix.SYS_READ, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)))
	return int(n), e
}

func rawWrite(fd int, p []byte) (int, unix.Errno) {
	n, _, e := unix.RawSyscall(unix.SYS_WRITE, uintptr(fd), uintptr(unsafe.Pointer(unsafe.SliceData(p))), uintptr(len(p)))
	return int(n), e
}

func (l *loop) accept(lfd int) {
	for range 256 {
		nfd, _, e := unix.RawSyscall6(unix.SYS_ACCEPT4, uintptr(lfd), 0, 0, unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0, 0)
		if e != 0 {
			if e != unix.EAGAIN && e != unix.ECONNABORTED && e != unix.EINTR {
				log.Printf("[accept] %v", e)
			}
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
		_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1)
		slots[fd] = slot{gen: slots[fd].gen, loop: uint16(l.id), kind: kindClient}
		if err := unix.EpollCtl(l.epfd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP, Fd: int32(fd)}); err != nil {
			l.closeClient(fd)
		}
	}
}

func (l *loop) closeClient(fd int) {
	// Release the slot before the descriptor: once it is closed, another loop may accept
	// a connection with the same number and claim the slot.
	s := &slots[fd]
	s.gen++
	s.kind, s.flags = kindFree, 0
	delete(l.inbuf, int32(fd))
	delete(l.outbuf, int32(fd))
	unix.Close(fd)
}

func (l *loop) readClient(fd int) {
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

// processInput handles the requests in data until one waits for Postgres; the rest is kept.
func (l *loop) processInput(fd int, data []byte) {
	for len(data) > 0 {
		var r request
		used, res := parseRequest(data, &r)
		switch res {
		case parseIncomplete:
			l.keepInput(fd, data)
			return
		case parseBad:
			l.respond(fd, 400, errorBody("malformed request"), false, true)
			return
		case parseTooLarge:
			l.respond(fd, 413, errorBody("request too large"), false, true)
			return
		}
		data = data[used:]
		j := job{fd: int32(fd), gen: slots[fd].gen}
		needsDB, status, body := l.route(&r, &j)
		if !needsDB {
			l.respond(fd, status, body, r.gzip, r.close)
			if r.close || slots[fd].kind != kindClient {
				return
			}
			continue
		}
		slots[fd].flags |= flagBusy
		// Keep the rest first: dispatch can answer at once (no database connection), and
		// that answer resumes from inbuf.
		l.keepInput(fd, data)
		l.dispatch(&j)
		return
	}
	delete(l.inbuf, int32(fd))
}

// route runs the request's validation; a panic there answers 500 instead of stopping
// every connection of the process.
func (l *loop) route(r *request, j *job) (needsDB bool, status int, body []byte) {
	defer func() {
		if p := recover(); p != nil {
			log.Printf("[panic] %v", p)
			needsDB, status, body = false, 500, internalErrorBody
		}
	}()
	return l.a.route(r, j)
}

func (l *loop) keepInput(fd int, data []byte) {
	if len(data) == 0 {
		delete(l.inbuf, int32(fd))
		return
	}
	l.inbuf[int32(fd)] = append([]byte(nil), data...)
}

// respond sends a complete response; body may be gzipped when the client accepts it.
func (l *loop) respond(fd int, status int, body []byte, acceptGzip, close bool) {
	gzipped := false
	if acceptGzip && len(body) >= gzipMinSize {
		l.gzOut = l.gz.appendGzip(l.gzOut[:0], body)
		body, gzipped = l.gzOut, true
	}
	l.resp = appendHead(l.resp[:0], status, len(body), gzipped, close)
	l.resp = append(l.resp, body...)
	l.send(fd, l.resp, close)
}

func (l *loop) send(fd int, p []byte, close bool) {
	s := &slots[fd]
	if pending, ok := l.outbuf[int32(fd)]; ok {
		l.outbuf[int32(fd)] = append(pending, p...)
		if close {
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
		if close {
			l.closeClient(fd)
		}
		return
	}
	l.outbuf[int32(fd)] = append([]byte(nil), p...)
	if close {
		s.flags |= flagCloseAfterWrite
	}
	s.flags |= flagWantOut
	_ = unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP | unix.EPOLLOUT, Fd: int32(fd)})
}

func (l *loop) flushClient(fd int) {
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
	_ = unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLRDHUP, Fd: int32(fd)})
}

// dispatch queues the job's statement on a Postgres connection; it is sent at the end of
// the iteration. All reads of an iteration share one connection and one Sync.
func (l *loop) dispatch(j *job) {
	var c *pgc
	if isWrite(j.stmt) {
		c = leastLoaded(l.writers)
	} else {
		if l.cur == nil || l.cur.dead {
			l.cur = leastLoaded(l.readers)
		}
		c = l.cur
	}
	if c == nil {
		l.fail(j, "no database connection")
		return
	}
	var p0, p1 [8]byte
	switch j.stmt {
	case stmtHealth:
		c.out = appendBindExecute(c.out, j.stmt)
	case stmtFeed, stmtPost:
		binary.BigEndian.PutUint64(p0[:], uint64(j.a))
		c.out = appendBindExecute(c.out, j.stmt, p0[:])
	case stmtInsertPost:
		binary.BigEndian.PutUint64(p0[:], uint64(j.a))
		c.out = appendBindExecute(c.out, j.stmt, p0[:], unsafe.Slice(unsafe.StringData(j.body), len(j.body)))
	case stmtLike:
		binary.BigEndian.PutUint64(p0[:], uint64(j.a))
		binary.BigEndian.PutUint64(p1[:], uint64(j.b))
		c.out = appendBindExecute(c.out, j.stmt, p0[:], p1[:])
	}
	c.q = append(c.q, pgItem{j: *j})
	c.inflight++
	if c.write {
		c.out = appendSync(c.out)
		c.q = append(c.q, pgItem{sync: true})
	} else {
		c.inBatch = true
	}
	if !c.dirty {
		c.dirty = true
		l.dirty = append(l.dirty, c)
	}
}

func leastLoaded(cs []*pgc) *pgc {
	var best *pgc
	for _, c := range cs {
		if !c.dead && (best == nil || c.inflight < best.inflight) {
			best = c
		}
	}
	return best
}

func (l *loop) flushBatches() {
	// By index: a failed flush answers its requests, which can dispatch pipelined ones and
	// mark more connections dirty while this runs; those are flushed here too.
	for i := 0; i < len(l.dirty); i++ {
		c := l.dirty[i]
		c.dirty = false
		if c.dead {
			continue
		}
		if c.inBatch {
			c.out = appendSync(c.out)
			c.q = append(c.q, pgItem{sync: true})
			c.inBatch = false
		}
		l.flushPG(c)
	}
	l.dirty = l.dirty[:0]
	l.cur = nil
}

func (l *loop) flushPG(c *pgc) {
	for len(c.out) > 0 {
		n, e := rawWrite(c.fd, c.out)
		if e == unix.EINTR {
			continue
		}
		if e == unix.EAGAIN {
			if !c.wantOut {
				c.wantOut = true
				_ = unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, c.fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLOUT, Fd: int32(c.fd)})
			}
			return
		}
		if e != 0 {
			l.killPG(c, e)
			return
		}
		c.out = c.out[:copy(c.out, c.out[n:])]
	}
	if c.wantOut {
		c.wantOut = false
		_ = unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, c.fd, &unix.EpollEvent{Events: unix.EPOLLIN, Fd: int32(c.fd)})
	}
}

func (l *loop) readPG(c *pgc) {
	for {
		if cap(c.in)-len(c.in) < 16<<10 {
			c.in = append(c.in, make([]byte, 64<<10)...)[:len(c.in)]
		}
		n, e := rawRead(c.fd, c.in[len(c.in):cap(c.in)])
		if e == unix.EAGAIN {
			break
		}
		if e == unix.EINTR {
			continue
		}
		if e != 0 || n == 0 {
			l.killPG(c, e)
			return
		}
		c.in = c.in[:len(c.in)+n]
		if len(c.in) < cap(c.in) {
			break
		}
	}
	buf := c.in
	for {
		typ, payload, n := nextMessage(buf)
		if n == 0 {
			break
		}
		buf = buf[n:]
		if !l.handlePG(c, typ, payload) {
			return
		}
	}
	c.in = c.in[:copy(c.in, buf)]
}

func (c *pgc) front() *pgItem {
	if c.qhead >= len(c.q) {
		return nil
	}
	return &c.q[c.qhead]
}

func (c *pgc) pop() {
	c.q[c.qhead] = pgItem{}
	c.qhead++
	switch {
	case c.qhead == len(c.q):
		c.q, c.qhead = c.q[:0], 0
	case c.qhead >= 1024 && c.qhead*2 >= len(c.q):
		// Under steady load the queue is never empty: move the live part down.
		c.q, c.qhead = c.q[:copy(c.q, c.q[c.qhead:])], 0
	}
}

// handlePG applies one backend message to the item at the head of the queue.
func (l *loop) handlePG(c *pgc, typ byte, payload []byte) bool {
	it := c.front()
	switch typ {
	case 'D':
		if it == nil || it.sync {
			l.killPG(c, errors.New("unexpected DataRow"))
			return false
		}
		cols, ok := rowColumns(payload, l.cols)
		if !ok {
			l.killPG(c, errors.New("bad DataRow"))
			return false
		}
		l.cols = cols
		l.row(c, &it.j, cols)
	case 'C', 'I':
		if it == nil || it.sync {
			l.killPG(c, errors.New("unexpected CommandComplete"))
			return false
		}
		l.complete(c, &it.j, payload)
		c.inflight--
		c.pop()
	case 'E':
		code, msg := errorFields(payload)
		if it == nil {
			l.killPG(c, errors.New("postgres: "+msg))
			return false
		}
		if !it.sync {
			j := it.j
			c.inflight--
			c.pop()
			c.rows, c.body = 0, c.body[:0]
			switch {
			case j.stmt == stmtLike && code == "23503":
				l.finish(&j, 404, errorBody("post not found"))
			default:
				log.Printf("[db] %s: %s", code, msg)
				l.fail(&j, msg)
			}
			// Postgres skips the rest of the transaction until its Sync.
			for it := c.front(); it != nil && !it.sync; it = c.front() {
				j := it.j
				c.inflight--
				c.pop()
				l.fail(&j, "transaction aborted")
			}
		}
	case 'Z':
		if it == nil || !it.sync {
			l.killPG(c, errors.New("unexpected ReadyForQuery"))
			return false
		}
		c.pop()
	}
	// BindComplete, NoData, notices and parameter statuses need nothing.
	return true
}

func (l *loop) row(c *pgc, j *job, cols [][]byte) {
	switch j.stmt {
	case stmtFeed, stmtPost:
		if len(cols) != 5 || len(cols[0]) != 8 || len(cols[2]) != 8 || len(cols[4]) != 4 {
			return
		}
		if j.stmt == stmtFeed {
			if c.rows == 0 {
				c.body = append(c.body[:0], `{"posts":[`...)
			} else {
				c.body = append(c.body, ',')
			}
		} else {
			c.body = append(c.body[:0], `{"post":`...)
		}
		c.body = appendPost(c.body, int64(binary.BigEndian.Uint64(cols[0])), cols[1],
			int64(binary.BigEndian.Uint64(cols[2])), cols[3], int64(int32(binary.BigEndian.Uint32(cols[4]))))
	case stmtInsertPost:
		if len(cols) != 2 || len(cols[0]) != 8 || len(cols[1]) != 8 {
			return
		}
		c.body = append(c.body[:0], `{"post":`...)
		c.body = appendPost(c.body, int64(binary.BigEndian.Uint64(cols[0])), unsafe.Slice(unsafe.StringData(j.body), len(j.body)),
			int64(binary.BigEndian.Uint64(cols[1])), unsafe.Slice(unsafe.StringData(j.username), len(j.username)), 0)
	}
	c.rows++
}

func (l *loop) complete(c *pgc, j *job, tag []byte) {
	rows := c.rows
	c.rows = 0
	switch j.stmt {
	case stmtHealth:
		b := append(c.body[:0], `{"status":"ok","db":"ok","uptime_s":`...)
		b = strconv.AppendInt(b, int64(time.Since(l.a.started).Seconds()), 10)
		c.body = append(b, '}')
		l.finish(j, 200, c.body)
	case stmtFeed:
		if rows == 0 {
			c.body = append(c.body[:0], `{"posts":[`...)
		}
		c.body = append(c.body, "]}"...)
		l.finish(j, 200, c.body)
	case stmtPost:
		if rows == 0 {
			l.finish(j, 404, errorBody("post not found"))
			break
		}
		c.body = append(c.body, '}')
		l.finish(j, 200, c.body)
	case stmtInsertPost:
		if rows == 0 {
			l.fail(j, "insert returned no row")
			break
		}
		c.body = append(c.body, '}')
		l.finish(j, 201, c.body)
	case stmtLike:
		n := rowsAffected(tag)
		status := 200
		if n > 0 {
			status = 201
		}
		b := append(c.body[:0], `{"liked":true,"already_liked":`...)
		b = strconv.AppendBool(b, n == 0)
		b = append(b, `,"post_id":`...)
		b = strconv.AppendInt(b, j.b, 10)
		c.body = append(b, '}')
		l.finish(j, status, c.body)
	}
	c.body = c.body[:0]
}

var internalErrorBody = errorBody("internal server error")

// fail answers a job whose statement could not run.
func (l *loop) fail(j *job, msg string) {
	if j.stmt == stmtHealth {
		b := append([]byte(`{"status":"degraded","db":"unreachable","error":"`), nil...)
		b = appendEscaped(b, msg)
		l.finish(j, 503, append(b, `"}`...))
		return
	}
	l.finish(j, 500, internalErrorBody)
}

// finish sends the job's response if its connection is still the one that asked.
func (l *loop) finish(j *job, status int, body []byte) {
	fd := int(j.fd)
	s := &slots[fd]
	if s.kind != kindClient || s.gen != j.gen || s.loop != uint16(l.id) {
		return
	}
	s.flags &^= flagBusy
	l.respond(fd, status, body, j.gzip, j.close)
	if slots[fd].kind != kindClient || slots[fd].gen != j.gen {
		return
	}
	if pending, ok := l.inbuf[int32(fd)]; ok {
		delete(l.inbuf, int32(fd))
		l.processInput(fd, pending)
	}
}

// killPG fails everything queued on a broken connection and reconnects it in the background.
func (l *loop) killPG(c *pgc, cause error) {
	if c.dead {
		return
	}
	log.Printf("[db] connection lost: %v", cause)
	c.dead = true
	l.ndead++
	delete(l.byFd, int32(c.fd))
	slots[c.fd] = slot{gen: slots[c.fd].gen + 1}
	unix.Close(c.fd)
	for it := c.front(); it != nil; it = c.front() {
		sync, j := it.sync, it.j
		c.pop()
		if !sync {
			l.fail(&j, "database connection lost")
		}
	}
	c.inflight, c.rows, c.body, c.out, c.in = 0, 0, c.body[:0], c.out[:0], c.in[:0]
	go func(write bool, idx int) {
		for {
			fd, err := connectPG(context.Background(), l.connStr)
			if err == nil {
				l.revived <- revived{fd: fd, write: write, idx: idx}
				return
			}
			log.Printf("[db] reconnect: %v", err)
			time.Sleep(time.Second)
		}
	}(c.write, c.idx)
}

func (l *loop) collectRevived() {
	for {
		select {
		case r := <-l.revived:
			c := l.readers
			if r.write {
				c = l.writers
			}
			if err := l.attachPG(c[r.idx], r.fd); err != nil {
				log.Printf("[db] attach: %v", err)
				unix.Close(r.fd)
				continue
			}
			l.ndead--
		default:
			return
		}
	}
}
