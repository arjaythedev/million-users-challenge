package main

import (
	"bytes"
	"context"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"io"
	"log"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	hertzapp "github.com/cloudwego/hertz/pkg/app"
	"github.com/cloudwego/hertz/pkg/app/server"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/klauspost/compress/gzip"
)

const (
	feedSize        = 20
	createdAtLayout = "2006-01-02T15:04:05.000Z"
)

const postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p
  JOIN users u ON u.id = p.user_id`

type app struct {
	jwtSecret []byte
	reads     DBTX
	writes    DBTX
	started   time.Time
}

func (a *app) routes(r *server.Hertz) {
	r.GET("/health", a.health)

	r.GET("/feed", a.feed) // the only response gzipped (writeGzip): ~5 KB -> ~2 KB
	r.GET("/posts/:id", a.getPost)

	// Require auth
	r.POST("/posts", a.auth(a.createPost))
	r.POST("/posts/:id/like", a.auth(a.likePost))

	r.NoRoute(func(ctx context.Context, rc *hertzapp.RequestContext) {
		a.error(rc, http.StatusNotFound, "not found")
	})
}

// response definitions

type healthResponse struct {
	Status  string
	DB      string
	Error   string
	UptimeS int64
}

func (r healthResponse) MarshalJSONTo(enc *jsontext.Encoder) error {
	enc.WriteToken(jsontext.BeginObject)

	enc.WriteToken(jsontext.String("status"))
	enc.WriteToken(jsontext.String(r.Status))

	enc.WriteToken(jsontext.String("db"))
	enc.WriteToken(jsontext.String(r.DB))

	if r.Error != "" {
		enc.WriteToken(jsontext.String("error"))
		enc.WriteToken(jsontext.String(r.Error))
	} else {
		enc.WriteToken(jsontext.String("uptime_s"))
		enc.WriteToken(jsontext.Int(r.UptimeS))
	}

	enc.WriteToken(jsontext.EndObject)

	return nil
}

// postRow is the scan target for postSelect and the post object in every response.
type postRow struct {
	ID        int64
	Body      string
	CreatedAt time.Time
	Author    string
	LikeCount int64
}

func (p postRow) MarshalJSONTo(enc *jsontext.Encoder) error {
	enc.WriteToken(jsontext.BeginObject)

	enc.WriteToken(jsontext.String("id"))
	enc.WriteToken(jsontext.Int(p.ID))

	enc.WriteToken(jsontext.String("body"))
	enc.WriteToken(jsontext.String(p.Body))

	// quoted timestamp built on the stack.
	var buf [64]byte
	ts := append(buf[:0], '"')
	ts = p.CreatedAt.UTC().AppendFormat(ts, createdAtLayout)
	ts = append(ts, '"')
	enc.WriteToken(jsontext.String("created_at"))
	enc.WriteValue(ts)

	enc.WriteToken(jsontext.String("author"))
	enc.WriteToken(jsontext.String(p.Author))

	enc.WriteToken(jsontext.String("like_count"))
	enc.WriteToken(jsontext.Int(p.LikeCount))

	enc.WriteToken(jsontext.EndObject)

	return nil
}

type feedResponse struct {
	Posts []postRow
}

func (r feedResponse) MarshalJSONTo(enc *jsontext.Encoder) error {
	enc.WriteToken(jsontext.BeginObject)
	enc.WriteToken(jsontext.String("posts"))

	enc.WriteToken(jsontext.BeginArray)
	for _, p := range r.Posts {
		p.MarshalJSONTo(enc)
	}
	enc.WriteToken(jsontext.EndArray)

	enc.WriteToken(jsontext.EndObject)

	return nil
}

type postResponse struct {
	Post postRow
}

func (r postResponse) MarshalJSONTo(enc *jsontext.Encoder) error {
	enc.WriteToken(jsontext.BeginObject)
	enc.WriteToken(jsontext.String("post"))
	r.Post.MarshalJSONTo(enc)
	enc.WriteToken(jsontext.EndObject)

	return nil
}

type likeResponse struct {
	AlreadyLiked bool
	PostID       int64
}

func (r likeResponse) MarshalJSONTo(enc *jsontext.Encoder) error {
	enc.WriteToken(jsontext.BeginObject)

	enc.WriteToken(jsontext.String("liked"))
	enc.WriteToken(jsontext.True)

	enc.WriteToken(jsontext.String("already_liked"))
	enc.WriteToken(jsontext.Bool(r.AlreadyLiked))

	enc.WriteToken(jsontext.String("post_id"))
	enc.WriteToken(jsontext.Int(r.PostID))

	enc.WriteToken(jsontext.EndObject)

	return nil
}

// route definitions

func (a *app) health(ctx context.Context, rc *hertzapp.RequestContext) {
	if _, err := a.reads.Exec(ctx, "SELECT 1"); err != nil {
		write(rc, http.StatusServiceUnavailable, healthResponse{
			Status: "degraded",
			DB:     "unreachable",
			Error:  err.Error(),
		})
		return
	}

	write(rc, http.StatusOK, healthResponse{
		Status:  "ok",
		DB:      "ok",
		UptimeS: int64(time.Since(a.started).Seconds()),
	})
}

func (a *app) feed(ctx context.Context, rc *hertzapp.RequestContext) {
	rows, err := a.reads.Query(ctx, postSelect+` ORDER BY p.created_at DESC, p.id DESC LIMIT $1`, feedSize)
	if err != nil {
		a.internalErr(rc, err)
		return
	}

	defer rows.Close()

	var posts [feedSize]postRow
	n := 0
	for n < feedSize && rows.Next() {
		p := &posts[n]
		if err := rows.Scan(&p.ID, &p.Body, &p.CreatedAt, &p.Author, &p.LikeCount); err != nil {
			a.internalErr(rc, err)
			return
		}
		n++
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		a.internalErr(rc, err)
		return
	}

	writeGzip(rc, http.StatusOK, feedResponse{Posts: posts[:n]})
}

func parseID(rc *hertzapp.RequestContext) (int64, bool) {
	id, err := strconv.ParseInt(rc.Param("id"), 10, 64)
	return id, err == nil && id > 0
}

func (a *app) getPost(ctx context.Context, rc *hertzapp.RequestContext) {
	id, ok := parseID(rc)
	if !ok {
		a.error(rc, http.StatusBadRequest, "invalid post id")
		return
	}

	row, _ := a.reads.Query(ctx, postSelect+` WHERE p.id = $1`, id)
	pr, err := pgx.CollectOneRow(row, pgx.RowToStructByPos[postRow])
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			a.error(rc, http.StatusNotFound, "post not found")
			return
		}
		a.internalErr(rc, err)
		return
	}

	write(rc, http.StatusOK, postResponse{Post: pr})
}

func (a *app) createPost(ctx context.Context, rc *hertzapp.RequestContext) {
	u, _ := getUser(ctx)

	var req struct {
		Body string `json:"body"`
	}

	err := json.Unmarshal(rc.Request.Body(), &req)
	if err != nil {
		if _, ok := errors.AsType[*json.SemanticError](err); !ok {
			a.error(rc, http.StatusBadRequest, "malformed JSON body")
			return
		}
		req.Body = ""
	}

	body := strings.TrimSpace(req.Body)
	if body == "" {
		a.error(rc, http.StatusBadRequest, "body is required")
		return
	}

	if utf8.RuneCountInString(body) > 500 {
		a.error(rc, http.StatusBadRequest, "body must be at most 500 characters")
		return
	}

	pr := postRow{Body: body, Author: u.username}
	err = a.writes.QueryRow(ctx,
		`INSERT INTO posts (user_id,  body) VALUES ($1, $2) RETURNING id, created_at`,
		u.id, body).Scan(&pr.ID, &pr.CreatedAt)
	if err != nil {
		a.internalErr(rc, err)
		return
	}

	write(rc, http.StatusCreated, postResponse{Post: pr})
}

func (a *app) likePost(ctx context.Context, rc *hertzapp.RequestContext) {
	u, _ := getUser(ctx)

	postId, ok := parseID(rc)
	if !ok {
		a.error(rc, http.StatusBadRequest, "invalid post id")
		return
	}

	tag, err := a.writes.Exec(ctx,
		`INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`,
		u.id, postId)
	if err != nil {
		if pe, ok := errors.AsType[*pgconn.PgError](err); ok && pe.Code == "23503" {
			a.error(rc, http.StatusNotFound, "post not found")
			return
		}
		a.internalErr(rc, err)
		return
	}

	n := tag.RowsAffected()
	status := http.StatusOK
	if n > 0 {
		status = http.StatusCreated
	}

	write(rc, status, likeResponse{AlreadyLiked: n == 0, PostID: postId})
}

var encoderPool = sync.Pool{
	New: func() any {
		return new(jsontext.Encoder)
	},
}

func getEncoder() *jsontext.Encoder {
	return encoderPool.Get().(*jsontext.Encoder)
}

func putEncoder(v *jsontext.Encoder) {
	encoderPool.Put(v)
}

var (
	jsonContentType = []byte("application/json")
	gzipEncoding    = []byte("gzip")
)

func write[T json.MarshalerTo](w *hertzapp.RequestContext, statusCode int, v T) {
	enc := getEncoder()
	enc.Reset(w)

	w.Response.Header.SetContentTypeBytes(jsonContentType)
	w.Status(statusCode)
	v.MarshalJSONTo(enc)

	// drop references to this request before pooling
	enc.Reset(io.Discard)
	putEncoder(enc)
}

var gzipPool = sync.Pool{
	New: func() any {
		zw, _ := gzip.NewWriterLevel(nil, gzip.BestSpeed)
		return zw
	},
}

func writeGzip[T json.MarshalerTo](w *hertzapp.RequestContext, statusCode int, v T) {
	w.Response.Header.Set("Vary", "Accept-Encoding")
	if !bytes.Contains(w.Request.Header.Peek("Accept-Encoding"), gzipEncoding) {
		write(w, statusCode, v)
		return
	}

	zw := gzipPool.Get().(*gzip.Writer)
	zw.Reset(w)
	enc := getEncoder()
	enc.Reset(zw)

	w.Response.Header.SetContentTypeBytes(jsonContentType)
	w.Response.Header.SetContentEncodingBytes(gzipEncoding)
	w.Status(statusCode)
	v.MarshalJSONTo(enc)
	zw.Close()

	// drop references to this request before pooling
	enc.Reset(io.Discard)
	putEncoder(enc)
	zw.Reset(io.Discard)
	gzipPool.Put(zw)
}

type errorResponse struct {
	Error string
}

func (r errorResponse) MarshalJSONTo(enc *jsontext.Encoder) error {
	enc.WriteToken(jsontext.BeginObject)
	enc.WriteToken(jsontext.String("error"))
	enc.WriteToken(jsontext.String(r.Error))
	enc.WriteToken(jsontext.EndObject)
	return nil
}

func (a *app) error(w *hertzapp.RequestContext, statusCode int, msg string) {
	write(w, statusCode, errorResponse{Error: msg})
}

func (a *app) internalErr(w *hertzapp.RequestContext, err error) {
	log.Println(err)
	a.error(w, http.StatusInternalServerError, "internal server error")
}
