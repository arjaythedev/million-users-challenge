package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"errors"
	"io"
	"slices"
	"strconv"
	"time"

	"golang.org/x/sys/unix"
)

// A loop owns its PostgreSQL sessions and drives them from the same epoll set
// as its clients. Requests wait in two loop-local queues, reads and writes; a
// free session takes a share of a queue and sends it as one batch: one write
// of Bind/Execute pairs closed by one Sync, answered by one stream of replies
// that is parsed as it arrives.
//
// A session starts batches no closer together than its gather window. An idle
// session sends a lone request at once, so light load pays no delay; under
// load the requests of one window travel together, which is what keeps
// packets, system calls and wakeups per request low on both machines.

const (
	jobFeed uint8 = iota
	jobPost
	jobHealth
	jobCreate
	jobLike
)

// job is a request waiting for the database. It names its connection by
// descriptor and generation; the connection may be gone when the reply comes.
type job struct {
	fd      int32
	gen     uint32
	kind    uint8
	retried bool
	// id is the post to read or like.
	id   int64
	user int64
	name string
	text string
}

type jobQueue struct {
	items []job
	head  int
}

func (q *jobQueue) len() int   { return len(q.items) - q.head }
func (q *jobQueue) push(j job) { q.items = append(q.items, j) }

// take moves the first n jobs to dst.
func (q *jobQueue) take(dst []job, n int) []job {
	taken := q.items[q.head : q.head+n]
	dst = append(dst, taken...)
	clear(taken)
	q.head += n
	switch {
	case q.head == len(q.items):
		q.items, q.head = q.items[:0], 0
	case q.head >= 4096 && q.head >= len(q.items)/2:
		q.items = q.items[:copy(q.items, q.items[q.head:])]
		q.head = 0
	}
	return dst
}

// unshift puts jobs back at the front, ahead of everything queued since.
func (q *jobQueue) unshift(jobs []job) {
	merged := make([]job, 0, len(jobs)+q.len())
	merged = append(append(merged, jobs...), q.items[q.head:]...)
	q.items, q.head = merged, 0
}

type writeResult struct {
	postID        int64
	createdMicros int64
	inserted      bool
	done          bool
	notFound      bool
	err           error
}

const (
	// modeReads: queries under one Sync, each answered as its rows arrive.
	modeReads uint8 = iota
	// modeGroup: writes under one Sync, one implicit transaction and one
	// commit, answered together at ReadyForQuery.
	modeGroup
	// modeAlone: the writes of a failed group again, a Sync after each.
	modeAlone
)

var errDatabaseTimeout = errors.New("postgres: no reply within the database timeout")

// sessionPlan is how each loop lays out its sessions.
type sessionPlan struct {
	// readers and writers are per loop. With no writers the readers carry
	// writes too (a pool too small to give each loop two sessions).
	readers, writers int
	batch            int
	readWindow       int64
	writeWindow      int64
}

func (p *sessionPlan) sessions(l *loop) []*session {
	sessions := make([]*session, p.readers+p.writers)
	for i := range sessions {
		sessions[i] = &session{
			l:        l,
			index:    i,
			fd:       -1,
			reads:    i < p.readers,
			writes:   i >= p.readers || p.writers == 0,
			in:       make([]byte, wireReadBuffer),
			lastSend: -int64(time.Hour),
		}
	}
	return sessions
}

type session struct {
	l      *loop
	index  int
	reads  bool
	writes bool

	// fd is -1 while the session is not connected.
	fd      int
	dialing bool
	retryAt int64
	err     error

	busy     bool
	mode     uint8
	sentAt   int64
	lastSend int64
	out      []byte
	written  int
	flushing bool
	in       []byte
	r, w     int

	// The batch in flight; next is the job whose reply is being read.
	batch []job
	next  int
	// A read's JSON body as its rows arrive.
	body     []byte
	rows     int
	started  bool
	skipping bool
	// A group's results, held until its commit.
	results []writeResult
	failure *serverError
}

type dialResult struct {
	index int
	fd    int
	err   error
}

// dial connects in the background; the result comes back through the inbox.
func (l *loop) dial(s *session) {
	s.dialing = true
	connect, index := l.app.dial, s.index
	go func() {
		fd, err := connect()
		l.inboxLock.Lock()
		l.inbox = append(l.inbox, dialResult{index, fd, err})
		l.inboxLock.Unlock()
		l.wake()
	}()
}

