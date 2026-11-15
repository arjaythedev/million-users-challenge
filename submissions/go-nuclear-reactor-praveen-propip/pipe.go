//go:build linux

package main

// The pipelined half of the Postgres client. Connections live in the event
// loop's epoll set: a request appends its Bind/Execute bytes to a connection's
// output buffer and a ring entry, answers come back in order and are matched
// to the ring. No goroutine, channel or lock sits between a request and its
// query.
//
// Reads that arrive in one loop iteration share one connection and one Sync:
// one write, one round trip.
//
// Writes group-commit. While a transaction is committing, new writes wait in
// the loop's queue; when a write connection is free the whole queue goes out
// as one transaction (one Sync, one fsync) and every response waits for the
// ReadyForQuery that follows the commit (rule 8). So the batch size follows
// the commit latency by itself. Likes are sorted inside a batch: batches on
// other connections and in other processes take the same unique-index entries
// in the same order, so they can't deadlock. If a batch fails, nothing in it
// committed, and each statement is replayed in a transaction of its own, so a
// like on a missing post fails alone (23503 -> 404).

import (
	"cmp"
	"encoding/binary"
	"net"
	"slices"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

var syncMsg = []byte{'S', 0, 0, 0, 4}

// Ring entry flags.
const (
	pGzip    = 1 << 0
	pClose   = 1 << 1
	pSyncEnd = 1 << 2 // the last statement before a Sync
	pSolo    = 1 << 3 // a write in a transaction of its own
	pFirst   = 1 << 4 // a like that inserted its row
)

// pending is one statement on the wire, oldest at the ring's head. A write
// keeps its result here until its commit.
type pending struct {
	id      int64  // post id; create: the id Postgres assigned
	uid     int64  // writes: user id
	ts      int64  // create: created_at
	extra   uint32 // create: offset of body + username in the write arena
	bodyLen uint32
	nameLen uint32
	fd      int32
	gen     uint32
	kind    reqKind
	flags   uint8
}

type pgConn struct {
	idx      int
	fd       int // -1 while disconnected
	writer   bool
	out      []byte // bytes not yet written
	outOff   int
	epollOut bool
	dirty    bool // on the loop's flush list
	broken   bool // a write failed; handled at the next flush
	unsynced int  // reads appended since the last Sync

	ring       []pending // power-of-two size
	head, tail uint32

	ack   uint32   // writer: the next statement to get its result
	extra []byte   // writer: the batch's bodies and usernames
	err   *pgError // writer: what failed the transaction on the wire

	partial  []byte    // incomplete trailing message of the last read
	json     []byte    // the head statement's response under construction
	marks    []rowMark // where its rows' fields are, for gzip
	rows     int       // rows received for the head statement
	aborted  bool      // an error is skipping the rest of this Sync's statements
	lastRecv int64
}

func (c *pgConn) outstanding() int { return int(c.tail - c.head) }

func (c *pgConn) push(p pending) {
	if c.outstanding() == len(c.ring) {
		grown := make([]pending, len(c.ring)*2)
		mask := uint32(len(c.ring) - 1)
		for i := c.head; i != c.tail; i++ {
			grown[i&uint32(len(grown)-1)] = c.ring[i&mask]
		}
		c.ring = grown
	}
	c.ring[c.tail&uint32(len(c.ring)-1)] = p
	c.tail++
}

func (c *pgConn) peek() *pending { return &c.ring[c.head&uint32(len(c.ring)-1)] }

func (c *pgConn) pop() pending {
	e := *c.peek()
	c.head++
	return e
}

// ---- statement templates ----

// buildBind frames Bind+Execute for a statement with n int8 parameters and
// binary results, returning the offsets of the parameter values.
func buildBind(stmt string, n int) ([]byte, [2]int) {
	var offs [2]int
	p := append([]byte{0}, stmt...) // unnamed portal, statement name
	p = append(p, 0, 0, 1, 0, 1)    // one format code: binary
	p = binary.BigEndian.AppendUint16(p, uint16(n))
	for i := 0; i < n; i++ {
		p = append(p, 0, 0, 0, 8)
		offs[i] = 5 + len(p)
		p = append(p, 0, 0, 0, 0, 0, 0, 0, 0)
	}
	p = append(p, 0, 1, 0, 1) // one result format: binary
	return append(frontendMsg('B', p), executeMsg...), offs
}

var executeMsg = []byte{'E', 0, 0, 0, 9, 0, 0, 0, 0, 0}

var (
	tmplFeed, _       = buildBind(stmtFeed, 0)
	tmplHealth, _     = buildBind(stmtOne, 0)
	tmplGet, offGet   = buildBind(stmtPost, 1)
	tmplLike, offLike = buildBind(stmtLike, 2)
)

// appendCreate frames Bind+Execute of the post insert.
func appendCreate(out []byte, uid int64, body []byte) []byte {
	start := len(out)
	out = append(out, 'B', 0, 0, 0, 0, 0)
	out = append(out, stmtIns...)
	out = append(out, 0, 0, 1, 0, 1, 0, 2, 0, 0, 0, 8)
	out = binary.BigEndian.AppendUint64(out, uint64(uid))
	out = binary.BigEndian.AppendUint32(out, uint32(len(body)))
	out = append(out, body...)
	out = append(out, 0, 1, 0, 1)
	binary.BigEndian.PutUint32(out[start+1:], uint32(len(out)-start-1))
	return append(out, executeMsg...)
}

// ---- submitting ----

// pickRead chooses the read connection for this iteration's batch: the one
// with the least outstanding, ties rotating.
func (l *loop) pickRead() *pgConn {
	var best *pgConn
	n := len(l.reads)
	l.rr++
	for i := 0; i < n; i++ {
		c := l.reads[(l.rr+i)%n]
		if c.fd >= 0 && (best == nil || c.outstanding() < best.outstanding()) {
			best = c
		}
	}
	return best
}

func (l *loop) markDirty(c *pgConn) {
	if !c.dirty {
		c.dirty = true
		l.dirty = append(l.dirty, c)
	}
}

// submit queues a parsed database request. False: no connection available.
func (l *loop) submit(fd int32, s *slot, p *parsed) bool {
	e := pending{fd: fd, gen: s.gen, kind: p.kind, id: p.id}
	if p.wantsGzip {
		e.flags |= pGzip
	}
	if p.closeAfter {
		e.flags |= pClose
	}
	if p.kind == rLike || p.kind == rCreate {
		if !l.writerUp() {
			return false
		}
		e.uid = p.uid
		l.queueWrite(e, p.body, p.username)
		s.flags |= fBusy
		return true
	}
	c := l.batch
	if c == nil || c.fd < 0 {
		if c = l.pickRead(); c == nil {
			return false
		}
		l.batch = c
	}
	if c.outstanding() == 0 {
		c.lastRecv = l.now
	}
	switch p.kind {
	case rFeed:
		c.out = append(c.out, tmplFeed...)
	case rGetPost:
		n := len(c.out)
		c.out = append(c.out, tmplGet...)
		binary.BigEndian.PutUint64(c.out[n+offGet[0]:], uint64(p.id))
	default:
		c.out = append(c.out, tmplHealth...)
	}
	c.push(e)
	c.unsynced++
	if c.unsynced >= maxBatchReads {
		l.seal(c)
		l.pgWrite(c)
		l.batch = nil
	}
	s.flags |= fBusy
	return true
}

func (l *loop) writerUp() bool {
	for _, c := range l.writes {
		if c.fd >= 0 {
			return true
		}
	}
	return false
}

// queueWrite parks a write for the next transaction.
func (l *loop) queueWrite(e pending, body, name []byte) {
	e.extra = uint32(len(l.warena))
	e.bodyLen, e.nameLen = uint32(len(body)), uint32(len(name))
	l.warena = append(append(l.warena, body...), name...)
	l.wq = append(l.wq, e)
}

// flushWrites sends the queue as one transaction on a free write connection;
// with none free it stays queued, and the next commit finds it.
func (l *loop) flushWrites() {
	if len(l.wq) == 0 {
		return
	}
	var c *pgConn
	up := false
	for _, w := range l.writes {
		if w.fd >= 0 {
			up = true
			if w.outstanding() == 0 {
				c = w
				break
			}
		}
	}
	if !up {
		q := l.wq
		l.wq = nil
		for i := range q {
			l.fail(&q[i], "no database connection")
		}
		l.wq, l.warena = q[:0], l.warena[:0]
		return
	}
	if c == nil {
		return
	}
	c.lastRecv = l.now
	slices.SortFunc(l.wq, func(a, b pending) int {
		if a.kind != b.kind {
			return cmp.Compare(a.kind, b.kind)
		}
		if a.uid != b.uid {
			return cmp.Compare(a.uid, b.uid)
		}
		return cmp.Compare(a.id, b.id)
	})
	single := len(l.wq) == 1
	c.extra = append(c.extra[:0], l.warena...)
	for i := range l.wq {
		e := &l.wq[i]
		if e.kind == rLike {
			n := len(c.out)
			c.out = append(c.out, tmplLike...)
			binary.BigEndian.PutUint64(c.out[n+offLike[0]:], uint64(e.uid))
			binary.BigEndian.PutUint64(c.out[n+offLike[1]:], uint64(e.id))
		} else {
			c.out = appendCreate(c.out, e.uid, c.extra[e.extra:e.extra+e.bodyLen])
		}
		if single {
			e.flags |= pSolo
		}
		if e.flags&pSolo != 0 { // a replay keeps its own transaction
			e.flags |= pSyncEnd
			c.out = append(c.out, syncMsg...)
		} else if i == len(l.wq)-1 || l.wq[i+1].flags&pSolo != 0 {
			e.flags |= pSyncEnd
			c.out = append(c.out, syncMsg...)
		}
		c.push(*e)
	}
	l.wq, l.warena = l.wq[:0], l.warena[:0]
	l.markDirty(c)
}

// seal closes the current read batch with its Sync.
func (l *loop) seal(c *pgConn) {
	c.out = append(c.out, syncMsg...)
	c.ring[(c.tail-1)&uint32(len(c.ring)-1)].flags |= pSyncEnd
	c.unsynced = 0
	l.markDirty(c)
}

// flushPG runs once per loop iteration, after every event was handled.
func (l *loop) flushPG() {
	if c := l.batch; c != nil {
		if c.fd >= 0 && c.unsynced > 0 {
			l.seal(c)
		}
		l.batch = nil
	}
	l.flushWrites()
	for i, c := range l.dirty {
		l.dirty[i] = nil
		c.dirty = false
		if c.fd >= 0 {
			l.pgWrite(c)
		}
	}
	l.dirty = l.dirty[:0]
	for _, c := range l.pgs {
		if c.broken {
			l.pgLost(c, "write failed")
		}
	}
}

// pgWrite sends what the connection's buffer holds; a full socket arms
// EPOLLOUT for the rest.
func (l *loop) pgWrite(c *pgConn) {
	for c.outOff < len(c.out) {
		n, e := rawWrite(int32(c.fd), c.out[c.outOff:])
		if e == syscall.EAGAIN {
			if !c.epollOut {
				c.epollOut = true
				l.epollMod(int32(c.fd), unix.EPOLLIN|unix.EPOLLOUT|unix.EPOLLRDHUP)
			}
			return
		}
		if e != 0 {
			c.broken = true
			return
		}
		c.outOff += n
	}
	c.out = c.out[:0]
	c.outOff = 0
	if c.epollOut {
		c.epollOut = false
		l.epollMod(int32(c.fd), unix.EPOLLIN|unix.EPOLLRDHUP)
	}
}

// ---- answers ----

func (l *loop) pgEvent(c *pgConn, events uint32) {
	if events&unix.EPOLLOUT != 0 {
		l.pgWrite(c)
	}
	if events&(unix.EPOLLIN|unix.EPOLLRDHUP|unix.EPOLLHUP|unix.EPOLLERR) != 0 {
		l.pgReadable(c)
	}
}

// pgReadable handles every complete message one read delivers. A message cut
// by the end of the read waits in c.partial; rows are consumed as they arrive,
// so that tail is never more than one row.
func (l *loop) pgReadable(c *pgConn) {
	b := l.pgbuf
	have := copy(b, c.partial)
	c.partial = c.partial[:0]
	n, e := rawRead(int32(c.fd), b[have:])
	if e == syscall.EAGAIN {
		c.partial = append(c.partial, b[:have]...)
		return
	}
	if e != 0 || n == 0 {
		l.pgLost(c, "connection lost")
		return
	}
	c.lastRecv = l.now
	end := have + n
	pos := 0
	if c.writer {
		if pos = l.pgWriterMessages(c, b[:end]); pos < 0 {
			return
		}
		c.partial = append(c.partial, b[pos:end]...)
		return
	}
	for end-pos >= 5 {
		size := 1 + int(binary.BigEndian.Uint32(b[pos+1:]))
		if size < 5 || size > len(b) {
			l.pgLost(c, "bad message length")
			return
		}
		if pos+size > end {
			break
		}
		msg := b[pos+5 : pos+size]
		switch b[pos] {
		case 'D': // DataRow
			if !l.pgRow(c, msg) {
				l.pgLost(c, "bad data row")
				return
			}
		case 'C': // CommandComplete
			if c.outstanding() == 0 {
				l.pgLost(c, "unexpected completion")
				return
			}
			l.pgComplete(c, msg)
		case 'E': // ErrorResponse: fails the head and skips to the Sync
			if c.outstanding() == 0 {
				l.pgLost(c, "unexpected error: "+pgErrorOf(msg).Error())
				return
			}
			e := c.pop()
			c.rows = 0
			if e.flags&pSyncEnd == 0 {
				c.aborted = true
			}
			l.pgFailed(&e, pgErrorOf(msg))
		case 'Z': // ReadyForQuery
			for c.aborted { // statements after the failed one never ran
				if c.outstanding() == 0 {
					l.pgLost(c, "sync out of step")
					return
				}
				e := c.pop()
				c.aborted = e.flags&pSyncEnd == 0
				l.fail(&e, "statement skipped")
			}
		}
		pos += size
	}
	c.partial = append(c.partial, b[pos:end]...)
}

// pgWriterMessages handles a write connection's messages: results collect in
// the ring, and only ReadyForQuery, after the commit, releases responses.
// Returns how far it got, or -1 if the connection was lost.
func (l *loop) pgWriterMessages(c *pgConn, b []byte) int {
	pos := 0
	for len(b)-pos >= 5 {
		size := 1 + int(binary.BigEndian.Uint32(b[pos+1:]))
		if size < 5 || size > len(b)+pgBufSize {
			l.pgLost(c, "bad message length")
			return -1
		}
		if pos+size > len(b) {
			break
		}
		msg := b[pos+5 : pos+size]
		switch b[pos] {
		case 'D', 'C':
			if c.ack == c.tail {
				l.pgLost(c, "unexpected result")
				return -1
			}
			e := &c.ring[c.ack&uint32(len(c.ring)-1)]
			if b[pos] == 'C' {
				if e.kind == rLike && commandTagRows(trimNul(msg)) > 0 {
					e.flags |= pFirst
				}
				c.ack++
				break
			}
			id, i, ok := rowField(msg, 2)
			if !ok || len(id) != 8 {
				l.pgLost(c, "bad data row")
				return -1
			}
			ts, _, ok := rowField(msg, i)
			if !ok || len(ts) != 8 {
				l.pgLost(c, "bad data row")
				return -1
			}
			e.id, e.ts = int64(binary.BigEndian.Uint64(id)), int64(binary.BigEndian.Uint64(ts))
		case 'E':
			c.err = pgErrorOf(msg)
		case 'Z':
			if c.outstanding() == 0 {
				l.pgLost(c, "sync out of step")
				return -1
			}
			l.pgCommitted(c)
		}
		pos += size
	}
	return pos
}

// pgCommitted handles one transaction's end: answers it, or takes it apart.
func (l *loop) pgCommitted(c *pgConn) {
	if pe := c.err; pe != nil {
		c.err = nil
		if c.peek().flags&pSolo != 0 {
			e := c.pop()
			l.pgFailed(&e, pe)
		} else {
			for done := false; !done; {
				e := c.pop()
				done = e.flags&pSyncEnd != 0
				l.queueWrite(pending{id: e.id, uid: e.uid, fd: e.fd, gen: e.gen, kind: e.kind,
					flags: e.flags&^(pSyncEnd|pFirst) | pSolo},
					c.extra[e.extra:e.extra+e.bodyLen],
					c.extra[e.extra+e.bodyLen:e.extra+e.bodyLen+e.nameLen])
			}
		}
		c.ack = c.head
		return
	}
	for done := false; !done; {
		e := c.pop()
		done = e.flags&pSyncEnd != 0
		s := l.client(&e)
		if s == nil {
			continue
		}
		closeConn := e.flags&pClose != 0
		if e.kind == rLike {
			b := l.tmp[:hdrRoom]
			b = append(b, `{"liked":true,"already_liked":`...)
			code := 200
			if e.flags&pFirst != 0 {
				b = append(b, "false"...)
				code = 201
			} else {
				b = append(b, "true"...)
			}
			b = append(b, `,"post_id":`...)
			b = appendInt(b, e.id)
			b = append(b, '}')
			l.tmp = b
			l.finish(&e, s, frame(b, code, false, closeConn))
			continue
		}
		body := c.extra[e.extra : e.extra+e.bodyLen]
		name := c.extra[e.extra+e.bodyLen : e.extra+e.bodyLen+e.nameLen]
		b := append(l.tmp[:hdrRoom], `{"post":`...)
		b = appendPost(b, e.id, body, name, e.ts, 0)
		b = append(b, '}')
		l.tmp = b
		l.finish(&e, s, frame(b, 201, false, closeConn))
	}
	c.ack = c.head
}

// rowField slices the next field of a DataRow.
func rowField(msg []byte, i int) ([]byte, int, bool) {
	if i+4 > len(msg) {
		return nil, 0, false
	}
	n := int(int32(binary.BigEndian.Uint32(msg[i:])))
	i += 4
	if n < 0 || i+n > len(msg) {
		return nil, 0, false
	}
	return msg[i : i+n], i + n, true
}

// pgRow turns one DataRow into JSON in c.json, for the head statement.
func (l *loop) pgRow(c *pgConn, msg []byte) bool {
	if c.outstanding() == 0 {
		return false
	}
	e := c.peek()
	switch e.kind {
	case rHealth:
		c.rows++
	default: // rFeed, rGetPost
		id, i, ok := rowField(msg, 2)
		if !ok || len(id) != 8 {
			return false
		}
		body, i, ok := rowField(msg, i)
		if !ok {
			return false
		}
		ts, i, ok := rowField(msg, i)
		if !ok || len(ts) != 8 {
			return false
		}
		name, i, ok := rowField(msg, i)
		if !ok {
			return false
		}
		likes, _, ok := rowField(msg, i)
		if !ok || len(likes) != 4 {
			return false
		}
		b := c.json
		if c.rows > 0 {
			b = append(b, ',')
		} else if e.kind == rFeed {
			b = append(b[:hdrRoom], `{"posts":[`...)
			c.marks = c.marks[:0]
		} else {
			b = append(b[:hdrRoom], `{"post":`...)
			c.marks = c.marks[:0]
		}
		var m rowMark
		c.json = appendPostMarked(b, int64(binary.BigEndian.Uint64(id)), body, name,
			int64(binary.BigEndian.Uint64(ts)), int64(int32(binary.BigEndian.Uint32(likes))), &m, hdrRoom)
		c.marks = append(c.marks, m)
		c.rows++
	}
	return true
}

// pgComplete finishes the head statement and answers its client.
func (l *loop) pgComplete(c *pgConn, tag []byte) {
	e := c.pop()
	rows := c.rows
	c.rows = 0
	s := l.client(&e)
	if s == nil {
		return
	}
	closeConn := e.flags&pClose != 0
	switch e.kind {
	case rFeed:
		if rows == 0 {
			c.json = append(c.json[:hdrRoom], `{"posts":[`...)
		}
		c.json = append(c.json, ']', '}')
		if e.flags&pGzip != 0 && len(c.json)-hdrRoom >= gzipMinBytes {
			l.finish(&e, s, frame(l.gz.compress(c.json[hdrRoom:], c.marks), 200, true, closeConn))
			return
		}
		l.finish(&e, s, frame(c.json, 200, false, closeConn))
	case rGetPost:
		if rows == 0 {
			l.answer(&e, s, 404, errPostNotFoundB)
			return
		}
		c.json = append(c.json, '}')
		l.finish(&e, s, frame(c.json, 200, false, closeConn))
	case rHealth:
		l.tmp = appendHealthOK(l.tmp)
		l.finish(&e, s, frame(l.tmp, 200, false, closeConn))
	}
}

func trimNul(b []byte) []byte {
	if n := len(b); n > 0 && b[n-1] == 0 {
		return b[:n-1]
	}
	return b
}

// pgFailed answers a statement Postgres rejected: a like whose foreign key
// failed is the SPEC's 404, anything else is a 500.
func (l *loop) pgFailed(e *pending, pe *pgError) {
	s := l.client(e)
	if s == nil {
		return
	}
	if e.kind == rLike && pe.code == pgForeignKeyViolation {
		l.answer(e, s, 404, errPostNotFoundB)
		return
	}
	l.answerFailure(e, s, pe.Error())
}

// fail answers a statement that never produced a result.
func (l *loop) fail(e *pending, why string) {
	if s := l.client(e); s != nil {
		l.answerFailure(e, s, why)
	}
}

func (l *loop) answerFailure(e *pending, s *slot, why string) {
	if e.kind == rHealth {
		l.tmp = appendHealthDegraded(l.tmp, why)
		l.finish(e, s, frame(l.tmp, 503, false, e.flags&pClose != 0))
		return
	}
	l.answer(e, s, 500, errInternal)
}

// ---- connection lifecycle ----

// pgLost fails everything on a dead connection and starts its reconnect.
func (l *loop) pgLost(c *pgConn, why string) {
	if c.fd < 0 {
		return
	}
	slogWarn("postgres connection lost", "conn", c.idx, "why", why)
	l.slots[c.fd].kind = kindFree
	unix.Close(c.fd)
	c.fd = -1
	c.broken = false
	if l.batch == c {
		l.batch = nil
	}
	c.out, c.outOff, c.epollOut, c.unsynced = c.out[:0], 0, false, 0
	c.partial, c.rows, c.aborted = c.partial[:0], 0, false
	for c.outstanding() > 0 {
		e := c.pop()
		l.fail(&e, why)
	}
	c.ack, c.err = c.head, nil
	go l.redial(c.idx)
}

type dialed struct {
	idx int
	fd  int
}

// redial reconnects in the background, one attempt per second; the loop picks
// the descriptor up from l.dialedCh.
func (l *loop) redial(idx int) {
	for {
		time.Sleep(time.Second)
		fd, err := connectPG(l.dsn)
		if err == nil {
			l.dialedCh <- dialed{idx, fd}
			return
		}
		slogError("database connect", err, "conn", idx)
	}
}

// connectPG returns a ready, nonblocking descriptor owned by the caller.
func connectPG(dsn *pgDSN) (int, error) {
	conn, err := dialPostgres(dsn)
	if err != nil {
		return -1, err
	}
	defer conn.Close()
	raw, err := conn.(*net.TCPConn).SyscallConn()
	if err != nil {
		return -1, err
	}
	fd, derr := -1, error(nil)
	if err := raw.Control(func(s uintptr) {
		fd, derr = unix.FcntlInt(s, unix.F_DUPFD_CLOEXEC, 0)
	}); err != nil {
		return -1, err
	}
	if derr != nil {
		return -1, derr
	}
	return fd, unix.SetNonblock(fd, true)
}

// adopt puts a freshly dialed descriptor to work.
func (l *loop) adopt(d dialed) {
	c := l.pgs[d.idx]
	c.fd = d.fd
	c.lastRecv = l.now
	l.noteFd(int32(d.fd))
	s := &l.slots[d.fd]
	s.kind, s.aux = kindPG, uint16(d.idx)
	l.epollAdd(int32(d.fd), unix.EPOLLIN|unix.EPOLLRDHUP)
}

// pgWatchdog drops a connection that owes answers but has gone silent.
func (l *loop) pgWatchdog() {
	for _, c := range l.pgs {
		if c.fd >= 0 && c.outstanding() > 0 && l.now-c.lastRecv > 15 {
			l.pgLost(c, "no answer for 15 s")
		}
	}
}
