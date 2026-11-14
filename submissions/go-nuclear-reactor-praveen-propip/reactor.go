//go:build linux

package main

import (
	"errors"
	"fmt"
	"log/slog"
	"net"
	"runtime/debug"
	"strconv"
	"syscall"
	"time"
	"unsafe"

	"golang.org/x/sys/unix"
)

// The reactor: one process per core (main.go), each running exactly one of
// these loops on one Go P, with its own epoll set, its own SO_REUSEPORT
// listeners and its own Postgres sockets. Nothing is shared, so nothing locks.
// An idle connection costs one slot in a table indexed by descriptor; buffers
// belong to the loop and are only ever held by a connection across a read
// when it is pipelining or trickling bytes.

// slot kinds; the descriptor's slot says what an epoll event is for.
const (
	kindFree uint8 = iota
	kindClient
	kindListener
	kindPG
)

// Client sockets are edge-triggered: level-triggered epoll polls every socket
// a second time after delivering its event, only to find it quiet. So a read
// that fills the buffer must look again, and a paused socket gets its event
// back from the EPOLL_CTL_MOD that resumes it.
const (
	clientEvents = unix.EPOLLIN | unix.EPOLLRDHUP | unix.EPOLLET
	writeEvents  = clientEvents | unix.EPOLLOUT
)

// slot flags.
const (
	fBusy    = 1 << 0 // a statement is on the wire for this connection
	fClose   = 1 << 1 // close once the output has drained
	fStash   = 1 << 2 // l.stash holds unparsed input
	fPartial = 1 << 3 // l.partial holds unsent output
	fPaused  = 1 << 4 // EPOLLIN is off (pipelined backlog)
	fEOF     = 1 << 5 // the peer finished sending: close once it has its answers
	fSend    = 1 << 6 // a response is queued on the ring
	fRecv    = 1 << 7 // a read is queued on the ring

	fBlocked = fBusy | fPartial | fSend // the connection owes a response: serve nothing more
)

// slot is one descriptor's whole per-connection state: 12 pointer-free bytes.
type slot struct {
	gen   uint32 // distinguishes a descriptor's successive connections
	last  int32  // loop clock (seconds since start) of the last activity
	kind  uint8
	flags uint8
	aux   uint16 // kindPG: index into l.pgs
}

type loop struct {
	epfd   int
	slots  []slot // mmap'd, indexed by descriptor; untouched pages cost nothing
	gen    uint32
	maxFd  int32
	sweep  int32 // next descriptor the idle sweep looks at
	events []unix.EpollEvent

	start  time.Time
	now    int64 // unix seconds, read once per iteration
	clock  int32 // seconds since start
	inbuf  []byte
	pgbuf  []byte
	tmp    []byte
	sc     parseScratch
	gz     *gzEncoder
	stash  map[int32][]byte // unparsed input of pipelining or slow clients
	output map[int32][]byte // unsent output of slow clients

	ring     *ring
	dsn      *pgDSN
	pgs      []*pgConn
	reads    []*pgConn
	writes   []*pgConn
	rr       int
	batch    *pgConn // this iteration's read batch
	dirty    []*pgConn
	wq       []pending // writes waiting for the next transaction
	warena   []byte    // their bodies and usernames
	dialedCh chan dialed
}

// ---- raw syscalls ----
//
// Sockets here are nonblocking, so these return immediately: the raw form
// skips the runtime's syscall bookkeeping on the two calls every request
// makes. send skips the VFS layer that write goes through, and never raises
// SIGPIPE.

func rawRead(fd int32, b []byte) (int, syscall.Errno) {
	n, _, e := syscall.RawSyscall(syscall.SYS_READ, uintptr(fd), uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)))
	return int(n), e
}

func rawWrite(fd int32, b []byte) (int, syscall.Errno) {
	n, _, e := syscall.RawSyscall6(syscall.SYS_SENDTO, uintptr(fd), uintptr(unsafe.Pointer(&b[0])), uintptr(len(b)),
		unix.MSG_DONTWAIT|unix.MSG_NOSIGNAL, 0, 0)
	return int(n), e
}

func (l *loop) epollAdd(fd int32, events uint32) {
	ev := unix.EpollEvent{Events: events, Fd: fd}
	if err := unix.EpollCtl(l.epfd, unix.EPOLL_CTL_ADD, int(fd), &ev); err != nil {
		slogError("epoll_ctl add", err)
	}
}