func (s *session) dialed(result dialResult) {
	l := s.l
	s.dialing = false
	if result.err == nil && l.stopping {
		_ = unix.Close(result.fd)
		return
	}
	if result.err == nil {
		result.err = l.watch(result.fd, unix.EPOLLIN, int32(kindDatabase<<kindShift|s.index))
		if result.err != nil {
			_ = unix.Close(result.fd)
		}
	}
	if result.err != nil {
		s.err, s.retryAt = result.err, l.now+int64(redialInterval)
		return
	}
	s.fd, s.err, s.r, s.w = result.fd, nil, 0, 0
}

// databaseTimers redials lost sessions and enforces the database timeout. It
// returns how long the loop may sleep before a session needs it again.
func (l *loop) databaseTimers() int64 {
	wait := int64(time.Second)
	plan := &l.app.plan
	for _, s := range l.sessions {
		switch {
		case s.fd < 0:
			if s.dialing || l.stopping {
				continue
			}
			if l.now >= s.retryAt {
				l.dial(s)
			} else if until := s.retryAt - l.now; until < wait {
				wait = until
			}
		case s.busy:
			if l.now-s.sentAt >= int64(databaseTimeout) {
				s.lost(errDatabaseTimeout)
				wait = 0
			}
		default:
			// Free, with work queued: it is waiting for its window.
			until := wait
			if s.writes && l.writes.len() > 0 {
				until = min(until, s.lastSend+plan.writeWindow-l.now)
			}
			if s.reads && l.reads.len() > 0 {
				until = min(until, s.lastSend+plan.readWindow-l.now)
			}
			wait = until
		}
	}
	return wait
}

// pump hands queued work to the sessions that are free and past their window.
func (l *loop) pump() {
	if l.writes.len() > 0 {
		l.dispatch(&l.writes, true)
	}
	if l.reads.len() > 0 {
		l.dispatch(&l.reads, false)
	}
}

func (l *loop) dispatch(queue *jobQueue, writes bool) {
	plan := &l.app.plan
	window, limit := plan.readWindow, plan.batch
	if writes {
		window, limit = plan.writeWindow, writeBatchLimit
	}
	serves := func(s *session) bool { return writes && s.writes || !writes && s.reads }
	ready, connected, pending := 0, 0, false
	var failure error
	for _, s := range l.sessions {
		switch {
		case !serves(s):
		case s.fd < 0:
			pending = pending || s.dialing
			if s.err != nil {
				failure = s.err
			}
		default:
			connected++
			if !s.busy && l.now-s.lastSend >= window {
				ready++
			}
		}
	}
	if connected == 0 && !pending {
		// The database is unreachable and the next attempt is not due: fail
		// fast instead of holding requests.
		if failure == nil {
			failure = errors.New("postgres: not connected")
		}
		var one [1]job
		for queue.len() > 0 {
			j := &queue.take(one[:0], 1)[0]
			if writes {
				l.written(j, writeResult{err: failure})
			} else {
				l.failRead(j, failure)
			}
		}
		return
	}
	for _, s := range l.sessions {
		if ready == 0 || queue.len() == 0 {
			return
		}
		if !serves(s) || s.fd < 0 || s.busy || l.now-s.lastSend < window {
			continue
		}
		// Sessions that are free together share the queue, so the database
		// works on the batches in parallel.
		share := min((queue.len()+ready-1)/ready, limit)
		ready--
		s.batch = queue.take(s.batch[:0], share)
		if writes {
			s.sendGroup()
		} else {
			s.sendReads()
		}
	}
}

func (s *session) sendReads() {
	out := s.out[:0]
	for i := range s.batch {
		switch j := &s.batch[i]; j.kind {
		case jobFeed:
			out = append(out, feedMessage...)
		case jobHealth:
			out = append(out, healthMessage...)
		default:
			out = appendPostQuery(out, j.id)
		}
	}
	s.out = append(out, syncMessage...)
	s.started, s.rows, s.skipping = false, 0, false
	s.transmit(modeReads)
}

