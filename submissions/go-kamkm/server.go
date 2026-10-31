package main

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"math"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"compress/gzip"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	feedLimit       = 20
	maxBodyBytes    = 16 << 10
	maxBodyChars    = 500
	databaseTimeout = 5 * time.Second
	createdLayout   = "2006-01-02T15:04:05.000Z"
)

const postQuery = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id`

type application struct {
	db      *pgxpool.Pool
	reads   *readBatcher
	writes  *writeBatcher
	secret  []byte
	started time.Time
}

type postJSON struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}

type postEnvelope struct {
	Post postJSON `json:"post"`
}

type feedEnvelope struct {
	Posts []postJSON `json:"posts"`
}

type errorEnvelope struct {
	Error string `json:"error"`
}

type healthOK struct {
	Status  string `json:"status"`
	DB      string `json:"db"`
	UptimeS int64  `json:"uptime_s"`
}

type healthBad struct {
	Status string `json:"status"`
	DB     string `json:"db"`
	Error  string `json:"error"`
}

type likeEnvelope struct {
	Liked        bool  `json:"liked"`
	AlreadyLiked bool  `json:"already_liked"`
	PostID       int64 `json:"post_id"`
}

type identity struct {
	id       int64
	username string
}

// Compression state is retained only in pools. Idle keep-alive connections do
// not retain a gzip writer or response buffer.
var gzipWriters sync.Pool
var gzipBuffers sync.Pool

func init() {
	gzipWriters.New = func() any {
		writer, err := gzip.NewWriterLevel(io.Discard, gzip.BestSpeed)
		if err != nil {
			panic(err)
		}
		return writer
	}
	gzipBuffers.New = func() any { return new(bytes.Buffer) }
}

func (a *application) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Path
	switch {
	case r.Method == http.MethodGet && path == "/health":
		a.health(w, r)
	case r.Method == http.MethodGet && path == "/feed":
		a.feed(w, r)
	case r.Method == http.MethodPost && path == "/posts":
		a.create(w, r)
	case r.Method == http.MethodPost && strings.HasPrefix(path, "/posts/") && len(path) >= len("/posts/")+len("/like") && strings.HasSuffix(path, "/like"):
		rawID := path[len("/posts/") : len(path)-len("/like")]
		// A route parameter is exactly one non-empty path segment. Unknown
		// paths must remain 404; authentication-before-ID applies once this
		// endpoint shape has matched.
		if rawID == "" || strings.Contains(rawID, "/") {
			writeJSON(w, r, http.StatusNotFound, errorEnvelope{Error: "not found"})
			return
		}
		a.like(w, r, rawID)
	case strings.HasPrefix(path, "/posts/") && r.Method == http.MethodGet:
		rawID := path[len("/posts/"):]
		if rawID == "" || strings.Contains(rawID, "/") {
			writeJSON(w, r, http.StatusNotFound, errorEnvelope{Error: "not found"})
			return
		}
		a.getPost(w, r, rawID)
	default:
		writeJSON(w, r, http.StatusNotFound, errorEnvelope{Error: "not found"})
	}
}

func (a *application) health(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := queryContext(r.Context())
	defer cancel()
	if _, err := a.db.Exec(ctx, "SELECT 1"); err != nil {
		writeJSON(w, r, http.StatusServiceUnavailable, healthBad{
			Status: "degraded",
			DB:     "unreachable",
			Error:  err.Error(),
		})
		return
	}
	writeJSON(w, r, http.StatusOK, healthOK{
		Status:  "ok",
		DB:      "ok",
		UptimeS: int64(time.Since(a.started) / time.Second),
	})
}

func (a *application) feed(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := queryContext(r.Context())
	defer cancel()
	result := a.reads.submit(ctx, 0)
	err := result.err
	if err != nil {
		writeJSON(w, r, http.StatusInternalServerError, errorEnvelope{Error: "internal server error"})
		return
	}
	body, err := json.Marshal(feedEnvelope{Posts: result.posts})
	if err != nil {
		writeJSON(w, r, http.StatusInternalServerError, errorEnvelope{Error: "internal server error"})
		return
	}
	writeBody(w, r, http.StatusOK, body)
}

func (a *application) getPost(w http.ResponseWriter, r *http.Request, rawID string) {
	id, ok := positiveID(rawID)
	if !ok {
		writeJSON(w, r, http.StatusBadRequest, errorEnvelope{Error: "invalid post id"})
		return
	}
	ctx, cancel := queryContext(r.Context())
	defer cancel()
	result := a.reads.submit(ctx, id)
	err := result.err
	if errors.Is(err, pgx.ErrNoRows) {
		writeJSON(w, r, http.StatusNotFound, errorEnvelope{Error: "post not found"})
		return
	}
	if err != nil {
		writeJSON(w, r, http.StatusInternalServerError, errorEnvelope{Error: "internal server error"})
		return
	}
	writeJSON(w, r, http.StatusOK, postEnvelope{Post: result.posts[0]})
}

func (a *application) create(w http.ResponseWriter, r *http.Request) {
	user, status, message := a.authenticate(r)
	if status != 0 {
		writeJSON(w, r, status, errorEnvelope{Error: message})
		return
	}
	body, status, message := decodeBody(w, r)
	if status != 0 {
		writeJSON(w, r, status, errorEnvelope{Error: message})
		return
	}

	result := a.writes.submit(r.Context(), writeJob{kind: writeCreate, userID: user.id, username: user.username, body: body})
	if result.err != nil {
		writeJSON(w, r, http.StatusInternalServerError, errorEnvelope{Error: "internal server error"})
		return
	}
	writeJSON(w, r, http.StatusCreated, postEnvelope{Post: result.post})
}

func (a *application) like(w http.ResponseWriter, r *http.Request, rawID string) {
	user, status, message := a.authenticate(r)
	if status != 0 {
		writeJSON(w, r, status, errorEnvelope{Error: message})
		return
	}
	id, ok := positiveID(rawID)
	if !ok {
		writeJSON(w, r, http.StatusBadRequest, errorEnvelope{Error: "invalid post id"})
		return
	}

	result := a.writes.submit(r.Context(), writeJob{kind: writeLike, userID: user.id, postID: id})
	if result.notFound {
		writeJSON(w, r, http.StatusNotFound, errorEnvelope{Error: "post not found"})
		return
	}
	if result.err != nil {
		writeJSON(w, r, http.StatusInternalServerError, errorEnvelope{Error: "internal server error"})
		return
	}
	status = http.StatusOK
	inserted := result.inserted
	if inserted {
		status = http.StatusCreated
	}
	writeJSON(w, r, status, likeEnvelope{Liked: true, AlreadyLiked: !inserted, PostID: id})
}

func queryContext(parent context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(parent, databaseTimeout)
}

func scanPost(row interface{ Scan(...any) error }) (postJSON, error) {
	var p postJSON
	var created time.Time
	err := row.Scan(&p.ID, &p.Body, &created, &p.Author, &p.LikeCount)
	if err == nil {
		p.CreatedAt = formatCreated(created)
	}
	return p, err
}

func formatCreated(t time.Time) string {
	return t.UTC().Format(createdLayout)
}

func positiveID(value string) (int64, bool) {
	if value == "" {
		return 0, false
	}
	for i := 0; i < len(value); i++ {
		if value[i] < '0' || value[i] > '9' {
			return 0, false
		}
	}
	id, err := strconv.ParseInt(value, 10, 64)
	return id, err == nil && id > 0
}

func decodeBody(w http.ResponseWriter, r *http.Request) (string, int, string) {
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, maxBodyBytes))
	var raw json.RawMessage
	if err := decoder.Decode(&raw); err != nil {
		if errors.Is(err, io.EOF) {
			return "", http.StatusBadRequest, "malformed JSON body"
		}
		return "", http.StatusBadRequest, "malformed JSON body"
	}
	// A second JSON value makes the request malformed, even if the first one is
	// an otherwise valid object.
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return "", http.StatusBadRequest, "malformed JSON body"
	}

	var fields map[string]json.RawMessage
	if err := json.Unmarshal(raw, &fields); err != nil {
		// Arrays, strings, numbers and null are valid JSON but have no body
		// property, so they use the missing-body error.
		return "", http.StatusBadRequest, "body is required"
	}
	value, exists := fields["body"]
	if !exists {
		return "", http.StatusBadRequest, "body is required"
	}
	var body string
	if err := json.Unmarshal(value, &body); err != nil {
		return "", http.StatusBadRequest, "body is required"
	}
	body = strings.TrimSpace(body)
	if body == "" {
		return "", http.StatusBadRequest, "body is required"
	}
	if utf8.RuneCountInString(body) > maxBodyChars {
		return "", http.StatusBadRequest, "body must be at most 500 characters"
	}
	return body, 0, ""
}

func (a *application) authenticate(r *http.Request) (identity, int, string) {
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, "Bearer ") {
		return identity{}, http.StatusUnauthorized, "missing bearer token"
	}
	token := strings.TrimSpace(header[len("Bearer "):])
	claims, ok := verifyToken(token, a.secret)
	if !ok {
		return identity{}, http.StatusUnauthorized, "invalid or expired token"
	}
	userID, userName, payloadOK := tokenIdentity(claims)
	if !payloadOK {
		return identity{}, http.StatusUnauthorized, "invalid token payload"
	}
	return identity{id: userID, username: userName}, 0, ""
}

// verifyToken checks the compact JWT structure, HS256 algorithm, signature and
// required expiration claim. The returned map is only used for identity fields
// after the cryptographic checks have completed.
func verifyToken(token string, secret []byte) (map[string]json.RawMessage, bool) {
	first := strings.IndexByte(token, '.')
	if first <= 0 {
		return nil, false
	}
	lastRelative := strings.IndexByte(token[first+1:], '.')
	if lastRelative < 1 {
		return nil, false
	}
	second := first + 1 + lastRelative
	if second+1 >= len(token) || strings.IndexByte(token[second+1:], '.') >= 0 {
		return nil, false
	}
	headerPart, payloadPart, signaturePart := token[:first], token[first+1:second], token[second+1:]
	headerJSON, err := base64.RawURLEncoding.DecodeString(headerPart)
	if err != nil {
		return nil, false
	}
	var header struct {
		Algorithm string `json:"alg"`
	}
	if json.Unmarshal(headerJSON, &header) != nil || header.Algorithm != "HS256" {
		return nil, false
	}
	signature, err := base64.RawURLEncoding.DecodeString(signaturePart)
	if err != nil || len(signature) != sha256.Size {
		return nil, false
	}
	mac := hmac.New(sha256.New, secret)
	_, _ = mac.Write([]byte(token[:second]))
	if !hmac.Equal(signature, mac.Sum(nil)) {
		return nil, false
	}
	payloadJSON, err := base64.RawURLEncoding.DecodeString(payloadPart)
	if err != nil {
		return nil, false
	}
	var claims map[string]json.RawMessage
	if err := json.Unmarshal(payloadJSON, &claims); err != nil || claims == nil {
		return nil, false
	}
	var expiry float64
	expiryRaw, exists := claims["exp"]
	if !exists || json.Unmarshal(expiryRaw, &expiry) != nil || math.IsNaN(expiry) || math.IsInf(expiry, 0) || expiry <= float64(time.Now().Unix()) {
		return nil, false
	}
	return claims, true
}

func tokenIdentity(claims map[string]json.RawMessage) (int64, string, bool) {
	var sub string
	if raw, exists := claims["sub"]; !exists || json.Unmarshal(raw, &sub) != nil || sub == "" {
		return 0, "", false
	}
	for i := 0; i < len(sub); i++ {
		if sub[i] < '0' || sub[i] > '9' {
			return 0, "", false
		}
	}
	id, err := strconv.ParseInt(sub, 10, 64)
	if err != nil || id <= 0 {
		return 0, "", false
	}
	var username string
	if raw, exists := claims["username"]; !exists || json.Unmarshal(raw, &username) != nil {
		return 0, "", false
	}
	return id, username, true
}

func writeJSON(w http.ResponseWriter, r *http.Request, status int, value any) {
	body, err := json.Marshal(value)
	if err != nil {
		// Every response type in this server is marshalable. Keep a valid error
		// response if a future type violates that assumption.
		status = http.StatusInternalServerError
		body = []byte(`{"error":"internal server error"}`)
	}
	writeBody(w, r, status, body)
}

func writeBody(w http.ResponseWriter, r *http.Request, status int, body []byte) {
	if len(body) >= 1024 && acceptsGzip(r.Header.Get("Accept-Encoding")) {
		buffer := gzipBuffers.Get().(*bytes.Buffer)
		buffer.Reset()
		writer := gzipWriters.Get().(*gzip.Writer)
		writer.Reset(buffer)
		_, _ = writer.Write(body)
		_ = writer.Close()
		compressed := buffer.Bytes()
		w.Header().Set("Content-Encoding", "gzip")
		w.Header().Add("Vary", "Accept-Encoding")
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Content-Length", strconv.Itoa(len(compressed)))
		w.WriteHeader(status)
		_, _ = w.Write(compressed)
		writer.Reset(io.Discard)
		gzipWriters.Put(writer)
		gzipBuffers.Put(buffer)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(body)))
	w.WriteHeader(status)
	_, _ = w.Write(body)
}

func acceptsGzip(value string) bool {
	for _, item := range strings.Split(value, ",") {
		parts := strings.Split(item, ";")
		if !strings.EqualFold(strings.TrimSpace(parts[0]), "gzip") {
			continue
		}
		for _, parameter := range parts[1:] {
			key, raw, found := strings.Cut(strings.TrimSpace(parameter), "=")
			if found && strings.EqualFold(strings.TrimSpace(key), "q") {
				quality, err := strconv.ParseFloat(strings.TrimSpace(raw), 64)
				if err == nil && quality <= 0 {
					return false
				}
			}
		}
		return true
	}
	return false
}