func (l *loop) epollMod(fd int32, events uint32) {
	ev := unix.EpollEvent{Events: events, Fd: fd}
	_ = unix.EpollCtl(l.epfd, unix.EPOLL_CTL_MOD, int(fd), &ev)
}

func (l *loop) noteFd(fd int32) {
	if fd >= int32(len(l.slots)) {
		panic("descriptor past the slot table")
	}
	if fd > l.maxFd {
		l.maxFd = fd
	}
}

// ---- setup ----

func newLoop(dsn *pgDSN, readConns, writeConns int) (*loop, error) {
	epfd, err := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
	if err != nil {
		return nil, err
	}
	var lim unix.Rlimit
	if err := unix.Getrlimit(unix.RLIMIT_NOFILE, &lim); err != nil {
		return nil, err
	}
	n := min(max(lim.Cur, 1<<16), 1<<24)
	mem, err := unix.Mmap(-1, 0, int(n)*int(unsafe.Sizeof(slot{})),
		unix.PROT_READ|unix.PROT_WRITE, unix.MAP_ANON|unix.MAP_PRIVATE|unix.MAP_NORESERVE)
	if err != nil {
		return nil, err
	}
	l := &loop{
		epfd:     epfd,
		ring:     newRing(),
		slots:    unsafe.Slice((*slot)(unsafe.Pointer(&mem[0])), n),
		events:   make([]unix.EpollEvent, epollBatch),
		start:    time.Now(),
		inbuf:    make([]byte, readBufSize),
		pgbuf:    make([]byte, pgBufSize),
		tmp:      make([]byte, hdrRoom, 4096),
		gz:       newGzEncoder(),
		stash:    make(map[int32][]byte),
		output:   make(map[int32][]byte),
		dsn:      dsn,
		dialedCh: make(chan dialed, readConns+writeConns),
	}
	for i := 0; i < readConns+writeConns; i++ {
		c := &pgConn{
			idx: i, fd: -1, writer: i >= readConns,
			ring: make([]pending, 256),
			json: make([]byte, hdrRoom, 16<<10),
		}
		l.pgs = append(l.pgs, c)
		if c.writer {
			l.writes = append(l.writes, c)
		} else {
			l.reads = append(l.reads, c)
		}
	}
	return l, nil
}

// connectAll opens the Postgres sockets before the first request; a socket
// that can't connect keeps trying in the background and /health says so.
func (l *loop) connectAll() {
	for i := range l.pgs {
		fd, err := connectPG(l.dsn)
		if err != nil {
			slogError("database connect", err, "conn", i)
			go l.redial(i)
			continue
		}
		l.adopt(dialed{i, fd})
	}
}

// listen binds host:port with SO_REUSEPORT: every process has its own accept
// queue and the kernel spreads new connections across them. TCP_NODELAY is
// inherited by accepted sockets, which saves a syscall on each.
func (l *loop) listen(host, port string) error {
	domain, addr, err := parseHostPort(host, port)
	if err != nil {
		return err
	}
	fd, err := unix.Socket(domain, unix.SOCK_STREAM|unix.SOCK_CLOEXEC|unix.SOCK_NONBLOCK, 0)
	if err != nil {
		return err
	}
	_ = unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEADDR, 1)
	_ = unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEPORT, 1)
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1)
	if err := unix.Bind(fd, addr); err != nil {
		unix.Close(fd)
		return err
	}
	if err := unix.Listen(fd, 65535); err != nil { // somaxconn on the box
		unix.Close(fd)
		return err
	}
	l.noteFd(int32(fd))
	l.slots[fd].kind = kindListener
	l.epollAdd(int32(fd), unix.EPOLLIN)
	return nil
}

func parseHostPort(host, port string) (int, unix.Sockaddr, error) {
	p, err := strconv.Atoi(port)
	if err != nil || p < 0 || p > 65535 {
		return 0, nil, errors.New("bad port: " + port)
	}
	if host == "" || host == "0.0.0.0" {
		return unix.AF_INET, &unix.SockaddrInet4{Port: p}, nil
	}
	ip := net.ParseIP(host)
	if ip4 := ip.To4(); ip4 != nil {
		sa := &unix.SockaddrInet4{Port: p}
		copy(sa.Addr[:], ip4)
		return unix.AF_INET, sa, nil
	}
	if ip6 := ip.To16(); ip6 != nil {
		sa := &unix.SockaddrInet6{Port: p}
		copy(sa.Addr[:], ip6)
		return unix.AF_INET6, sa, nil
	}
	return 0, nil, errors.New("unsupported HOST: " + host)
}

