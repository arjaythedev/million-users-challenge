package main

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"hash"
	"io"
	"math"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"

	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	feedLimit       = 20
	maxBodyBytes    = 16 << 10
	maxBodyChars    = 500
	gzipMinBytes    = 1024
	databaseTimeout = 5 * time.Second
)

const postQuery = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id`

type application struct {
	db      *pgxpool.Pool
	reads   *readBatcher
	writes  *writeBatcher
	secret  []byte
	macs    sync.Pool
	started time.Time
}

type identity struct {
	id       int64
	username string
}

var (
	bodyNotFound       = []byte(`{"error":"not found"}`)
	bodyPostNotFound   = []byte(`{"error":"post not found"}`)
	bodyInvalidID      = []byte(`{"error":"invalid post id"}`)
	bodyInternal       = []byte(`{"error":"internal server error"}`)
	bodyMissingBearer  = []byte(`{"error":"missing bearer token"}`)
	bodyInvalidToken   = []byte(`{"error":"invalid or expired token"}`)
	bodyInvalidPayload = []byte(`{"error":"invalid token payload"}`)
	bodyMalformed      = []byte(`{"error":"malformed JSON body"}`)
	bodyRequired       = []byte(`{"error":"body is required"}`)
	bodyTooLong        = []byte(`{"error":"body must be at most 500 characters"}`)
)

// respond appends the complete HTTP response for req to wire.
func (a *application) respond(wire *[]byte, req *request) *[]byte {
	scratch := getBuffer()
	status, body := a.handle(scratch, req)
	if req.gzip && len(body) >= gzipMinBytes {
		compressed := getBuffer()
		*compressed = gzipAppend(*compressed, body)
		*wire = appendResponse(*wire, status, *compressed, true, req.close)
		putBuffer(compressed)
	} else {
		*wire = appendResponse(*wire, status, body, false, req.close)
	}
	putBuffer(scratch)
	return wire
}

// handle returns the status and JSON body. A body built here is appended to
// scratch; constant bodies are returned directly.
func (a *application) handle(scratch *[]byte, req *request) (int, []byte) {
	switch req.route {
	case routeHealth:
		return a.health(scratch)
	case routeFeed:
		return a.read(scratch, 0)
	case routeGetPost:
		if !req.idOK {
			return 400, bodyInvalidID
		}
		return a.read(scratch, req.id)
	case routeCreate:
		return a.create(scratch, req)
	case routeLike:
		return a.like(scratch, req)
	default:
		return 404, bodyNotFound
	}
}

func (a *application) health(scratch *[]byte) (int, []byte) {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	if _, err := a.db.Exec(ctx, "SELECT 1"); err != nil {
		body, _ := json.Marshal(struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Error  string `json:"error"`
		}{"degraded", "unreachable", err.Error()})
		return 503, body
	}
	b := append((*scratch)[:0], `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(a.started)/time.Second), 10)
	*scratch = append(b, '}')
	return 200, *scratch
}

func (a *application) read(scratch *[]byte, id int64) (int, []byte) {
	result := a.reads.submit(id, *scratch)
	*scratch = result.body
	switch {
	case result.err != nil:
		return 500, bodyInternal
	case result.notFound:
		return 404, bodyPostNotFound
	}
	return 200, result.body
}

func (a *application) create(scratch *[]byte, req *request) (int, []byte) {
	user, status, message := a.authenticate(req)
	if status != 0 {
		return status, message
	}
	body, message := decodeBody(req.body)
	if message != nil {
		return 400, message
	}
	result := a.writes.submit(writeJob{kind: writeCreate, userID: user.id, body: body})
	if result.err != nil {
		return 500, bodyInternal
	}
	b := append((*scratch)[:0], `{"post":`...)
	b = appendPost(b, result.postID, body, result.createdMicros, user.username, 0)
	*scratch = append(b, '}')
	return 201, *scratch
}

func (a *application) like(scratch *[]byte, req *request) (int, []byte) {
	user, status, message := a.authenticate(req)
	if status != 0 {
		return status, message
	}
	if !req.idOK {
		return 400, bodyInvalidID
	}
	result := a.writes.submit(writeJob{kind: writeLike, userID: user.id, postID: req.id})
	if result.notFound {
		return 404, bodyPostNotFound
	}
	if result.err != nil {
		return 500, bodyInternal
	}
	status = 200
	b := append((*scratch)[:0], `{"liked":true,"already_liked":`...)
	if result.inserted {
		status = 201
		b = append(b, "false"...)
	} else {
		b = append(b, "true"...)
	}
	b = append(b, `,"post_id":`...)
	b = strconv.AppendInt(b, req.id, 10)
	*scratch = append(b, '}')
	return status, *scratch
}

