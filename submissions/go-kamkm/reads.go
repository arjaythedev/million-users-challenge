package main

import (
	"context"
	"encoding/binary"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgtype"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	feedSQL = postQuery + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
	postSQL = postQuery + " WHERE p.id = $1"

	// PostgreSQL timestamps count microseconds from 2000-01-01 UTC.
	postgresEpochMicros = 946684800 * 1000000
)

// readJob is one queued read: the feed when id is 0, otherwise one post.
type readJob struct {
	id int64
	ex *exchange
}

type readResult struct {
	buffer   *[]byte
	notFound bool
	err      error
}

// readBatcher runs many independent pipelines. Each worker sends what is
// queued (up to its batch limit) as one pipelined round trip, so PostgreSQL
// executes several small batches in parallel instead of a few long serial
// ones. Under light load a batch holds a single request.
type readBatcher struct {
	db    *pgxpool.Pool
	jobs  chan readJob
	limit int
	// done completes a job's exchange with its result.
	done func(*exchange, readResult)
}

// readQueue bounds the reads waiting for a pipeline. Queued reads hold no
// response buffer, so a deep queue costs little memory; when it is full the
// event loops stop taking new requests until the pipelines catch up.
const readQueue = 65536

func newReadBatcher(db *pgxpool.Pool, workers, limit int, done func(*exchange, readResult)) *readBatcher {
	b := &readBatcher{db: db, jobs: make(chan readJob, readQueue), limit: limit, done: done}
	for i := 0; i < workers; i++ {
		go b.run()
	}
	return b
}

// enqueue queues a read without holding a response buffer. A worker always
// completes it: every batch runs under the database timeout.
func (b *readBatcher) enqueue(job readJob) { b.jobs <- job }

func (b *readBatcher) run() {
	jobs := make([]readJob, 0, b.limit)
	for first := range b.jobs {
		jobs = append(jobs[:0], first)
	drain:
		for len(jobs) < b.limit {
			select {
			case job := <-b.jobs:
				jobs = append(jobs, job)
			default:
				break drain
			}
		}
		b.process(jobs)
		clear(jobs)
	}
}

func (b *readBatcher) process(jobs []readJob) {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	commands := &pgx.Batch{QueuedQueries: make([]*pgx.QueuedQuery, 0, len(jobs))}
	for _, job := range jobs {
		if job.id == 0 {
			commands.Queue(feedSQL)
		} else {
			commands.Queue(postSQL, job.id)
		}
	}
	responses := b.db.SendBatch(ctx, commands)
	for _, job := range jobs {
		// Each read completes as soon as its own rows are in.
		b.done(job.ex, readOne(responses, job))
	}
	_ = responses.Close()
}

func readOne(responses pgx.BatchResults, job readJob) (result readResult) {
	rows, err := responses.Query()
	if err != nil {
		return readResult{err: err}
	}
	defer rows.Close()
	result.buffer = getBuffer()
	body := (*result.buffer)[:0]
	defer func() { *result.buffer = body }()
	raw := rawPostColumns(rows)
	if job.id == 0 {
		body = append(body, `{"posts":[`...)
	} else {
		body = append(body, `{"post":`...)
	}
	count := 0
	for rows.Next() {
		if count > 0 {
			body = append(body, ',')
		}
		if values := rows.RawValues(); raw && len(values[0]) == 8 && values[1] != nil && len(values[2]) == 8 && values[3] != nil && len(values[4]) == 4 {
			body = appendPost(body,
				int64(binary.BigEndian.Uint64(values[0])),
				values[1],
				int64(binary.BigEndian.Uint64(values[2]))+postgresEpochMicros,
				values[3],
				int64(int32(binary.BigEndian.Uint32(values[4]))))
		} else {
			var id, likes int64
			var text, author string
			var created time.Time
			if err := rows.Scan(&id, &text, &created, &author, &likes); err != nil {
				result.err = err
				return
			}
			body = appendPost(body, id, text, created.UnixMicro(), author, likes)
		}
		count++
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		result.err = err
		return
	}
	if job.id != 0 {
		if count == 0 {
			result.notFound = true
			return
		}
		body = append(body, '}')
		return
	}
	body = append(body, ']', '}')
	return
}

// rawPostColumns reports whether the row values are in the binary formats the
// raw decoder reads: int8 id, timestamptz created_at, int4 like_count, and
// text columns (whose text and binary encodings are the same bytes).
func rawPostColumns(rows pgx.Rows) bool {
	fields := rows.FieldDescriptions()
	if len(fields) != 5 {
		return false
	}
	return fields[0].DataTypeOID == pgtype.Int8OID && fields[0].Format == pgx.BinaryFormatCode &&
		fields[1].DataTypeOID == pgtype.TextOID &&
		fields[2].DataTypeOID == pgtype.TimestamptzOID && fields[2].Format == pgx.BinaryFormatCode &&
		fields[3].DataTypeOID == pgtype.TextOID &&
		fields[4].DataTypeOID == pgtype.Int4OID && fields[4].Format == pgx.BinaryFormatCode
}
