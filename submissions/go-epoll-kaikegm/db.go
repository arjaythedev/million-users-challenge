package main

import (
	"bytes"
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/klauspost/compress/gzip"
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

type DBJobType uint8

const (
	JobHealth DBJobType = iota
	JobFeed
	JobGetPost
	JobCreatePost
	JobLikePost
)

type dbJob struct {
	fd         int32
	jobType    DBJobType
	gzip       bool
	closeAfter bool
	postID     int64
	userID     int64
	username   string
	body       string
}

type DBPool struct {
	readPool    *pgxpool.Pool
	writePool   *pgxpool.Pool
	readJobs    chan dbJob
	writeJobs   chan dbJob
	started     time.Time
	readLinger  time.Duration
	writeLinger time.Duration
}

func NewDBPool(readPool, writePool *pgxpool.Pool, readWorkers, writeWorkers int, readLinger, writeLinger time.Duration) *DBPool {
	dp := &DBPool{
		readPool:    readPool,
		writePool:   writePool,
		readJobs:    make(chan dbJob, 65536),
		writeJobs:   make(chan dbJob, 16384),
		started:     time.Now(),
		readLinger:  readLinger,
		writeLinger: writeLinger,
	}
	for i := 0; i < readWorkers; i++ {
		go dp.readWorker()
	}
	for i := 0; i < writeWorkers; i++ {
		go dp.writeWorker()
	}
	return dp
}

func (dp *DBPool) Submit(job dbJob) {
	if job.jobType == JobCreatePost || job.jobType == JobLikePost {
		select {
		case dp.writeJobs <- job:
		default:
			t := time.NewTimer(200 * time.Millisecond)
			select {
			case dp.writeJobs <- job:
				t.Stop()
			case <-t.C:
				var buf [256]byte
				sendSyncError(int(job.fd), 500, "internal server error", buf[:0])
			}
		}
		return
	}

	select {
	case dp.readJobs <- job:
	default:
		t := time.NewTimer(200 * time.Millisecond)
		select {
		case dp.readJobs <- job:
			t.Stop()
		case <-t.C:
			var buf [256]byte
			sendSyncError(int(job.fd), 500, "internal server error", buf[:0])
		}
	}
}

func (dp *DBPool) readWorker() {
	jsonBuf := make([]byte, 0, 16384)
	httpBuf := make([]byte, 0, 16384)
	var gzipBuf bytes.Buffer
	gzWriter, _ := gzip.NewWriterLevel(&gzipBuf, gzip.BestSpeed)

	ctx := context.Background()
	jobs := make([]dbJob, 0, 32)

	for {
		j, ok := <-dp.readJobs
		if !ok {
			return
		}
		jobs = append(jobs[:0], j)

	drain:
		for len(jobs) < 32 {
			select {
			case j2, ok := <-dp.readJobs:
				if !ok {
					break drain
				}
				jobs = append(jobs, j2)
			default:
				if len(jobs) < 16 {
					time.Sleep(dp.readLinger)
					select {
					case j2, ok := <-dp.readJobs:
						if !ok {
							break drain
						}
						jobs = append(jobs, j2)
						continue
					default:
					}
				}
				break drain
			}
		}

		dp.processReadBatch(ctx, jobs, &jsonBuf, &httpBuf, &gzipBuf, gzWriter)
	}
}

func (dp *DBPool) processReadBatch(ctx context.Context, jobs []dbJob, jsonBuf, httpBuf *[]byte, gzipBuf *bytes.Buffer, gzWriter *gzip.Writer) {
	conn, err := dp.readPool.Acquire(ctx)
	if err != nil {
		for i := range jobs {
			*httpBuf = (*httpBuf)[:0]
			*jsonBuf = formatErrorJSON((*jsonBuf)[:0], "db unavailable")
			*httpBuf = appendHTTPResponse(*httpBuf, 500, *jsonBuf, false)
			_ = sendAll(int(jobs[i].fd), *httpBuf)
			if jobs[i].closeAfter {
				clearConn(int(jobs[i].fd))
				_ = unix.Close(int(jobs[i].fd))
			}
		}
		return
	}
	defer conn.Release()

	batch := &pgx.Batch{}
	for i := range jobs {
		j := &jobs[i]
		switch j.jobType {
		case JobHealth:
			batch.Queue("SELECT 1")
		case JobFeed:
			batch.Queue(feedSelect)
		case JobGetPost:
			batch.Queue(getPostSelect, j.postID)
		}
	}

	br := conn.SendBatch(ctx, batch)
	for i := range jobs {
		dp.processReadResult(br, &jobs[i], jsonBuf, httpBuf, gzipBuf, gzWriter)
	}
	_ = br.Close()
}

func (dp *DBPool) processReadResult(br pgx.BatchResults, job *dbJob, jsonBuf, httpBuf *[]byte, gzipBuf *bytes.Buffer, gzWriter *gzip.Writer) {
	*jsonBuf = (*jsonBuf)[:0]
	*httpBuf = (*httpBuf)[:0]
	status := 200
	shouldGzip := false

	switch job.jobType {
	case JobHealth:
		_, err := br.Exec()
		if err != nil {
			status = 503
			*jsonBuf = formatHealthError(*jsonBuf, err.Error())
		} else {
			*jsonBuf = formatHealthOK(*jsonBuf, int64(time.Since(dp.started).Seconds()))
		}

	case JobFeed:
		rows, err := br.Query()
		if err != nil {
			status = 500
			*jsonBuf = formatErrorJSON(*jsonBuf, "internal server error")
		} else {
			var posts [20]Post
			n := 0
			for rows.Next() {
				var p Post
				var t time.Time
				if err := rows.Scan(&p.ID, &p.Body, &t, &p.Author, &p.LikeCount); err == nil && n < 20 {
					p.CreatedAt = formatTime(t)
					posts[n] = p
					n++
				}
			}
			rows.Close()
			*jsonBuf = formatFeedJSON(*jsonBuf, posts[:n])
			if job.gzip && len(*jsonBuf) > 256 {
				shouldGzip = true
			}
		}

	case JobGetPost:
		var p Post
		var t time.Time
		err := br.QueryRow().Scan(&p.ID, &p.Body, &t, &p.Author, &p.LikeCount)
		if errors.Is(err, pgx.ErrNoRows) {
			status = 404
			*jsonBuf = formatErrorJSON(*jsonBuf, "post not found")
		} else if err != nil {
			status = 500
			*jsonBuf = formatErrorJSON(*jsonBuf, "internal server error")
		} else {
			p.CreatedAt = formatTime(t)
			*jsonBuf = formatGetPostJSON(*jsonBuf, p)
		}
	}

	var finalBody []byte
	if shouldGzip {
		gzipBuf.Reset()
		gzWriter.Reset(gzipBuf)
		_, _ = gzWriter.Write(*jsonBuf)
		_ = gzWriter.Close()
		finalBody = gzipBuf.Bytes()
	} else {
		finalBody = *jsonBuf
	}

	*httpBuf = appendHTTPResponse(*httpBuf, status, finalBody, shouldGzip)
	if err := sendAll(int(job.fd), *httpBuf); err != nil || job.closeAfter {
		clearConn(int(job.fd))
		_ = unix.Close(int(job.fd))
	}
}

func (dp *DBPool) writeWorker() {
	jsonBuf := make([]byte, 0, 16384)
	httpBuf := make([]byte, 0, 16384)

	ctx := context.Background()
	jobs := make([]dbJob, 0, 32)

	for {
		j, ok := <-dp.writeJobs
		if !ok {
			return
		}
		jobs = append(jobs[:0], j)

	drain:
		for len(jobs) < 32 {
			select {
			case j2, ok := <-dp.writeJobs:
				if !ok {
					break drain
				}
				jobs = append(jobs, j2)
			default:
				if len(jobs) < 8 {
					time.Sleep(dp.writeLinger)
					select {
					case j2, ok := <-dp.writeJobs:
						if !ok {
							break drain
						}
						jobs = append(jobs, j2)
						continue
					default:
					}
				}
				break drain
			}
		}

		dp.processWriteBatch(ctx, jobs, &jsonBuf, &httpBuf)
	}
}

func (dp *DBPool) processWriteBatch(ctx context.Context, jobs []dbJob, jsonBuf, httpBuf *[]byte) {
	conn, err := dp.writePool.Acquire(ctx)
	if err != nil {
		for i := range jobs {
			*httpBuf = (*httpBuf)[:0]
			*jsonBuf = formatErrorJSON((*jsonBuf)[:0], "db unavailable")
			*httpBuf = appendHTTPResponse(*httpBuf, 500, *jsonBuf, false)
			_ = sendAll(int(jobs[i].fd), *httpBuf)
			if jobs[i].closeAfter {
				clearConn(int(jobs[i].fd))
				_ = unix.Close(int(jobs[i].fd))
			}
		}
		return
	}
	defer conn.Release()

	batch := &pgx.Batch{}
	for i := range jobs {
		j := &jobs[i]
		switch j.jobType {
		case JobCreatePost:
			batch.Queue(createPostQuery, j.userID, j.body)
		case JobLikePost:
			batch.Queue(likePostQuery, j.userID, j.postID)
		}
	}

	br := conn.SendBatch(ctx, batch)
	for i := range jobs {
		dp.processWriteResult(br, &jobs[i], jsonBuf, httpBuf)
	}
	_ = br.Close()
}

func (dp *DBPool) processWriteResult(br pgx.BatchResults, job *dbJob, jsonBuf, httpBuf *[]byte) {
	*jsonBuf = (*jsonBuf)[:0]
	*httpBuf = (*httpBuf)[:0]
	status := 200

	switch job.jobType {
	case JobCreatePost:
		var postID int64
		var t time.Time
		err := br.QueryRow().Scan(&postID, &t)
		if err != nil {
			status = 500
			*jsonBuf = formatErrorJSON(*jsonBuf, "internal server error")
		} else {
			status = 201
			*jsonBuf = formatCreatePostJSON(*jsonBuf, Post{
				ID: postID, Body: job.body, CreatedAt: formatTime(t), Author: job.username,
			})
		}

	case JobLikePost:
		tag, err := br.Exec()
		if err != nil {
			var pe *pgconn.PgError
			if errors.As(err, &pe) && pe.Code == "23503" {
				status = 404
				*jsonBuf = formatErrorJSON(*jsonBuf, "post not found")
			} else {
				status = 500
				*jsonBuf = formatErrorJSON(*jsonBuf, "internal server error")
			}
		} else if tag.RowsAffected() > 0 {
			status = 201
			*jsonBuf = formatLikeJSON(*jsonBuf, false, job.postID)
		} else {
			status = 200
			*jsonBuf = formatLikeJSON(*jsonBuf, true, job.postID)
		}
	}

	*httpBuf = appendHTTPResponse(*httpBuf, status, *jsonBuf, false)
	if err := sendAll(int(job.fd), *httpBuf); err != nil || job.closeAfter {
		clearConn(int(job.fd))
		_ = unix.Close(int(job.fd))
	}
}

// Fix 7: deadline total of 100ms prevents slow/unresponsive clients from starving DB workers
func sendAll(fd int, b []byte) error {
	deadline := time.Now().Add(100 * time.Millisecond)
	for len(b) > 0 {
		n, err := unix.Write(fd, b)
		if err != nil {
			if err == unix.EAGAIN || err == unix.EWOULDBLOCK {
				if time.Now().After(deadline) {
					return errors.New("slow client timeout")
				}
				time.Sleep(50 * time.Microsecond)
				continue
			}
			return err
		}
		b = b[n:]
	}
	return nil
}

func sendSyncResponse(fd int, status int, body []byte, dst []byte) error {
	dst = dst[:0]
	dst = appendHTTPResponse(dst, status, body, false)
	return sendAll(fd, dst)
}

func sendSyncError(fd int, status int, msg string, dst []byte) error {
	var body [256]byte
	b := formatErrorJSON(body[:0], msg)
	return sendSyncResponse(fd, status, b, dst)
}
