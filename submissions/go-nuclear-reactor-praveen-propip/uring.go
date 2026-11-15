//go:build linux

package main

import (
	"sync/atomic"
	"syscall"
	"unsafe"

	"golang.org/x/sys/unix"
)

// One io_uring per loop batches an iteration's client reads and writes into a
// single io_uring_enter. Sockets stay nonblocking and epoll still reports
// readiness, so every operation finishes inside that enter, on this thread.
// A kernel without io_uring (or one that forbids it) leaves l.ring nil and
// the loop falls back to one syscall per read and write.

const (
	useRing      = true
	ringEntries  = 1024
	sendArena    = 1 << 20
	recvChunk    = 4096
	sysRingSetup = 425
	sysRingEnter = 426

	ringOffSQ   = 0
	ringOffCQ   = 0x8000000
	ringOffSQEs = 0x10000000

	setupSubmitAll    = 1 << 7
	setupSingleIssuer = 1 << 12
	setupDeferTaskrun = 1 << 13
	featSingleMmap    = 1 << 0
	enterGetEvents    = 1 << 0

	opSend = 26
	opRecv = 27

	sockFlags = unix.MSG_DONTWAIT | unix.MSG_NOSIGNAL
)

type ringOffsets struct {
	head, tail, mask, entries, flags, dropped, array, resv uint32
	userAddr                                               uint64
}

type ringParams struct {
	sqEntries, cqEntries, flags, sqThreadCPU, sqThreadIdle, features, wqFd uint32
	resv                                                                   [3]uint32
	sq, cq                                                                 ringOffsets
}

type sqe struct {
	opcode, flags uint8
	ioprio        uint16
	fd            int32
	off, addr     uint64
	length        uint32
	msgFlags      uint32
	userData      uint64
	pad           [3]uint64
}

type cqe struct {
	userData uint64
	res      int32
	flags    uint32
}

// ringRef is what a completion needs to know about its operation.
type ringRef struct{ off, n int32 }

type ring struct {
	fd                     int
	sqTail, cqHead, cqTail *uint32
	sqMask, cqMask         uint32
	sqes                   []sqe
	cqes                   []cqe
	tail                   uint32 // our copy of the submission tail
	queued                 uint32 // not yet handed to the kernel
	used                   int32  // operations since the buffers were last reset
	refs                   [ringEntries]ringRef
	sendBuf                []byte
	sendOff                int32
	recvBuf                []byte
	recvs                  int32
}

func newRing() *ring {
	if !useRing {
		return nil
	}
	p := ringParams{flags: setupSubmitAll | setupSingleIssuer | setupDeferTaskrun}
	r0, _, e := syscall.Syscall(sysRingSetup, ringEntries, uintptr(unsafe.Pointer(&p)), 0)
	if e != 0 || p.features&featSingleMmap == 0 {
		return nil
	}
	fd := int(r0)
	cqesOff := p.cq.dropped // in the CQ offsets this word is cqes
	size := max(p.sq.array+p.sqEntries*4, cqesOff+p.cqEntries*uint32(unsafe.Sizeof(cqe{})))
	const shared = unix.PROT_READ | unix.PROT_WRITE
	rings, err := unix.Mmap(fd, ringOffSQ, int(size), shared, unix.MAP_SHARED|unix.MAP_POPULATE)
	if err != nil {
		unix.Close(fd)
		return nil
	}
	mem, err := unix.Mmap(fd, ringOffSQEs, int(p.sqEntries)*int(unsafe.Sizeof(sqe{})), shared, unix.MAP_SHARED|unix.MAP_POPULATE)
	if err != nil {
		unix.Close(fd)
		return nil
	}
	word := func(off uint32) *uint32 { return (*uint32)(unsafe.Pointer(&rings[off])) }
	r := &ring{
		fd:     fd,
		sqTail: word(p.sq.tail), cqHead: word(p.cq.head), cqTail: word(p.cq.tail),
		sqMask: *word(p.sq.mask), cqMask: *word(p.cq.mask),
		sqes:    unsafe.Slice((*sqe)(unsafe.Pointer(&mem[0])), p.sqEntries),
		cqes:    unsafe.Slice((*cqe)(unsafe.Pointer(&rings[cqesOff])), p.cqEntries),
		sendBuf: make([]byte, sendArena),
		recvBuf: make([]byte, epollBatch*recvChunk),
	}
	array := unsafe.Slice(word(p.sq.array), p.sqEntries)
	for i := range array {
		array[i] = uint32(i)
	}
	return r
}

