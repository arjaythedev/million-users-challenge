package main

import (
	"context"
	"errors"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	writeBatchLimit = 64
	createSQL       = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
	likeSQL         = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
)

type writeKind uint8

const (
	writeCreate writeKind = iota
	writeLike
)

type writeJob struct {
	kind   writeKind
	userID int64
	body   string
	postID int64
	result chan writeResult
}

type writeResult struct {
	postID        int64
	createdMicros int64
	inserted      bool
	notFound      bool
	err           error
}

// writeBatcher runs a few independent group-commit lanes. A lane sends its
// queued writes as one pipeline with a single Sync, which PostgreSQL runs as
// one implicit transaction: one round trip and one commit for the group.
// Likes are routed by (user, post), so the same pair is always serialized in
// one lane and concurrent lanes never wait on each other's uncommitted rows.
type writeBatcher struct {
	db      *pgxpool.Pool
	lanes   []chan writeJob
	reserve bool
	stop    chan struct{}
	done    sync.WaitGroup
}

var writeReplies = sync.Pool{New: func() any { return make(chan writeResult, 1) }}

func newWriteBatcher(db *pgxpool.Pool, lanes int, reserve bool) *writeBatcher {
	b := &writeBatcher{db: db, lanes: make([]chan writeJob, lanes), reserve: reserve, stop: make(chan struct{})}
	for i := range b.lanes {
		b.lanes[i] = make(chan writeJob, writeBatchLimit*4)
		b.done.Add(1)
		go b.run(b.lanes[i])
	}
	return b
}

func (b *writeBatcher) lane(job writeJob) chan writeJob {
	key := uint64(job.userID) * 0x9e3779b97f4a7c15
	if job.kind == writeLike {
		key ^= uint64(job.postID) * 0xc2b2ae3d27d4eb4f
	}
	key ^= key >> 29
	return b.lanes[key%uint64(len(b.lanes))]
}

// submit always receives a result: lanes answer every accepted job, and every
// database call runs under the database timeout.
func (b *writeBatcher) submit(job writeJob) writeResult {
	reply := writeReplies.Get().(chan writeResult)
	job.result = reply
	select {
	case b.lane(job) <- job:
	case <-b.stop:
		return writeResult{err: context.Canceled}
	}
	result := <-reply
	writeReplies.Put(reply)
	return result
}

func (b *writeBatcher) run(jobs chan writeJob) {
	defer b.done.Done()
	// A reserved connection keeps commits from queueing behind read batches
	// in the shared pool.
	var conn *pgxpool.Conn
	defer func() {
		if conn != nil {
			conn.Release()
		}
	}()
	batch := make([]writeJob, 0, writeBatchLimit)
	for {
		var first writeJob
		select {
		case first = <-jobs:
		case <-b.stop:
			return
		}
		// Group commit: take whatever queued while the previous group was
		// committing. No timer is needed; under load the queue fills itself.
		batch = append(batch[:0], first)
	collect:
		for len(batch) < writeBatchLimit {
			select {
			case job := <-jobs:
				batch = append(batch, job)
			default:
				break collect
			}
		}
		if conn == nil || conn.Conn().IsClosed() {
			if conn != nil {
				conn.Release()
			}
			ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
			var err error
			conn, err = b.db.Acquire(ctx)
			cancel()
			if err != nil {
				conn = nil
				for _, job := range batch {
					job.result <- writeResult{err: err}
				}
				continue
			}
		}
		b.process(conn.Conn(), batch)
		if !b.reserve {
			conn.Release()
			conn = nil
		}
		clear(batch)
	}
}

// Stop only after the HTTP servers have drained their handlers.
func (b *writeBatcher) close() {
	close(b.stop)
	b.done.Wait()
}

func (b *writeBatcher) process(conn *pgx.Conn, batch []writeJob) {
	if len(batch) == 1 {
		batch[0].result <- executeWrite(conn, batch[0])
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	commands := &pgx.Batch{QueuedQueries: make([]*pgx.QueuedQuery, 0, len(batch))}
	for _, job := range batch {
		if job.kind == writeCreate {
			commands.Queue(createSQL, job.userID, job.body)
		} else {
			commands.Queue(likeSQL, job.userID, job.postID)
		}
	}
	results := make([]writeResult, len(batch))
	responses := conn.SendBatch(ctx, commands)
	var err error
	for i, job := range batch {
		if job.kind == writeCreate {
			var created time.Time
			err = responses.QueryRow().Scan(&results[i].postID, &created)
			results[i].createdMicros = created.UnixMicro()
		} else {
			var tag pgconn.CommandTag
			tag, err = responses.Exec()
			results[i].inserted = tag.RowsAffected() != 0
		}
		if err != nil {
			break
		}
	}
	// Close reads through ReadyForQuery, which PostgreSQL sends only after the
	// implicit transaction has committed.
	if closeErr := responses.Close(); err == nil {
		err = closeErr
	}
	if err == nil {
		for i, job := range batch {
			job.result <- results[i]
		}
		return
	}
	// A server error means the implicit transaction was aborted and nothing in
	// it committed, so each write can be retried alone. Any other failure (a
	// lost connection or timeout) may have happened after COMMIT; those writes
	// are reported as failed and never replayed.
	var serverError *pgconn.PgError
	if !errors.As(err, &serverError) || conn.IsClosed() {
		for _, job := range batch {
			job.result <- writeResult{err: err}
		}
		return
	}
	for _, job := range batch {
		job.result <- executeWrite(conn, job)
	}
}

// executeWrite runs one statement in its own implicit transaction.
func executeWrite(conn *pgx.Conn, job writeJob) writeResult {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	switch job.kind {
	case writeCreate:
		var result writeResult
		var created time.Time
		if err := conn.QueryRow(ctx, createSQL, job.userID, job.body).Scan(&result.postID, &created); err != nil {
			return writeResult{err: err}
		}
		result.createdMicros = created.UnixMicro()
		return result
	case writeLike:
		tag, err := conn.Exec(ctx, likeSQL, job.userID, job.postID)
		if err != nil {
			var serverError *pgconn.PgError
			if errors.As(err, &serverError) && serverError.Code == "23503" && serverError.ConstraintName == "likes_post_id_fkey" {
				return writeResult{notFound: true, err: err}
			}
			return writeResult{err: err}
		}
		return writeResult{inserted: tag.RowsAffected() != 0}
	default:
		return writeResult{err: errUnknownWrite}
	}
}