func appendWrite(out []byte, j *job) []byte {
	if j.kind == jobCreate {
		return appendCreateQuery(out, j.user, j.text)
	}
	return appendLikeQuery(out, j.user, j.id)
}

func (s *session) sendGroup() {
	// Every group takes its like rows in the same order. Two loops can then
	// insert the same (user, post) pair in concurrent transactions, where
	// the second waits for the first, without ever waiting on each other in
	// a cycle.
	slices.SortFunc(s.batch, func(a, b job) int {
		switch {
		case a.kind != b.kind:
			return int(a.kind) - int(b.kind)
		case a.user != b.user:
			return compareInt(a.user, b.user)
		}
		return compareInt(a.id, b.id)
	})
	out := s.out[:0]
	for i := range s.batch {
		out = appendWrite(out, &s.batch[i])
	}
	s.out = append(out, syncMessage...)
	s.transmit(modeGroup)
}

func compareInt(a, b int64) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

// sendAlone runs each write of a failed group as its own implicit
// transaction: the same statements, with a Sync after each.
func (s *session) sendAlone() {
	out := s.out[:0]
	for i := range s.batch {
		out = append(appendWrite(out, &s.batch[i]), syncMessage...)
	}
	s.out = out
	s.transmit(modeAlone)
}

func (s *session) transmit(mode uint8) {
	now := s.l.now
	s.busy, s.mode, s.next, s.failure = true, mode, 0, nil
	s.sentAt, s.lastSend, s.written = now, now, 0
	if mode != modeReads {
		if cap(s.results) < len(s.batch) {
			s.results = make([]writeResult, writeBatchLimit)
		}
		s.results = s.results[:len(s.batch)]
		clear(s.results)
	}
	s.flush()
}

// flush writes the batch, normally in one call.
func (s *session) flush() {
	for s.written < len(s.out) {
		n, errno := rawWrite(s.fd, s.out[s.written:])
		if n < 0 {
			if errno == unix.EINTR {
				continue
			}
			if errno == unix.EAGAIN {
				if !s.flushing {
					s.flushing = true
					s.rearm(unix.EPOLLIN | unix.EPOLLOUT)
				}
				return
			}
			s.lost(errno)
			return
		}
		s.written += n
	}
	if s.flushing {
		s.flushing = false
		s.rearm(unix.EPOLLIN)
	}
	if cap(s.out) > 1<<20 {
		s.out = nil
	}
}

func (s *session) rearm(events uint32) {
	event := unix.EpollEvent{Events: events, Fd: int32(s.fd), Pad: int32(kindDatabase<<kindShift | s.index)}
	_ = unix.EpollCtl(s.l.epfd, unix.EPOLL_CTL_MOD, s.fd, &event)
}

func (s *session) event(fd int, events uint32) {
	if s.fd != fd {
		return
	}
	if s.flushing && events&unix.EPOLLOUT != 0 {
		s.flush()
	}
	if s.fd >= 0 && events&^unix.EPOLLOUT != 0 {
		s.receive()
	}
}