// ---- event loop ----

func (l *loop) run() {
	nextSecond := int32(0)
	window := batchMinWindow
	for {
		n, err := unix.EpollWait(l.epfd, l.events, 1000)
		if err != nil && err != unix.EINTR {
			slogError("epoll_wait", err)
			return
		}
		now := time.Now()
		l.now = now.Unix()
		l.clock = int32(now.Sub(l.start) / time.Second)
		l.sc.now = l.now

		for i := 0; i < n; {
			i = l.dispatch(i, n)
		}
		l.flushRing()
		l.flushPG()

		if l.clock >= nextSecond {
			nextSecond = l.clock + 1
			l.housekeeping()
		}
		l.flushRing() // failure paths above may have answered clients
		if n > 0 {
			window = nextWindow(window, n)
			if n < batchTarget {
				pause(window)
			}
		}
	}
}

// dispatch handles events from i on. A panic costs only the connection that
// caused it, as in net/http; next is where the loop picks the batch back up.
func (l *loop) dispatch(i, n int) (next int) {
	defer func() {
		if r := recover(); r != nil {
			l.drop(l.events[next-1].Fd, r)
		}
	}()
	for next = i; next < n; {
		ev := &l.events[next]
		next++
		fd := ev.Fd
		s := &l.slots[fd]
		switch s.kind {
		case kindClient:
			l.clientEvent(fd, s, ev.Events)
		case kindPG:
			l.pgEvent(l.pgs[s.aux], ev.Events)
		case kindListener:
			l.accept(fd)
		}
		// kindFree: closed earlier in this batch.
	}
	return next
}

func (l *loop) drop(fd int32, r any) {
	slog.Error("panic", "fd", fd, "why", fmt.Sprint(r), "stack", string(debug.Stack()))
	switch s := &l.slots[fd]; s.kind {
	case kindClient:
		l.closeConn(fd, s)
	case kindPG:
		l.pgLost(l.pgs[s.aux], "panic while reading results")
	}
}

// pause waits out the batching window, so one wake-up serves what would have
// been many. nextWindow steers it toward batchTarget events per wake-up, so it
// follows each worker's own rate: a busy loop never pauses, a quiet one waits
// longer. A request crosses two or three windows, a few ms at the very most.
func pause(window int64) {
	ts := unix.Timespec{Nsec: window}
	_ = unix.Nanosleep(&ts, nil)
}

func nextWindow(w int64, events int) int64 {
	w += (w*batchTarget/int64(events) - w) / 4
	return min(max(w, batchMinWindow), batchMaxWindow)
}

const (
	batchTarget    = 32
	batchMinWindow = int64(500 * time.Microsecond)
	batchMaxWindow = int64(8 * time.Millisecond)
)

func (l *loop) housekeeping() {
	for len(l.dialedCh) > 0 {
		l.adopt(<-l.dialedCh)
	}
	l.pgWatchdog()
	// Idle sweep: a sixtieth of the table per second, so a pass never stalls
	// the loop.
	step := l.maxFd/60 + 1
	for i := int32(0); i < step; i++ {
		l.sweep++
		if l.sweep > l.maxFd {
			l.sweep = 0
		}
		s := &l.slots[l.sweep]
		if s.kind == kindClient && s.flags&fBusy == 0 && l.clock-s.last > idleTimeoutSecs {
			l.closeConn(l.sweep, s)
		}
	}
}

// accept drains a listener's backlog (bounded, level-triggered epoll brings us
// back for the rest).
func (l *loop) accept(lfd int32) {
	for i := 0; i < 128; i++ {
		r, _, e := syscall.RawSyscall6(syscall.SYS_ACCEPT4, uintptr(lfd), 0, 0,
			unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0, 0)
		if e != 0 {
			return // EAGAIN or transient
		}
		fd := int32(r)
		l.noteFd(fd)
		l.gen++
		l.slots[fd] = slot{gen: l.gen, last: l.clock, kind: kindClient}
		l.epollAdd(fd, clientEvents)
	}
}

