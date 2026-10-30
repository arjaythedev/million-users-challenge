package main

import (
	"context"
	"errors"
	"slices"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	likeInsert = `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`
	postInsert = `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`
)

type writeKind uint8

const (
	writeLike writeKind = iota
	writePost
)

type writeReq struct {
	kind   writeKind
	userID int64
	postID int64  // like
	body   string // post
	done   chan writeRes
}

type writeRes struct {
	inserted  bool      // like: false when it already existed
	id        int64     // post
	createdAt time.Time // post
	err       error
}

var writeReqPool = sync.Pool{
	New: func() any {
		return &writeReq{done: make(chan writeRes, 1)}
	},
}

type writer struct {
	db   *pgxpool.Pool
	reqs chan *writeReq
	max  int
}

func newWriter(db *pgxpool.Pool, workers, max int) *writer {
	w := &writer{
		db:   db,
		reqs: make(chan *writeReq, workers*max),
		max:  max,
	}
	for range workers {
		go w.run()
	}
	return w
}

func (w *writer) like(ctx context.Context, userID, postID int64) (bool, error) {
	res, err := w.submit(ctx, writeLike, userID, postID, "")
	return res.inserted, err
}

func (w *writer) post(ctx context.Context, userID int64, body string) (int64, time.Time, error) {
	res, err := w.submit(ctx, writePost, userID, 0, body)
	return res.id, res.createdAt, err
}

func (w *writer) submit(ctx context.Context, kind writeKind, userID, postID int64, body string) (writeRes, error) {
	req := writeReqPool.Get().(*writeReq)
	req.kind, req.userID, req.postID, req.body = kind, userID, postID, body

	select {
	case w.reqs <- req:
	case <-ctx.Done():
		writeReqPool.Put(req)
		return writeRes{}, ctx.Err()
	}

	res := <-req.done
	req.body = ""
	writeReqPool.Put(req)
	return res, res.err
}

func (w *writer) run() {
	batch := make([]*writeReq, 0, w.max)
	for {
		batch = append(batch[:0], <-w.reqs)
	drain:
		for len(batch) < w.max {
			select {
			case req := <-w.reqs:
				batch = append(batch, req)
			default:
				break drain
			}
		}
		w.commit(batch)
	}
}

func (w *writer) commit(batch []*writeReq) {
	for len(batch) > 0 {
		failed, err := w.exec(batch)
		if failed < 0 {
			return
		}

		pe, ok := errors.AsType[*pgconn.PgError](err)
		if !ok {
			for _, req := range batch {
				req.done <- writeRes{err: err}
			}
			return
		}
		if pe.Code == "40P01" { // deadlock victim: retry the whole batch
			continue
		}
		batch[failed].done <- writeRes{err: err}
		batch = slices.Delete(batch, failed, failed+1)
	}
}

func (w *writer) exec(batch []*writeReq) (int, error) {
	var b pgx.Batch
	for _, req := range batch {
		switch req.kind {
		case writeLike:
			b.Queue(likeInsert, req.userID, req.postID)
		case writePost:
			b.Queue(postInsert, req.userID, req.body)
		}
	}

	br := w.db.SendBatch(context.Background(), &b)

	res := make([]writeRes, len(batch))
	for i, req := range batch {
		switch req.kind {
		case writeLike:
			tag, err := br.Exec()
			res[i] = writeRes{inserted: tag.RowsAffected() > 0, err: err}
		case writePost:
			res[i].err = br.QueryRow().Scan(&res[i].id, &res[i].createdAt)
		}
		if res[i].err != nil {
			_ = br.Close()
			return i, res[i].err
		}
	}

	if err := br.Close(); err != nil {
		for _, req := range batch {
			req.done <- writeRes{err: err}
		}
		return -1, nil
	}

	for i, req := range batch {
		req.done <- res[i]
	}
	return -1, nil
}