func (s *session) receive() {
	for {
		if s.w == len(s.in) {
			if s.r > 0 {
				s.w = copy(s.in, s.in[s.r:s.w])
				s.r = 0
			} else {
				s.in = append(s.in, make([]byte, len(s.in))...)
			}
		}
		space := len(s.in) - s.w
		n, errno := rawRead(s.fd, s.in[s.w:])
		if n > 0 {
			s.w += n
			s.parse()
			if s.fd < 0 || n < space {
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
		if n == 0 {
			s.lost(io.ErrUnexpectedEOF)
		} else {
			s.lost(errno)
		}
		return
	}
}

// parse handles every complete message in the buffer.
func (s *session) parse() {
	for s.w-s.r >= 5 {
		size := int(binary.BigEndian.Uint32(s.in[s.r+1:])) - 4
		if size < 0 || size > maxWireMessage {
			s.lost(errProtocol)
			return
		}
		total := 5 + size
		if s.w-s.r < total {
			if total > len(s.in) {
				grown := make([]byte, total+wireReadBuffer)
				s.w = copy(grown, s.in[s.r:s.w])
				s.r, s.in = 0, grown
			}
			break
		}
		kind, body := s.in[s.r], s.in[s.r+5:s.r+total]
		s.r += total
		if kind == 'E' && endsSession(body) {
			// The server is closing the session (a shutdown, a terminated
			// backend): no statement failed, the connection did.
			s.lost(parseServerError(body))
			return
		}
		if !s.busy {
			// Notices and parameter changes may arrive between batches.
			continue
		}
		if s.mode == modeReads {
			s.readMessage(kind, body)
		} else {
			s.writeMessage(kind, body)
		}
		if s.fd < 0 {
			return
		}
	}
	switch {
	case s.r == s.w:
		s.r, s.w = 0, 0
	case s.r >= len(s.in)/2:
		s.w = copy(s.in, s.in[s.r:s.w])
		s.r = 0
	}
}

func (s *session) open(j *job) {
	if s.started {
		return
	}
	s.started = true
	if j.kind == jobFeed {
		s.body = append(s.body[:0], `{"posts":[`...)
	} else {
		s.body = append(s.body[:0], `{"post":`...)
	}
}

func (s *session) readMessage(kind byte, body []byte) {
	l := s.l
	switch kind {
	case 'D':
		if s.next >= len(s.batch) || s.skipping {
			s.lost(errProtocol)
			return
		}
		j := &s.batch[s.next]
		if j.kind == jobHealth {
			return
		}
		s.open(j)
		if s.rows > 0 {
			s.body = append(s.body, ',')
		}
		var ok bool
		if s.body, ok = appendPostRow(s.body, body); !ok {
			s.lost(errProtocol)
			return
		}
		s.rows++
	case 'C':
		if s.next >= len(s.batch) || s.skipping {
			s.lost(errProtocol)
			return
		}
		// Each read is answered as soon as its own rows are in.
		j := &s.batch[s.next]
		s.next++
		rows := s.rows
		switch {
		case j.kind == jobHealth:
			l.healthy(j)
		case j.kind == jobFeed:
			s.open(j)
			s.body = append(s.body, ']', '}')
			l.complete(j, 200, s.body)
		case rows == 0:
			l.complete(j, 404, bodyPostNotFound)
		default:
			s.body = append(s.body, '}')
			l.complete(j, 200, s.body)
		}
		s.started, s.rows = false, 0
	case 'E':
		// The statement failed. It alone is answered with the error: the
		// server skips the queries behind it up to the Sync, and those are
		// queued again.
		if s.next < len(s.batch) && !s.skipping {
			j := &s.batch[s.next]
			s.next++
			s.started, s.rows, s.skipping = false, 0, true
			l.failRead(j, parseServerError(body))
		}
	case 'Z':
		s.busy, s.skipping = false, false
		if s.next < len(s.batch) {
			l.reads.unshift(s.batch[s.next:])
		}
		s.release()
	}
}

func (s *session) writeMessage(kind byte, body []byte) {
	l := s.l
	switch kind {
	case 'D':
		if s.next >= len(s.batch) || s.batch[s.next].kind != jobCreate || len(body) < 2 || binary.BigEndian.Uint16(body) != 2 {
			s.lost(errProtocol)
			return
		}
		id, rest, ok := rowColumn(body[2:])
		created, _, ok2 := rowColumn(rest)
		postID, err := strconv.ParseInt(string(id), 10, 64)
		if !ok || !ok2 || len(created) != 8 || err != nil {
			s.lost(errProtocol)
			return
		}
		s.results[s.next].postID = postID
		s.results[s.next].createdMicros = int64(binary.BigEndian.Uint64(created)) + postgresEpochMicros
	case 'C':
		if s.next >= len(s.batch) {
			s.lost(errProtocol)
			return
		}
		// The tag of an INSERT is "INSERT 0 <rows>".
		s.results[s.next].inserted = !bytes.HasSuffix(body, []byte(" 0\x00"))
		s.results[s.next].done = true
		if s.mode == modeGroup {
			s.next++
		}
	case 'E':
		if s.failure == nil {
			s.failure = parseServerError(body)
		}
	case 'Z':
		// ReadyForQuery follows the commit: only now is anything answered. An
		// error raised by the commit itself arrives just before it.
		if s.mode == modeAlone {
			if s.next >= len(s.batch) {
				s.lost(errProtocol)
				return
			}
			j, result := &s.batch[s.next], s.results[s.next]
			switch {
			case s.failure != nil:
				result = classify(j, s.failure)
			case !result.done:
				s.lost(errProtocol)
				return
			}
			s.failure = nil
			s.next++
			last := s.next == len(s.batch)
			if last {
				s.busy = false
			}
			l.written(j, result)
			if last {
				s.release()
			}
			return
		}
		switch {
		case s.failure == nil && s.next == len(s.batch):
			s.busy = false
			for i := range s.batch {
				l.written(&s.batch[i], s.results[i])
			}
			s.release()
		case s.failure == nil:
			s.lost(errProtocol)
		case len(s.batch) == 1:
			s.busy = false
			l.written(&s.batch[0], classify(&s.batch[0], s.failure))
			s.release()
		default:
			// A server error aborted the implicit transaction and nothing in
			// it committed, so every write can run again on its own.
			s.sendAlone()
		}
	}
}

// release forgets a finished batch.
func (s *session) release() {
	if trip := s.l.now - s.sentAt; trip > s.l.slowTrip {
		s.l.slowTrip = trip
	}
	clear(s.batch)
	s.batch, s.failure = s.batch[:0], nil
}

// lost closes a session whose connection failed, timed out or broke protocol,
// and settles the batch it carried. Reads have no side effects and go back to
// the queue once. Writes may have committed before the failure: they are
// reported as failed and never replayed.
func (s *session) lost(err error) {
	l := s.l
	if s.fd >= 0 {
		_ = unix.Close(s.fd)
		s.fd = -1
	}
	s.err, s.flushing, s.r, s.w = err, false, 0, 0
	// An established session is replaced at once, so the work it carried
	// waits for the new one; only a failed dial backs off.
	if !l.stopping {
		l.dial(s)
	}
	if !s.busy {
		return
	}
	s.busy, s.started, s.rows, s.skipping = false, false, 0, false
	first := s.next
	if s.mode == modeGroup {
		first = 0
	}
	var again []job
	for i := first; i < len(s.batch); i++ {
		j := &s.batch[i]
		switch {
		case s.mode != modeReads:
			l.written(j, writeResult{err: err})
		case j.retried:
			l.failRead(j, err)
		default:
			j.retried = true
			again = append(again, *j)
		}
	}
	if len(again) > 0 {
		l.reads.unshift(again)
	}
	s.release()
}

// classify turns a statement error into a result. A like of a post that does
// not exist violates the likes foreign key.
func classify(j *job, failure *serverError) writeResult {
	if j.kind == jobLike && failure.code == "23503" && failure.constraint == "likes_post_id_fkey" {
		return writeResult{notFound: true, err: failure}
	}
	return writeResult{err: failure}
}

// written answers a write once its transaction has committed, or failed.
func (l *loop) written(j *job, result writeResult) {
	if j.kind == jobLike && result.notFound {
		l.complete(j, 404, bodyPostNotFound)
		return
	}
	if result.err != nil {
		l.complete(j, 500, bodyInternal)
		return
	}
	b := l.body[:0]
	status := 201
	if j.kind == jobCreate {
		b = append(b, `{"post":`...)
		b = appendPost(b, result.postID, j.text, result.createdMicros, j.name, 0)
		b = append(b, '}')
	} else {
		b = append(b, `{"liked":true,"already_liked":`...)
		if result.inserted {
			b = append(b, "false"...)
		} else {
			status = 200
			b = append(b, "true"...)
		}
		b = append(b, `,"post_id":`...)
		b = strconv.AppendInt(b, j.id, 10)
		b = append(b, '}')
	}
	l.body = b
	l.complete(j, status, b)
}

// failRead answers a read the database could not serve.
func (l *loop) failRead(j *job, err error) {
	if j.kind != jobHealth {
		l.complete(j, 500, bodyInternal)
		return
	}
	body, _ := json.Marshal(struct {
		Status string `json:"status"`
		DB     string `json:"db"`
		Error  string `json:"error"`
	}{"degraded", "unreachable", err.Error()})
	l.complete(j, 503, body)
}

// healthy answers a health check whose SELECT 1 came back.
func (l *loop) healthy(j *job) {
	b := append(l.body[:0], `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(l.app.started)/time.Second), 10)
	l.body = append(b, '}')
	l.complete(j, 200, l.body)
}