func (r *ring) push(op uint8, fd int32, addr unsafe.Pointer, n int, ref ringRef) {
	i := r.tail & r.sqMask
	r.sqes[i] = sqe{
		opcode: op, fd: fd, addr: uint64(uintptr(addr)), length: uint32(n), msgFlags: sockFlags,
		userData: uint64(uint32(fd)) | uint64(op)<<32 | uint64(i)<<40,
	}
	r.refs[i] = ref
	r.tail++
	r.queued++
	r.used++
}

// send queues a response; false when this round is out of room.
func (r *ring) send(fd int32, b []byte) bool {
	if r.used == ringEntries || int(r.sendOff)+len(b) > len(r.sendBuf) {
		return false
	}
	off := r.sendOff
	copy(r.sendBuf[off:], b)
	r.sendOff += int32(len(b))
	r.push(opSend, fd, unsafe.Pointer(&r.sendBuf[off]), len(b), ringRef{off, int32(len(b))})
	return true
}

// recv queues a read; false when this round is out of room.
func (r *ring) recv(fd int32) bool {
	if r.used == ringEntries || int(r.recvs+1)*recvChunk > len(r.recvBuf) {
		return false
	}
	off := r.recvs * recvChunk
	r.recvs++
	r.push(opRecv, fd, unsafe.Pointer(&r.recvBuf[off]), recvChunk, ringRef{off, recvChunk})
	return true
}

func (r *ring) submit() {
	atomic.StoreUint32(r.sqTail, r.tail)
	for r.queued > 0 {
		n, _, e := syscall.RawSyscall6(sysRingEnter, uintptr(r.fd), uintptr(r.queued), 0, enterGetEvents, 0, 0)
		if e != 0 {
			if e == syscall.EINTR || e == syscall.EAGAIN || e == syscall.EBUSY {
				continue
			}
			panic("io_uring_enter: " + e.Error())
		}
		r.queued -= uint32(n)
	}
}

// flushRing runs what the iteration queued and everything its completions
// queue in turn.
func (l *loop) flushRing() {
	r := l.ring
	if r == nil {
		return
	}
	for r.queued > 0 {
		r.submit()
		for !l.reap() {
		}
	}
	r.sendOff, r.recvs, r.used = 0, 0, 0
}

// reap handles the completions that are ready; false means a panic cut it
// short and there may be more. Like dispatch, a panic costs one connection.
func (l *loop) reap() (done bool) {
	r := l.ring
	var fd int32
	defer func() {
		if p := recover(); p != nil {
			l.drop(fd, p)
		}
	}()
	head := *r.cqHead
	for tail := atomic.LoadUint32(r.cqTail); head != tail; {
		c := r.cqes[head&r.cqMask]
		head++
		atomic.StoreUint32(r.cqHead, head)
		fd = int32(uint32(c.userData))
		ref := r.refs[c.userData>>40]
		s := &l.slots[fd]
		if uint8(c.userData>>32) == opSend {
			l.sendDone(fd, s, c.res, r.sendBuf[ref.off:ref.off+ref.n])
		} else {
			l.recvDone(fd, s, c.res, r.recvBuf[ref.off:ref.off+ref.n])
		}
	}
	return true
}

func (l *loop) sendDone(fd int32, s *slot, res int32, b []byte) {
	s.flags &^= fSend
	switch {
	case int(res) == len(b):
		if s.flags&fClose != 0 {
			l.closeConn(fd, s)
			return
		}
		l.resume(fd, s)
	case res == -int32(syscall.EAGAIN):
		l.backlog(fd, s, b)
	case res < 0:
		l.closeConn(fd, s)
	default:
		l.backlog(fd, s, b[res:])
	}
}

func (l *loop) recvDone(fd int32, s *slot, res int32, b []byte) {
	s.flags &^= fRecv
	if s.flags&fClose != 0 {
		l.closeConn(fd, s)
		return
	}
	switch {
	case res == -int32(syscall.EAGAIN):
	case res < 0:
		l.closeConn(fd, s)
	case res == 0:
		s.flags |= fEOF
		l.closeIfDone(fd, s)
	default:
		l.process(fd, s, b[:res])
		// Edge-triggered: a full chunk may have left more.
		for int(res) == recvChunk && s.kind == kindClient && s.flags&(fClose|fPaused) == 0 && l.readable(fd, s) {
		}
	}
}