func (l *loop) clientEvent(fd int32, s *slot, events uint32) {
	if events&(unix.EPOLLHUP|unix.EPOLLERR) != 0 {
		l.closeConn(fd, s)
		return
	}
	if events&unix.EPOLLOUT != 0 {
		l.flushOutput(fd, s)
		if s.kind != kindClient {
			return
		}
	}
	if events == unix.EPOLLIN && l.ring != nil && s.flags&(fStash|fPaused|fRecv) == 0 && l.ring.recv(fd) {
		s.flags |= fRecv
		return
	}
	if events&(unix.EPOLLIN|unix.EPOLLRDHUP) != 0 {
		for l.readable(fd, s) { // edge-triggered: a full buffer may have left more
		}
	}
	// The hang-up edge arrives once, possibly with the last request.
	if events&unix.EPOLLRDHUP != 0 && s.kind == kindClient {
		s.flags |= fEOF
		l.closeIfDone(fd, s)
	}
}

// closeIfDone closes a connection whose peer stopped sending, once nothing is
// left to answer. Stashed bytes with nothing running are a request cut short.
func (l *loop) closeIfDone(fd int32, s *slot) {
	if s.flags&fEOF != 0 && s.flags&fBlocked == 0 {
		l.closeConn(fd, s)
	}
}

// readable reads once; true means the read filled the buffer, so read again.
func (l *loop) readable(fd int32, s *slot) bool {
	s.last = l.clock
	have := 0
	if s.flags&fStash != 0 {
		st := l.stash[fd]
		if len(st) >= stashLimit {
			if s.flags&fBlocked != 0 { // backlog behind an answer: stop reading
				s.flags |= fPaused
				l.epollMod(fd, unix.EPOLLRDHUP|unix.EPOLLET)
				return false
			}
			l.reject(fd, s) // a request no complete parse can ever come from
			return false
		}
		have = copy(l.inbuf, st)
	}
	n, e := rawRead(fd, l.inbuf[have:])
	if e == syscall.EAGAIN {
		return false
	}
	if e != 0 {
		l.closeConn(fd, s)
		return false
	}
	if n == 0 { // the peer finished sending
		s.flags |= fEOF
		l.closeIfDone(fd, s)
		return false
	}
	if have > 0 {
		s.flags &^= fStash
		delete(l.stash, fd)
	}
	l.process(fd, s, l.inbuf[:have+n])
	return have+n == len(l.inbuf) && s.kind == kindClient && s.flags&fClose == 0
}

// process runs every complete request in b, stashing what it can't run yet:
// a connection has at most one statement on the wire, so responses stay in
// request order, and a client not reading its responses stops being served.
func (l *loop) process(fd int32, s *slot, b []byte) {
	for len(b) > 0 {
		if s.flags&fBlocked != 0 {
			l.stashInput(fd, s, b)
			return
		}
		p, used, fatal := parseRequest(b, &l.sc)
		if fatal {
			p.closeAfter = true
			l.respondImmediate(fd, s, &p)
			return
		}
		if used == 0 {
			l.stashInput(fd, s, b)
			return
		}
		b = b[used:]
		if p.kind == rImmediate {
			l.respondImmediate(fd, s, &p)
			if p.closeAfter || s.kind != kindClient {
				return
			}
			continue
		}
		if !l.submit(fd, s, &p) {
			l.noDatabase(fd, s, &p)
			if p.closeAfter || s.kind != kindClient {
				return
			}
		}
	}
}

func (l *loop) stashInput(fd int32, s *slot, b []byte) {
	s.flags |= fStash
	l.stash[fd] = append([]byte(nil), b...)
}

// resume continues a connection's stashed requests once its answer is out.
func (l *loop) resume(fd int32, s *slot) {
	if s.flags&fStash != 0 {
		b := l.stash[fd]
		delete(l.stash, fd)
		s.flags &^= fStash
		l.process(fd, s, b)
		if s.kind != kindClient {
			return
		}
	}
	if s.flags&fPaused != 0 && s.flags&fBlocked == 0 {
		s.flags &^= fPaused
		l.epollMod(fd, clientEvents) // the MOD redelivers a pending edge
	}
	l.closeIfDone(fd, s)
}