func decodeBody(raw []byte) (string, []byte) {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	var value json.RawMessage
	if err := decoder.Decode(&value); err != nil {
		return "", bodyMalformed
	}
	// A second JSON value makes the request malformed, even if the first one is
	// an otherwise valid object.
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		return "", bodyMalformed
	}
	var fields map[string]json.RawMessage
	if err := json.Unmarshal(value, &fields); err != nil {
		// Arrays, strings, numbers and null are valid JSON but have no body
		// property, so they use the missing-body error.
		return "", bodyRequired
	}
	field, exists := fields["body"]
	if !exists {
		return "", bodyRequired
	}
	var body string
	if err := json.Unmarshal(field, &body); err != nil {
		return "", bodyRequired
	}
	body = strings.TrimSpace(body)
	if body == "" {
		return "", bodyRequired
	}
	if utf8.RuneCountInString(body) > maxBodyChars {
		return "", bodyTooLong
	}
	return body, nil
}

func (a *application) authenticate(req *request) (identity, int, []byte) {
	if !req.hasAuth || !strings.HasPrefix(req.auth, "Bearer ") {
		return identity{}, 401, bodyMissingBearer
	}
	token := strings.TrimSpace(req.auth[len("Bearer "):])
	claims, ok := a.verifyToken(token)
	if !ok {
		return identity{}, 401, bodyInvalidToken
	}
	userID, userName, payloadOK := tokenIdentity(claims)
	if !payloadOK {
		return identity{}, 401, bodyInvalidPayload
	}
	return identity{id: userID, username: userName}, 0, nil
}

// verifyToken checks the compact JWT structure, HS256 algorithm, signature and
// required expiration claim on every call. The returned map is only used for
// identity fields after the cryptographic checks have completed. Pooled HMAC
// states hold the key schedule, never a token or its verification result.
func (a *application) verifyToken(token string) (map[string]json.RawMessage, bool) {
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
	// An HS256 signature is 32 bytes: exactly 43 unpadded base64url characters.
	if len(signaturePart) != base64.RawURLEncoding.EncodedLen(sha256.Size) {
		return nil, false
	}
	var signature [sha256.Size]byte
	n, err := base64.RawURLEncoding.Decode(signature[:], []byte(signaturePart))
	if err != nil || n != sha256.Size {
		return nil, false
	}
	mac, _ := a.macs.Get().(hash.Hash)
	if mac == nil {
		mac = hmac.New(sha256.New, a.secret)
	}
	mac.Reset()
	_, _ = io.WriteString(mac, token[:second])
	var sum [sha256.Size]byte
	valid := hmac.Equal(signature[:], mac.Sum(sum[:0]))
	a.macs.Put(mac)
	if !valid {
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
	id, ok := positiveID([]byte(sub))
	if !ok {
		return 0, "", false
	}
	var username string
	raw, exists := claims["username"]
	raw = bytes.TrimSpace(raw)
	if !exists || len(raw) == 0 || raw[0] != '"' || json.Unmarshal(raw, &username) != nil {
		return 0, "", false
	}
	return id, username, true
}

// Compression state is retained only in pools. Idle keep-alive connections do
// not retain a gzip writer or response buffer.
type gzipState struct {
	writer *gzip.Writer
	out    appendWriter
}

type appendWriter struct{ b []byte }

func (w *appendWriter) Write(p []byte) (int, error) { w.b = append(w.b, p...); return len(p), nil }

var gzipStates = sync.Pool{New: func() any {
	state := &gzipState{}
	writer, err := gzip.NewWriterLevel(&state.out, gzip.BestSpeed)
	if err != nil {
		panic(err)
	}
	state.writer = writer
	return state
}}

func gzipAppend(dst, body []byte) []byte {
	state := gzipStates.Get().(*gzipState)
	state.out.b = dst
	state.writer.Reset(&state.out)
	_, _ = state.writer.Write(body)
	_ = state.writer.Close()
	dst = state.out.b
	state.out.b = nil
	gzipStates.Put(state)
	return dst
}

var errUnknownWrite = errors.New("unknown write kind")
