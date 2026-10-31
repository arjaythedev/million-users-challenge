package main

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

const writeBatchLimit = 64

type writeKind uint8

const (
	writeCreate writeKind = iota
	writeLike
)

type writeJob struct {
	kind     writeKind
	userID   int64
	username string
	body     string
	postID   int64
	result   chan writeResult
}

type writeResult struct {
	post     postJSON
	inserted bool
	notFound bool
	err      error
}

type writeBatcher struct {
	db   *pgxpool.Pool
	jobs chan writeJob
	done chan struct{}
	stop chan struct{}
}

func newWriteBatcher(db *pgxpool.Pool) *writeBatcher {
	b := &writeBatcher{db: db, jobs: make(chan writeJob, writeBatchLimit*4), done: make(chan struct{}), stop: make(chan struct{})}
	go b.run()
	return b
}

func (b *writeBatcher) submit(ctx context.Context, job writeJob) writeResult {
	job.result = make(chan writeResult, 1)
	select {
	case b.jobs <- job:
	case <-ctx.Done():
		return writeResult{err: ctx.Err()}
	case <-b.stop:
		return writeResult{err: context.Canceled}
	}
	select {
	case result := <-job.result:
		return result
	case <-ctx.Done():
		return writeResult{err: ctx.Err()}
	case <-b.stop:
		return writeResult{err: context.Canceled}
	}
}

func (b *writeBatcher) run() {
	defer close(b.done)
	// Reserve the writer's connection while it is healthy. Read traffic cannot
	// put every write batch at the back of the shared pool's acquisition queue.
	var conn *pgxpool.Conn
	defer func() {
		if conn != nil {
			conn.Release()
		}
	}()
	for {
		var first writeJob
		select {
		case first = <-b.jobs:
		case <-b.stop:
			return
		}
		batch := make([]writeJob, 1, writeBatchLimit)
		batch[0] = first
		timer := time.NewTimer(time.Millisecond)
	collect:
		for len(batch) < writeBatchLimit {
			select {
			case job, open := <-b.jobs:
				if !open {
					break collect
				}
				batch = append(batch, job)
			case <-timer.C:
				break collect
			}
		}
		if !timer.Stop() {
			select {
			case <-timer.C:
			default:
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
				for _, job := range batch {
					job.result <- writeResult{err: err}
				}
				continue
			}
		}
		b.process(conn, batch)
		if b.db.Config().MaxConns == 1 {
			conn.Release()
			conn = nil
		}
	}
}

// Stop only after the HTTP servers have drained their handlers.
func (b *writeBatcher) close() {
	close(b.stop)
	<-b.done
}

func (b *writeBatcher) process(conn *pgxpool.Conn, batch []writeJob) {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	tx, err := conn.Begin(ctx)
	if err != nil {
		for _, job := range batch {
			b.processOne(conn, job)
		}
		return
	}
	results := make([]writeResult, len(batch))
	commands := &pgx.Batch{}
	for _, job := range batch {
		switch job.kind {
		case writeCreate:
			commands.Queue("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", job.userID, job.body)
		case writeLike:
			commands.Queue("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", job.userID, job.postID)
		}
	}
	responses := tx.SendBatch(ctx, commands)
	for i, job := range batch {
		if job.kind == writeCreate {
			var created time.Time
			results[i].post.Body, results[i].post.Author = job.body, job.username
			err = responses.QueryRow().Scan(&results[i].post.ID, &created)
			results[i].post.CreatedAt = formatCreated(created)
		} else {
			var tag pgconn.CommandTag
			tag, err = responses.Exec()
			results[i].inserted = tag.RowsAffected() != 0
		}
		if err != nil {
			break
		}
	}
	if closeErr := responses.Close(); err == nil {
		err = closeErr
	}
	if err == nil {
		// A lost COMMIT acknowledgement may follow a successful commit.
		// Return the error without replaying writes whose outcome is unknown.
		err = tx.Commit(ctx)
		for i, job := range batch {
			results[i].err = err
			job.result <- results[i]
		}
		return
	}
	_ = tx.Rollback(ctx)
	for _, job := range batch {
		b.processOne(conn, job)
	}
}

func (b *writeBatcher) processOne(conn *pgxpool.Conn, job writeJob) {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	tx, err := conn.Begin(ctx)
	if err != nil {
		job.result <- writeResult{err: err}
		return
	}
	result, err := executeWrite(ctx, tx, job)
	if err == nil {
		err = tx.Commit(ctx)
	}
	if err != nil {
		_ = tx.Rollback(ctx)
		result.err = err
	}
	job.result <- result
}

func executeWrite(ctx context.Context, tx pgx.Tx, job writeJob) (writeResult, error) {
	switch job.kind {
	case writeCreate:
		var result writeResult
		result.post.Body, result.post.Author = job.body, job.username
		var created time.Time
		err := tx.QueryRow(ctx, "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", job.userID, job.body).Scan(&result.post.ID, &created)
		if err != nil {
			return result, err
		}
		result.post.CreatedAt = formatCreated(created)
		return result, nil
	case writeLike:
		tag, err := tx.Exec(ctx, "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", job.userID, job.postID)
		if err != nil {
			var pgError *pgconn.PgError
			if errors.As(err, &pgError) && pgError.Code == "23503" && pgError.ConstraintName == "likes_post_id_fkey" {
				return writeResult{notFound: true}, err
			}
			return writeResult{}, err
		}
		return writeResult{inserted: tag.RowsAffected() != 0}, nil
	default:
		return writeResult{}, errors.New("unknown write kind")
	}
}