func (l *loop) respondImmediate(fd int32, s *slot, p *parsed) {
	if p.closeAfter {
		s.flags |= fClose
	}
	var out []byte
	l.tmp, out = errorResponse(l.tmp, int(p.status), immediateBody(p), p.closeAfter)
	l.writeClient(fd, s, out)
}

// noDatabase answers when no Postgres connection is up.
func (l *loop) noDatabase(fd int32, s *slot, p *parsed) {
	e := pending{kind: p.kind}
	if p.closeAfter {
		s.flags |= fClose
		e.flags = pClose
	}
	s.flags |= fBusy
	e.fd, e.gen = fd, s.gen
	l.answerFailure(&e, s, "no database connection")
}

func (l *loop) reject(fd int32, s *slot) {
	p := parsed{status: 400, reason: "malformed JSON body", closeAfter: true}
	l.respondImmediate(fd, s, &p)
}

// client returns the live connection a finished statement belongs to, nil if
// it went away meanwhile (its descriptor may even be a new connection now).
func (l *loop) client(e *pending) *slot {
	s := &l.slots[e.fd]
	if s.kind != kindClient || s.gen != e.gen {
		return nil
	}
	return s
}

func (l *loop) answer(e *pending, s *slot, code int, body []byte) {
	var out []byte
	l.tmp, out = errorResponse(l.tmp, code, body, e.flags&pClose != 0)
	l.finish(e, s, out)
}

// finish writes a statement's response, then moves on to whatever the
// connection stashed behind it.
func (l *loop) finish(e *pending, s *slot, out []byte) {
	s.flags &^= fBusy
	s.last = l.clock
	if e.flags&pClose != 0 {
		s.flags |= fClose
	}
	l.writeClient(e.fd, s, out)
	if s.kind == kindClient && s.flags&fBlocked == 0 {
		l.resume(e.fd, s)
	}
}

// writeClient writes one response in one syscall; what the socket won't take
// goes to l.output and EPOLLOUT finishes it. Nothing else is served on the
// connection meanwhile, so there is never more than one response queued.
func (l *loop) writeClient(fd int32, s *slot, b []byte) {
	if l.ring != nil && l.ring.send(fd, b) {
		s.flags |= fSend
		return
	}
	n, e := rawWrite(fd, b)
	switch {
	case e == 0 && n == len(b):
		if s.flags&fClose != 0 {
			l.closeConn(fd, s)
		}
		return
	case e == syscall.EAGAIN:
		n = 0
	case e != 0:
		l.closeConn(fd, s)
		return
	}
	l.backlog(fd, s, b[n:])
}

// backlog keeps what the socket wouldn't take for EPOLLOUT to finish.
func (l *loop) backlog(fd int32, s *slot, b []byte) {
	s.flags |= fPartial
	l.output[fd] = append([]byte(nil), b...)
	if s.flags&fPaused != 0 {
		l.epollMod(fd, unix.EPOLLOUT|unix.EPOLLRDHUP|unix.EPOLLET)
	} else {
		l.epollMod(fd, writeEvents)
	}
}

func (l *loop) flushOutput(fd int32, s *slot) {
	q := l.output[fd]
	for len(q) > 0 {
		n, e := rawWrite(fd, q)
		if e == syscall.EAGAIN {
			l.output[fd] = q
			return
		}
		if e != 0 {
			l.closeConn(fd, s)
			return
		}
		q = q[n:]
	}
	delete(l.output, fd)
	s.flags &^= fPartial
	if s.flags&fClose != 0 {
		l.closeConn(fd, s)
		return
	}
	if s.flags&fPaused == 0 {
		l.epollMod(fd, clientEvents)
	}
	l.resume(fd, s)
}

// closeConn releases a connection; the kernel drops it from epoll on close.
// One with an operation on the ring closes when that completes, since the
// descriptor could otherwise be reused under it.
func (l *loop) closeConn(fd int32, s *slot) {
	if s.flags&(fSend|fRecv) != 0 {
		s.flags |= fClose
		return
	}
	if s.flags&fStash != 0 {
		delete(l.stash, fd)
	}
	if s.flags&fPartial != 0 {
		delete(l.output, fd)
	}
	s.kind = kindFree
	s.flags = 0
	syscall.RawSyscall(syscall.SYS_CLOSE, uintptr(fd), 0, 0)
}
