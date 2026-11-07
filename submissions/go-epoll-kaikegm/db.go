package main

import (
	"context"
	"encoding/binary"
	"errors"
	"log"
	"strconv"
	"syscall"
	"time"
	"unsafe"

	"github.com/jackc/pgx/v5/pgconn"
	"golang.org/x/sys/unix"
)

const postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p
  JOIN users u ON u.id = p.user_id`

const feedSelect = postSelect + ` ORDER BY p.created_at DESC, p.id DESC LIMIT 20`

const getPostSelect = postSelect + ` WHERE p.id = $1`
const createPostQuery = `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`
const likePostQuery = `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`

// job is a request waiting for its statement inside the loop. It holds no pointers into the read buffer.
type job struct {
	stmt     uint8
	gzip     bool
	close    bool
	solo     bool // a write sent again after its batch failed: runs in its own transaction
	fd       int32
	gen      uint32
	a, b     int64  // statement parameters (userID, postID)
	body     string // stmtInsertPost: trimmed body
	username string // stmtInsertPost: token username
	start    int64  // loop monotonic clock (ns) when handed to Postgres
}

type pgItem struct {
	sync bool
	j    job
}

type heldResp struct {
	j        job
	status   int
	from, to int // slice in heldBody
}

// pgc is a Postgres connection owned by one loop after pgconn connected and prepared it.
type pgc struct {
	fd       int
	idx      int
	write    bool
	dead     bool
	inBatch  bool // statements queued without their Sync yet
	dirty    bool
	wantOut  bool
	out      []byte
	in       []byte
	q        []pgItem
	qhead    int
	inflight int
	rows     int
	body     []byte // response JSON being built for the job at the head of q
	// Write responses held from their CommandComplete until the ReadyForQuery after
	// their batch's Sync: the implicit transaction commits at the Sync (Rule 8).
	held     []heldResp
	heldBody []byte
}

type revived struct {
	fd    int
	write bool
	idx   int
}

// connectPG connects, authenticates and prepares the 5 SPEC statements with pgconn,
// then takes the raw socket over so the loop speaks the extended wire protocol directly.
func connectPG(ctx context.Context, connStr string) (int, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()

	conn, err := pgconn.Connect(ctx, connStr)
	if err != nil {
		return -1, err
	}
	// Otimização: jit, paralelismo, seqscan e memoize desligados para forçar plano de índice direto e economizar CPU no PG.
	_ = conn.Exec(ctx, "SET jit = off; SET max_parallel_workers_per_gather = 0; SET enable_seqscan = off; SET enable_memoize = off;").Close()
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
	setThinTimeouts(fd)
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1)
	if err := unix.SetNonblock(fd, true); err != nil {
		unix.Close(fd)
		return -1, err
	}
	return fd, nil
}

func (l *Worker) attachPG(c *pgc, fd int) error {
	if fd >= len(slots) {
		unix.Close(fd)
		return errors.New("descriptor table full")
	}
	c.fd, c.dead = fd, false
	c.out, c.in, c.q, c.qhead, c.inflight = c.out[:0], c.in[:0], c.q[:0], 0, 0
	c.inBatch, c.dirty, c.wantOut = false, false, false
	c.held, c.heldBody = c.held[:0], c.heldBody[:0]
	slots[fd] = slot{gen: slots[fd].gen, loop: uint16(l.id), kind: kindPG}
	l.byFd[int32(fd)] = c
	return unix.EpollCtl(l.epollFd, unix.EPOLL_CTL_ADD, fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLET, Fd: int32(fd)})
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
		c.q, c.qhead = c.q[:copy(c.q, c.q[c.qhead:])], 0
	}
}

func leastLoaded(cs []*pgc, rr *int) *pgc {
	var best *pgc
	n := len(cs)
	if n == 0 {
		return nil
	}
	*rr++
	start := *rr
	for i := 0; i < n; i++ {
		c := cs[(start+i)%n]
		if !c.dead {
			if c.inflight == 0 {
				return c
			}
			if best == nil || c.inflight < best.inflight {
				best = c
			}
		}
	}
	return best
}

// dispatch queues the job's statement directly onto a Postgres connection;
// it is sent at the end of the iteration. Up to l.dynReadCap reads in a pass
// share one connection and one Sync before rotating to the next least-loaded connection,
// eliminating head-of-line blocking on large passes while preserving pipelining.
func (l *Worker) dispatch(j *job) {
	var c *pgc
	if isWriteStmt(j.stmt) {
		if l.curW == nil || l.curW.dead {
			l.curW = leastLoaded(l.writers, &l.rrWrite)
		}
		c = l.curW
	} else {
		if l.cur == nil || l.cur.dead || l.curReads >= l.dynReadCap {
			l.cur = leastLoaded(l.readers, &l.rrRead)
			l.curReads = 0
		}
		c = l.cur
		l.curReads++
	}
	if c == nil {
		l.fail(j, "no database connection")
		return
	}
	if j.solo && c.inBatch {
		// Close the open batch first, so the retried write gets a transaction of its own.
		c.out = appendSync(c.out)
		c.q = append(c.q, pgItem{sync: true})
		c.inBatch = false
	}
	var p0 [8]byte
	switch j.stmt {
	case stmtHealth:
		c.out = append(c.out, wireHealth...)
	case stmtFeed:
		c.out = append(c.out, wireFeed...)
	case stmtPost:
		off := len(c.out)
		c.out = append(c.out, wirePost...)
		binary.BigEndian.PutUint64(c.out[off+19:], uint64(j.a))
	case stmtInsertPost:
		binary.BigEndian.PutUint64(p0[:], uint64(j.a))
		c.out = appendBindExecute(c.out, j.stmt, p0[:], unsafe.Slice(unsafe.StringData(j.body), len(j.body)))
	case stmtLike:
		off := len(c.out)
		c.out = append(c.out, wireLike...)
		binary.BigEndian.PutUint64(c.out[off+19:], uint64(j.a))
		binary.BigEndian.PutUint64(c.out[off+31:], uint64(j.b))
	}
	c.q = append(c.q, pgItem{j: *j})
	c.inflight++
	if j.solo {
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

func (l *Worker) flushBatches() {
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
	l.cur, l.curW, l.curReads = nil, nil, 0
}

func (l *Worker) flushPG(c *pgc) {
	for len(c.out) > 0 {
		n, e := rawWrite(c.fd, c.out)
		if e == unix.EINTR {
			continue
		}
		if e == unix.EAGAIN {
			if !c.wantOut {
				c.wantOut = true
				_ = unix.EpollCtl(l.epollFd, unix.EPOLL_CTL_MOD, c.fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLOUT | unix.EPOLLET, Fd: int32(c.fd)})
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
		_ = unix.EpollCtl(l.epollFd, unix.EPOLL_CTL_MOD, c.fd, &unix.EpollEvent{Events: unix.EPOLLIN | unix.EPOLLET, Fd: int32(c.fd)})
	}
}

func (l *Worker) readPG(c *pgc) {
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

func (l *Worker) handlePG(c *pgc, typ byte, payload []byte) bool {
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
				l.finish(&j, 404, errBodyPostNotFound)
			default:
				log.Printf("[db] %s: %s", code, msg)
				l.fail(&j, msg)
			}
			// Postgres rolls the implicit batch transaction back and skips the rest of it until its Sync.
			// If an open batch was queued after the failed batch without a Sync yet, close it first
			// so the Sync queue stays aligned with the backend's ReadyForQuery messages.
			if c.inBatch {
				c.out = appendSync(c.out)
				c.q = append(c.q, pgItem{sync: true})
				c.inBatch = false
			}
			// Replay the other writes of the aborted batch in their own solo transactions (Rule 8).
			retry := c.write
			held := c.held
			c.held, c.heldBody = nil, nil
			for i := range held {
				held[i].j.solo = true
				l.dispatch(&held[i].j)
			}
			for it := c.front(); it != nil && !it.sync; it = c.front() {
				j := it.j
				c.inflight--
				c.pop()
				if retry {
					j.solo = true
					l.dispatch(&j)
				} else {
					l.fail(&j, "transaction aborted")
				}
			}
		} else if len(c.held) > 0 {
			log.Printf("[db] commit: %s: %s", code, msg)
			l.failHeld(c, msg)
		}
	case 'Z':
		if it == nil || !it.sync {
			l.killPG(c, errors.New("unexpected ReadyForQuery"))
			return false
		}
		c.pop()
		// Rule 8: implicit batch transaction is now committed at ReadyForQuery; release held write responses.
		for i := range c.held {
			h := &c.held[i]
			l.finish(&h.j, h.status, c.heldBody[h.from:h.to])
		}
		c.held, c.heldBody = c.held[:0], c.heldBody[:0]
	}
	return true
}

func (l *Worker) row(c *pgc, j *job, cols [][]byte) {
	switch j.stmt {
	case stmtFeed, stmtPost:
		if len(cols) != 5 || len(cols[0]) == 0 || len(cols[2]) != 8 || len(cols[4]) == 0 {
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
		c.body = appendWirePost(c.body, cols[0], cols[1], int64(binary.BigEndian.Uint64(cols[2])), cols[3], cols[4])
	case stmtInsertPost:
		if len(cols) != 2 || len(cols[0]) == 0 || len(cols[1]) != 8 {
			return
		}
		c.body = append(c.body[:0], `{"post":`...)
		c.body = appendWirePost(c.body, cols[0], unsafe.Slice(unsafe.StringData(j.body), len(j.body)),
			int64(binary.BigEndian.Uint64(cols[1])), unsafe.Slice(unsafe.StringData(j.username), len(j.username)), zeroLikes)
	}
	c.rows++
}

func (l *Worker) complete(c *pgc, j *job, tag []byte) {
	rows := c.rows
	c.rows = 0
	switch j.stmt {
	case stmtHealth:
		c.body = formatHealthOK(c.body[:0], int64(time.Since(l.started).Seconds()))
		l.finish(j, 200, c.body)
	case stmtFeed:
		if rows == 0 {
			c.body = append(c.body[:0], `{"posts":[`...)
		}
		c.body = append(c.body, "]}"...)
		l.finish(j, 200, c.body)
	case stmtPost:
		if rows == 0 {
			l.finish(j, 404, errBodyPostNotFound)
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
		l.hold(c, j, 201, c.body)
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
		l.hold(c, j, status, c.body)
	}
	c.body = c.body[:0]
}

// hold keeps a write's response until its batch has committed at ReadyForQuery after Sync (Rule 8).
func (l *Worker) hold(c *pgc, j *job, status int, body []byte) {
	from := len(c.heldBody)
	c.heldBody = append(c.heldBody, body...)
	c.held = append(c.held, heldResp{j: *j, status: status, from: from, to: len(c.heldBody)})
}

func (l *Worker) failHeld(c *pgc, msg string) {
	held := c.held
	c.held, c.heldBody = nil, nil
	for i := range held {
		l.fail(&held[i].j, msg)
	}
}

func (l *Worker) fail(j *job, msg string) {
	if j.stmt == stmtHealth {
		b := formatHealthError(nil, msg)
		l.finish(j, 503, b)
		return
	}
	l.finish(j, 500, errBodyInternal)
}

func (l *Worker) finish(j *job, status int, body []byte) {
	fd := int(j.fd)
	s := &slots[fd]
	if s.kind != kindClient || s.gen != j.gen || s.loop != uint16(l.id) {
		return
	}

	// Update latency EWMA (alpha = 1/16)
	lat := l.now - j.start
	if lat > 0 {
		l.latEwma = l.latEwma - (l.latEwma >> 4) + (lat >> 4)
	}

	s.flags &^= flagBusy
	l.respond(fd, status, body, j.gzip, j.close)
	if slots[fd].kind != kindClient || slots[fd].gen != j.gen {
		return
	}
	if lat >= lateResponse {
		_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_QUICKACK, 0)
	}
	if pending, ok := l.inbuf[int32(fd)]; ok {
		delete(l.inbuf, int32(fd))
		l.processInput(fd, pending)
	}
}

func (l *Worker) killPG(c *pgc, cause error) {
	if c.dead {
		return
	}
	log.Printf("[db] connection lost: %v", cause)
	c.dead = true
	l.ndead++
	delete(l.byFd, int32(c.fd))
	slots[c.fd] = slot{gen: slots[c.fd].gen + 1}
	unix.Close(c.fd)
	l.failHeld(c, "database connection lost")
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

func (l *Worker) collectRevived() {
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
