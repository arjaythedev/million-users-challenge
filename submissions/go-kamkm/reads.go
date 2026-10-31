package main

import (
	"context"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

const readBatchLimit = 64

type readJob struct {
	id    int64
	reply chan readResult
}
type readResult struct {
	posts []postJSON
	err   error
}
type readBatcher struct {
	db   *pgxpool.Pool
	jobs chan readJob
}

func newReadBatcher(db *pgxpool.Pool) *readBatcher {
	b := &readBatcher{db: db, jobs: make(chan readJob, 1024)}
	for i := 0; i < 8; i++ {
		go b.run()
	}
	return b
}
func (b *readBatcher) submit(ctx context.Context, id int64) readResult {
	job := readJob{id: id, reply: make(chan readResult, 1)}
	select {
	case b.jobs <- job:
	case <-ctx.Done():
		return readResult{err: ctx.Err()}
	}
	select {
	case result := <-job.reply:
		return result
	case <-ctx.Done():
		return readResult{err: ctx.Err()}
	}
}
func (b *readBatcher) run() {
	for first := range b.jobs {
		jobs := make([]readJob, 1, readBatchLimit)
		jobs[0] = first
	drain:
		for len(jobs) < readBatchLimit {
			select {
			case job := <-b.jobs:
				jobs = append(jobs, job)
			default:
				break drain
			}
		}
		b.process(jobs)
	}
}
func (b *readBatcher) process(jobs []readJob) {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	commands := &pgx.Batch{}
	for _, job := range jobs {
		if job.id == 0 {
			commands.Queue(postQuery + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
		} else {
			commands.Queue(postQuery+" WHERE p.id = $1", job.id)
		}
	}
	responses := b.db.SendBatch(ctx, commands)
	results := make([]readResult, len(jobs))
	for i, job := range jobs {
		rows, err := responses.Query()
		if err == nil {
			capacity := 1
			if job.id == 0 {
				capacity = feedLimit
			}
			results[i].posts = make([]postJSON, 0, capacity)
			for rows.Next() {
				post, scanErr := scanPost(rows)
				if scanErr != nil {
					err = scanErr
					break
				}
				results[i].posts = append(results[i].posts, post)
			}
			rows.Close()
			if err == nil {
				err = rows.Err()
			}
			if err == nil && job.id != 0 && len(results[i].posts) == 0 {
				err = pgx.ErrNoRows
			}
		}
		results[i].err = err
	}
	err := responses.Close()
	for i, job := range jobs {
		if err != nil && results[i].err == nil {
			results[i].err = err
		}
		job.reply <- results[i]
	}
}
