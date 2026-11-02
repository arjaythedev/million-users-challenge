package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode/utf16"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	feedSize          = 20
	maxPostBodyLength = 500 // characters, counted as UTF-16 code units after trimming
	maxRequestBody    = 16 << 10
)

// postSelect is the SPEC.md query; handlers append the feed ORDER BY/LIMIT or
// the id WHERE clause. pgx prepares it per pooled connection and reuses the plan.
// The feed's LIMIT stays a literal: a parameter hides it from the planner.
const postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p
  JOIN users u ON u.id = p.user_id`

// app wires the pool and configuration into the HTTP handlers.
type app struct {
	cfg     Config
	pool    *pgxpool.Pool
	started time.Time
}

type ctxKey int

const userKey ctxKey = 0

func withUser(ctx context.Context, u user) context.Context {
	return context.WithValue(ctx, userKey, u)
}

func requestUser(r *http.Request) user {
	u, _ := r.Context().Value(userKey).(user)
	return u
}

// routes builds the handler tree. ServeMux patterns pin the method; "/" is the
// catch-all that answers the SPEC 404 body.
func (a *app) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /health", a.health)
	mux.HandleFunc("GET /feed", a.feed)
	mux.HandleFunc("GET /posts/{id}", a.getPost)
	mux.HandleFunc("POST /posts", a.requireAuth(a.createPost))
	mux.HandleFunc("POST /posts/{id}/like", a.requireAuth(a.likePost))
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		writeError(w, http.StatusNotFound, "not found")
	})
	return mux
}

func writeError(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	b, _ := marshalJSON(errorBody{Error: msg})
	w.Write(b)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	b, err := marshalJSON(v)
	if err != nil {
		// Marshaling of these fixed shapes cannot fail; guard anyway.
		slog.Error("marshal failed", "err", err)
		writeError(w, http.StatusInternalServerError, "internal server error")
		return
	}
	w.WriteHeader(status)
	w.Write(b)
}

// dbCtx bounds waiting for a pooled connection plus statement execution.
func (a *app) dbCtx(r *http.Request) (context.Context, context.CancelFunc) {
	return context.WithTimeout(r.Context(), time.Duration(a.cfg.DBTimeoutMs)*time.Millisecond)
}

func (a *app) health(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := a.dbCtx(r)
	defer cancel()
	if _, err := a.pool.Exec(ctx, "SELECT 1"); err != nil {
		slog.Warn("health check failed", "err", err)
		writeJSON(w, http.StatusServiceUnavailable, healthDegraded{"degraded", "unreachable", err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, healthOK{"ok", "ok", int64(time.Since(a.started).Seconds())})
}

func (a *app) feed(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := a.dbCtx(r)
	defer cancel()
	rows, err := a.pool.Query(ctx, postSelect+" ORDER BY p.created_at DESC, p.id DESC LIMIT 20")
	if err != nil {
		internalError(w, "feed", err)
		return
	}
	defer rows.Close()
	posts := make([]Post, 0, feedSize)
	for rows.Next() {
		p, err := scanPost(rows)
		if err != nil {
			internalError(w, "feed", err)
			return
		}
		posts = append(posts, p)
	}
	if err := rows.Err(); err != nil {
		internalError(w, "feed", err)
		return
	}
	writeJSON(w, http.StatusOK, feedResponse{Posts: posts})
}

func (a *app) getPost(w http.ResponseWriter, r *http.Request) {
	id, ok := parsePostID(r.PathValue("id"))
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	ctx, cancel := a.dbCtx(r)
	defer cancel()
	p, err := scanPost(a.pool.QueryRow(ctx, postSelect+" WHERE p.id = $1", id))
	if errors.Is(err, pgx.ErrNoRows) {
		writeError(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		internalError(w, "get_post", err)
		return
	}
	writeJSON(w, http.StatusOK, postResponse{Post: p})
}

func (a *app) createPost(w http.ResponseWriter, r *http.Request) {
	u := requestUser(r)
	body, status, msg, ok := readPostBody(r)
	if !ok {
		writeError(w, status, msg)
		return
	}
	ctx, cancel := a.dbCtx(r)
	defer cancel()
	var id int64
	var created time.Time
	err := a.pool.QueryRow(ctx,
		`INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`,
		u.id, body).Scan(&id, &created)
	if err != nil {
		internalError(w, "create_post", err)
		return
	}
	writeJSON(w, http.StatusCreated, postResponse{Post: Post{
		ID:        id,
		Body:      body,
		CreatedAt: formatCreatedAt(created),
		Author:    u.username,
		LikeCount: 0,
	}})
}

func (a *app) likePost(w http.ResponseWriter, r *http.Request) {
	u := requestUser(r)
	postID, ok := parsePostID(r.PathValue("id"))
	if !ok {
		writeError(w, http.StatusBadRequest, "invalid post id")
		return
	}
	ctx, cancel := a.dbCtx(r)
	defer cancel()
	tag, err := a.pool.Exec(ctx,
		`INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`,
		u.id, postID)
	if err != nil {
		var pe *pgconn.PgError
		if errors.As(err, &pe) && pe.Code == "23503" { // foreign_key_violation: post does not exist
			writeError(w, http.StatusNotFound, "post not found")
			return
		}
		internalError(w, "like_post", err)
		return
	}
	first := tag.RowsAffected() > 0
	status := http.StatusOK
	if first {
		status = http.StatusCreated
	}
	writeJSON(w, status, likeResponse{Liked: true, AlreadyLiked: !first, PostID: postID})
}

// parsePostID accepts positive integers only: "0", "-1", "abc" and "1.5" are
// all invalid (SPEC.md errors table).
func parsePostID(s string) (int64, bool) {
	id, err := strconv.ParseInt(s, 10, 64)
	return id, err == nil && id > 0
}

type createPostRequest struct {
	Body any `json:"body"`
}

// readPostBody validates the create-post request in SPEC order: JSON must
// parse ("malformed JSON body"), body must be a non-empty-after-trim string
// ("body is required") of at most 500 UTF-16 code units after trimming.
func readPostBody(r *http.Request) (body string, status int, msg string, ok bool) {
	var req createPostRequest
	dec := json.NewDecoder(http.MaxBytesReader(nil, r.Body, maxRequestBody))
	if err := dec.Decode(&req); err != nil && !errors.Is(err, io.EOF) {
		return "", http.StatusBadRequest, "malformed JSON body", false
	}
	s, _ := req.Body.(string)
	s = strings.TrimSpace(s)
	if s == "" {
		return "", http.StatusBadRequest, "body is required", false
	}
	if len(utf16.Encode([]rune(s))) > maxPostBodyLength {
		return "", http.StatusBadRequest, "body must be at most 500 characters", false
	}
	return s, 0, "", true
}

// scanPost decodes one row of postSelect. Valid TEXT is always valid UTF-8,
// which writeJSON relies on.
func scanPost(row pgx.Row) (Post, error) {
	var p Post
	var created time.Time
	err := row.Scan(&p.ID, &p.Body, &created, &p.Author, &p.LikeCount)
	p.CreatedAt = formatCreatedAt(created)
	return p, err
}
