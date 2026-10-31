package main

import (
	"context"
	"encoding/binary"
	"errors"
	"log"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
)

// The SPEC's statements, prepared once per connection. Every request runs exactly one of them.
const (
	stmtHealth = iota
	stmtFeed
	stmtPost
	stmtInsertPost
	stmtLike
	stmtCount
)

var stmtSQL = [stmtCount]string{
	stmtHealth:     `SELECT 1`,
	stmtFeed:       postSelect + ` ORDER BY p.created_at DESC, p.id DESC LIMIT $1`,
	stmtPost:       postSelect + ` WHERE p.id = $1`,
	stmtInsertPost: `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`,
	stmtLike:       `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`,
}

const (
	maxBatch = 256
	// A worker waits this long after its first queued job, so the next round trip serves
	// every request that arrived meanwhile: fewer writes, reads and ReadyForQuery messages
	// per request, for a few ms of latency (the limits are 500 ms p95 and 1 s p99).
	batchWindow = 2 * time.Millisecond
	// Postgres timestamps count microseconds from 2000-01-01 UTC.
	pgEpochUnixMicro = 946684800 * 1_000_000
)

var (
	binaryFormat = []int16{1}
	errNoRows    = errors.New("no rows")
)

// dbJob is one request's statement and, once done is closed, its result.
type dbJob struct {
	stmt  int
	a, b  int64
	body  string
	posts []post // stmtFeed: rows are appended here
	post  post   // stmtPost, stmtInsertPost
	rows  int64  // rows affected (stmtLike)
	err   error
	done  chan struct{}
}

// pipelineDB runs requests' statements over a fixed set of connections. Each connection's
// worker takes every job queued while it was busy and sends them in one pipelined write.
// Reads and writes use separate connections, so a commit never delays a read:
//   - a read batch shares one Sync: one implicit transaction and one ReadyForQuery;
//   - each write gets its own Sync, so it is its own transaction: a like on a missing post
//     fails alone, and every write is committed before its result is delivered.
type pipelineDB struct {
	cfg    *pgconn.Config
	reads  chan *dbJob
	writes chan *dbJob
	window time.Duration
}

func newPipelineDB(ctx context.Context, connString string, conns int, window time.Duration) (*pipelineDB, error) {
	cfg, err := pgconn.ParseConfig(connString)
	if err != nil {
		return nil, err
	}
	d := &pipelineDB{cfg: cfg, reads: make(chan *dbJob, 16384), writes: make(chan *dbJob, 16384), window: window}
	writers := max(1, conns/4)
	for i := range max(2, conns) {
		conn, sds, err := d.connect(ctx)
		if err != nil {
			return nil, err
		}
		if i < writers {
			go d.worker(conn, sds, d.writes, false)
		} else {
			go d.worker(conn, sds, d.reads, true)
		}
	}
	return d, nil
}

func (d *pipelineDB) connect(ctx context.Context) (*pgconn.PgConn, []*pgconn.StatementDescription, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	conn, err := pgconn.ConnectConfig(ctx, d.cfg)
	if err != nil {
		return nil, nil, err
	}
	sds := make([]*pgconn.StatementDescription, stmtCount)
	for i, sql := range stmtSQL {
		if sds[i], err = conn.Prepare(ctx, "s"+string(rune('0'+i)), sql, nil); err != nil {
			conn.Close(context.Background())
			return nil, nil, err
		}
	}
	return conn, sds, nil
}

// do queues the job and waits for its result or the context's deadline.
func (d *pipelineDB) do(ctx context.Context, j *dbJob) error {
	// done is kept locally: the worker owns j from the send until it closes done.
	done := make(chan struct{})
	j.done = done
	queue := d.reads
	if j.stmt == stmtInsertPost || j.stmt == stmtLike {
		queue = d.writes
	}
	select {
	case queue <- j:
	case <-ctx.Done():
		return ctx.Err()
	}
	select {
	case <-done:
		return j.err
	case <-ctx.Done():
		return ctx.Err()
	}
}

