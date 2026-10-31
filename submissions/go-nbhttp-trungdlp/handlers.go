package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"hash"
	"io"
	"log"
	"math"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf16"

	"github.com/bytedance/sonic"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	feedSize          = 20
	maxPostBodyLength = 500
	maxRequestBody    = 16 << 10
	// Upper bound on waiting for a pool connection plus the statement (pgxpool has no
	// separate acquire timeout; Acquire honours the context deadline).
	dbTimeout = 5 * time.Second
	// created_at on the wire: UTC, ms. Go's fractional-second layout truncates, never rounds.
	createdAtLayout = "2006-01-02T15:04:05.000Z"
)

const postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p
  JOIN users u ON u.id = p.user_id`

var feedPostsPool = sync.Pool{
	New: func() any { return make([]post, feedSize) },
}

type app struct {
	db        *pgxpool.Pool
	jwtSecret []byte
	jwtMAC    sync.Pool
	started   time.Time
}

func (a *app) routes() http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == http.MethodGet && r.URL.Path == "/health":
			a.health(w, r)
		case r.Method == http.MethodGet && r.URL.Path == "/feed":
			a.feed(w, r)
		case r.Method == http.MethodGet && postPath(r.URL.Path, ""):
			a.getPost(w, r)
		case r.Method == http.MethodPost && r.URL.Path == "/posts":
			a.createPost(w, r)
		case r.Method == http.MethodPost && postPath(r.URL.Path, "/like"):
			a.likePost(w, r)
		default:
			fail(w, http.StatusNotFound, "not found")
		}
	})
}

func postPath(path, suffix string) bool {
	if !strings.HasPrefix(path, "/posts/") || !strings.HasSuffix(path, suffix) {
		return false
	}
	id := strings.TrimSuffix(strings.TrimPrefix(path, "/posts/"), suffix)
	return id != "" && !strings.Contains(id, "/")
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	body, err := sonic.ConfigFastest.Marshal(value)
	if err != nil {
		log.Printf("[json] %v", err)
		fail(w, http.StatusInternalServerError, "internal server error")
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if _, err := w.Write(body); err != nil {
		log.Printf("[write] %v", err)
	}
}

// Field order is wire order.
type post struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}

func scanPost(row pgx.Row) (post, error) {
	var p post
	var created time.Time
	err := row.Scan(&p.ID, &p.Body, &created, &p.Author, &p.LikeCount)
	p.CreatedAt = formatTime(created)
	return p, err
}

func formatTime(t time.Time) string { return t.UTC().Format(createdAtLayout) }

func dbCtx(r *http.Request) (context.Context, context.CancelFunc) {
	return context.WithTimeout(r.Context(), dbTimeout)
}

func fail(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, struct {
		Error string `json:"error"`
	}{msg})
}

func internalError(w http.ResponseWriter, err error) {
	log.Printf("[error] %v", err)
	fail(w, http.StatusInternalServerError, "internal server error")
}

func parseID(r *http.Request) (int64, bool) {
	path := strings.TrimPrefix(r.URL.Path, "/posts/")
	path = strings.TrimSuffix(path, "/like")
	id, err := strconv.ParseInt(path, 10, 64)
	return id, err == nil && id > 0
}

type user struct {
	id       int64
	username string
}

// authenticate checks the signature and expiry on every authenticated request.
func (a *app) authenticate(w http.ResponseWriter, r *http.Request) (user, bool) {
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, "Bearer ") {
		fail(w, http.StatusUnauthorized, "missing bearer token")
		return user{}, false
	}
	sub, name, tokenOK, payloadOK := verifyJWT(strings.TrimSpace(header[len("Bearer "):]), a.jwtSecret, &a.jwtMAC)
	if !tokenOK {
		fail(w, http.StatusUnauthorized, "invalid or expired token")
		return user{}, false
	}
	if !payloadOK {
		fail(w, http.StatusUnauthorized, "invalid token payload")
		return user{}, false
	}
	id, err := strconv.ParseInt(sub, 10, 64)
	if err != nil || id <= 0 {
		fail(w, http.StatusUnauthorized, "invalid token payload")
		return user{}, false
	}
	return user{id: id, username: name}, true
}

// verifyJWT checks the compact HS256 token without allocating jwt.Token/Header objects.
// It returns ok=false for all signature, encoding, algorithm, and expiration failures;
// payload type validation remains in authenticate so the API can return its exact error.
func verifyJWT(raw string, secret []byte, macPool *sync.Pool) (sub, username string, tokenOK, payloadOK bool) {
	first := strings.IndexByte(raw, '.')
	if first <= 0 || first == len(raw)-1 {
		return "", "", false, false
	}
	rest := raw[first+1:]
	secondRel := strings.IndexByte(rest, '.')
	if secondRel <= 0 || secondRel == len(rest)-1 || strings.IndexByte(rest[secondRel+1:], '.') >= 0 {
		return "", "", false, false
	}
	second := first + 1 + secondRel
	headerBytes, err := base64.RawURLEncoding.DecodeString(raw[:first])
	if err != nil {
		return "", "", false, false
	}
	var header struct {
		Alg string `json:"alg"`
	}
	if err := sonic.ConfigStd.Unmarshal(headerBytes, &header); err != nil || header.Alg != "HS256" {
		return "", "", false, false
	}
	payloadBytes, err := base64.RawURLEncoding.DecodeString(raw[first+1 : second])
	if err != nil {
		return "", "", false, false
	}
	var claims struct {
		Sub      any     `json:"sub"`
		Username any     `json:"username"`
		Expires  float64 `json:"exp"`
	}
	if err := sonic.ConfigStd.Unmarshal(payloadBytes, &claims); err != nil {
		return "", "", false, false
	}
	signature, err := base64.RawURLEncoding.DecodeString(raw[second+1:])
	if err != nil || len(signature) != sha256.Size {
		return "", "", false, false
	}
	var mac hash.Hash
	if macPool.New != nil {
		mac = macPool.Get().(hash.Hash)
	} else {
		mac = hmac.New(sha256.New, secret)
	}
	mac.Reset()
	_, _ = mac.Write([]byte(raw[:second]))
	var digest [sha256.Size]byte
	sum := mac.Sum(digest[:0])
	if macPool.New != nil {
		macPool.Put(mac)
	}
	if subtle.ConstantTimeCompare(signature, sum) != 1 {
		return "", "", false, false
	}
	if math.IsNaN(claims.Expires) || math.IsInf(claims.Expires, 0) || claims.Expires <= float64(time.Now().Unix()) {
		return "", "", false, false
	}
	sub, ok := claims.Sub.(string)
	if !ok {
		return "", "", true, false
	}
	username, ok = claims.Username.(string)
	if !ok {
		return "", "", true, false
	}
	return sub, username, true, true
}

func (a *app) health(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := dbCtx(r)
	defer cancel()
	if _, err := a.db.Exec(ctx, "SELECT 1"); err != nil {
		writeJSON(w, http.StatusServiceUnavailable, struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Error  string `json:"error"`
		}{"degraded", "unreachable", err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Status  string `json:"status"`
		DB      string `json:"db"`
		UptimeS int64  `json:"uptime_s"`
	}{"ok", "ok", int64(time.Since(a.started).Seconds())})
}

func (a *app) feed(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := dbCtx(r)
	defer cancel()
	rows, err := a.db.Query(ctx, postSelect+` ORDER BY p.created_at DESC, p.id DESC LIMIT $1`, feedSize)
	if err != nil {
		internalError(w, err)
		return
	}
	defer rows.Close()
	pooled := feedPostsPool.Get().([]post)
	posts := pooled[:0]
	defer func() {
		clear(pooled)
		feedPostsPool.Put(pooled)
	}()
	for rows.Next() {
		p, err := scanPost(rows)
		if err != nil {
			internalError(w, err)
			return
		}
		posts = append(posts, p)
	}
	if err := rows.Err(); err != nil {
		internalError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Posts []post `json:"posts"`
	}{posts})
}

func (a *app) getPost(w http.ResponseWriter, r *http.Request) {
	id, ok := parseID(r)
	if !ok {
		fail(w, http.StatusBadRequest, "invalid post id")
		return
	}
	ctx, cancel := dbCtx(r)
	defer cancel()
	p, err := scanPost(a.db.QueryRow(ctx, postSelect+` WHERE p.id = $1`, id))
	if errors.Is(err, pgx.ErrNoRows) {
		fail(w, http.StatusNotFound, "post not found")
		return
	}
	if err != nil {
		internalError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Post post `json:"post"`
	}{p})
}

func (a *app) createPost(w http.ResponseWriter, r *http.Request) {
	u, ok := a.authenticate(w, r)
	if !ok {
		return
	}
	data, err := io.ReadAll(http.MaxBytesReader(w, r.Body, maxRequestBody))
	var value any
	if err != nil || (len(data) != 0 && sonic.ConfigStd.Unmarshal(data, &value) != nil) {
		fail(w, http.StatusBadRequest, "malformed JSON body")
		return
	}
	obj, _ := value.(map[string]any)
	body, _ := obj["body"].(string)
	body = strings.TrimSpace(body)
	if body == "" {
		fail(w, http.StatusBadRequest, "body is required")
		return
	}
	if len(utf16.Encode([]rune(body))) > maxPostBodyLength {
		fail(w, http.StatusBadRequest, "body must be at most 500 characters")
		return
	}
	p := post{Body: body, Author: u.username}
	ctx, cancel := dbCtx(r)
	defer cancel()
	var created time.Time
	err = a.db.QueryRow(ctx,
		`INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`,
		u.id, body).Scan(&p.ID, &created)
	p.CreatedAt = formatTime(created)
	if err != nil {
		internalError(w, err)
		return
	}
	writeJSON(w, http.StatusCreated, struct {
		Post post `json:"post"`
	}{p})
}

func (a *app) likePost(w http.ResponseWriter, r *http.Request) {
	u, ok := a.authenticate(w, r)
	if !ok {
		return
	}
	postID, ok := parseID(r)
	if !ok {
		fail(w, http.StatusBadRequest, "invalid post id")
		return
	}
	ctx, cancel := dbCtx(r)
	defer cancel()
	tag, err := a.db.Exec(ctx,
		`INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`,
		u.id, postID)
	if err != nil {
		var pe *pgconn.PgError
		if errors.As(err, &pe) && pe.Code == "23503" { // foreign_key_violation
			fail(w, http.StatusNotFound, "post not found")
			return
		}
		internalError(w, err)
		return
	}
	n := tag.RowsAffected()
	status := http.StatusOK
	if n > 0 {
		status = http.StatusCreated
	}
	writeJSON(w, status, struct {
		Liked        bool  `json:"liked"`
		AlreadyLiked bool  `json:"already_liked"`
		PostID       int64 `json:"post_id"`
	}{true, n == 0, postID})
}