func (d *pipelineDB) worker(conn *pgconn.PgConn, sds []*pgconn.StatementDescription, jobs chan *dbJob, sharedSync bool) {
	batch := make([]*dbJob, 0, maxBatch)
	for {
		batch = append(batch[:0], <-jobs)
		if d.window > 0 {
			// Let more requests arrive: one round trip then serves all of them.
			time.Sleep(d.window)
		}
	drain:
		for len(batch) < maxBatch {
			select {
			case j := <-jobs:
				batch = append(batch, j)
			default:
				break drain
			}
		}
		if n, err := runBatch(conn, sds, batch, sharedSync); err != nil {
			// The connection is unusable: fail what is left of the batch, then reconnect.
			log.Printf("[db] %v", err)
			for _, j := range batch[n:] {
				j.err = err
				close(j.done)
			}
			conn.Close(context.Background())
			for {
				var cerr error
				if conn, sds, cerr = d.connect(context.Background()); cerr == nil {
					break
				}
				log.Printf("[db] reconnect: %v", cerr)
				time.Sleep(time.Second)
			}
		}
	}
}

// runBatch returns how many jobs of the batch it completed, and a connection-level error.
// With sharedSync the batch is one implicit transaction, so any error ends the batch.
func runBatch(conn *pgconn.PgConn, sds []*pgconn.StatementDescription, batch []*dbJob, sharedSync bool) (int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), dbTimeout)
	defer cancel()
	p := conn.StartPipeline(ctx)
	var params [2][8]byte
	for _, j := range batch {
		var values [][]byte
		switch j.stmt {
		case stmtHealth:
		case stmtFeed, stmtPost:
			binary.BigEndian.PutUint64(params[0][:], uint64(j.a))
			values = [][]byte{params[0][:]}
		case stmtInsertPost:
			binary.BigEndian.PutUint64(params[0][:], uint64(j.a))
			values = [][]byte{params[0][:], []byte(j.body)}
		case stmtLike:
			binary.BigEndian.PutUint64(params[0][:], uint64(j.a))
			binary.BigEndian.PutUint64(params[1][:], uint64(j.b))
			values = [][]byte{params[0][:], params[1][:]}
		}
		// The parameters are copied into the send buffer here, so params can be reused.
		p.SendQueryStatement(sds[j.stmt], values, binaryFormat, binaryFormat)
		if !sharedSync {
			p.SendPipelineSync()
		}
	}
	if sharedSync {
		p.SendPipelineSync()
	}
	if err := p.Flush(); err != nil {
		return 0, err
	}
	for i, j := range batch {
		res, err := p.GetResults()
		if err == nil {
			rr, ok := res.(*pgconn.ResultReader)
			if !ok {
				return i, errors.New("pipeline: unexpected result")
			}
			err = readResult(rr, j)
		}
		var pgErr *pgconn.PgError
		if err != nil && !errors.Is(err, errNoRows) && (sharedSync || !errors.As(err, &pgErr)) {
			return i, err // connection-level failure, or an aborted shared transaction
		}
		j.err = err
		if !sharedSync {
			if _, err := p.GetResults(); err != nil { // this statement's Sync
				if !errors.As(err, &pgErr) {
					return i, err
				}
			}
		}
		close(j.done)
	}
	if sharedSync {
		if _, err := p.GetResults(); err != nil {
			return len(batch), err
		}
	}
	return len(batch), p.Close()
}

func readResult(rr *pgconn.ResultReader, j *dbJob) error {
	n := 0
	for rr.NextRow() {
		v := rr.Values()
		switch j.stmt {
		case stmtFeed:
			if n < cap(j.posts) {
				j.posts = append(j.posts, decodePost(v))
			}
		case stmtPost:
			j.post = decodePost(v)
		case stmtInsertPost:
			if len(v) == 2 && len(v[0]) == 8 && len(v[1]) == 8 {
				j.post.ID = int64(binary.BigEndian.Uint64(v[0]))
				j.post.CreatedAt = formatPgTime(v[1])
			}
		}
		n++
	}
	tag, err := rr.Close()
	if err != nil {
		return err
	}
	j.rows = tag.RowsAffected()
	if n == 0 && (j.stmt == stmtPost || j.stmt == stmtInsertPost) {
		return errNoRows
	}
	return nil
}

// decodePost reads a binary row of postSelect: int8, text, timestamptz, text, int4.
func decodePost(v [][]byte) post {
	if len(v) != 5 || len(v[0]) != 8 || len(v[2]) != 8 || len(v[4]) != 4 {
		return post{}
	}
	return post{
		ID:        int64(binary.BigEndian.Uint64(v[0])),
		Body:      string(v[1]),
		CreatedAt: formatPgTime(v[2]),
		Author:    string(v[3]),
		LikeCount: int64(int32(binary.BigEndian.Uint32(v[4]))),
	}
}

func formatPgTime(b []byte) string {
	us := int64(binary.BigEndian.Uint64(b))
	return formatTime(time.UnixMicro(us + pgEpochUnixMicro))
}
