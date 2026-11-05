package main

import (
	"bytes"
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
	"github.com/panjf2000/gnet/v2"
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
	// responses carries completed exchanges to the responders.
	responses chan *exchange
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

// exchange is one request on its way from the event loop to its response. The
// event loop starts it; a read pipeline, a write lane or the health check
// completes it with a status and body; a responder encodes and sends it. No
// goroutine waits on the database for a request.
type exchange struct {
	c   gnet.Conn
	req *request
	// The response: body may live in scratch, a pooled buffer released once
	// the response is encoded. Constant bodies leave scratch nil.
	status  int
	body    []byte
	scratch *[]byte
	// For writes: the authenticated user and, for a new post, its text.
	user identity
	text string
}

// responseQueue bounds completed exchanges waiting for a responder.
const responseQueue = 16384

// complete hands a finished exchange to the responders.
func (a *application) complete(ex *exchange, status int, body []byte, scratch *[]byte) {
	ex.status, ex.body, ex.scratch = status, body, scratch
	a.responses <- ex
}

// start routes a parsed request. It runs on the event loop, so it only checks
// and queues work: database operations complete the exchange later.
func (a *application) start(ex *exchange) {
	req := ex.req
	switch req.route {
	case routeHealth:
		go a.health(ex)
	case routeFeed:
		a.reads.enqueue(readJob{ex: ex})
	case routeGetPost:
		if !req.idOK {
			a.complete(ex, 400, bodyInvalidID, nil)
			return
		}
		a.reads.enqueue(readJob{id: req.id, ex: ex})
	case routeCreate:
		a.create(ex)
	case routeLike:
		a.like(ex)
	default:
		a.complete(ex, 404, bodyNotFound, nil)
	}
}

// encode builds the HTTP response. Queued requests retain no response payload:
// database workers acquire body buffers only when producing rows, and the wire
// buffer is acquired here, after completion.
func encode(ex *exchange) *[]byte {
	wire := getBuffer()
	if ex.req.gzip && len(ex.body) >= gzipMinBytes {
		compressed := getBuffer()
		*compressed = gzipAppend(*compressed, ex.body)
		*wire = appendResponse(*wire, ex.status, *compressed, true, ex.req.close)
		putBuffer(compressed)
	} else {
		*wire = appendResponse(*wire, ex.status, ex.body, false, ex.req.close)
	}
	putBuffer(ex.scratch)
	ex.body, ex.scratch = nil, nil
	return wire
}

func (a *application) health(ex *exchange) {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	if _, err := a.db.Exec(ctx, "SELECT 1"); err != nil {
		body, _ := json.Marshal(struct {
			Status string `json:"status"`
			DB     string `json:"db"`
			Error  string `json:"error"`
		}{"degraded", "unreachable", err.Error()})
		a.complete(ex, 503, body, nil)
		return
	}
	buffer := getBuffer()
	b := append((*buffer)[:0], `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(a.started)/time.Second), 10)
	*buffer = append(b, '}')
	a.complete(ex, 200, *buffer, buffer)
}

// readDone completes a read with its rows, or an error.
func (a *application) readDone(ex *exchange, result readResult) {
	switch {
	case result.err != nil:
		putBuffer(result.buffer)
		a.complete(ex, 500, bodyInternal, nil)
	case result.notFound:
		putBuffer(result.buffer)
		a.complete(ex, 404, bodyPostNotFound, nil)
	default:
		a.complete(ex, 200, *result.buffer, result.buffer)
	}
}

func (a *application) create(ex *exchange) {
	user, status, message := a.authenticate(ex.req)
	if status != 0 {
		a.complete(ex, status, message, nil)
		return
	}
	body, message := decodeBody(ex.req.body)
	if message != nil {
		a.complete(ex, 400, message, nil)
		return
	}
	ex.user, ex.text = user, body
	a.writes.enqueue(writeJob{kind: writeCreate, userID: user.id, body: body, ex: ex})
}

func (a *application) like(ex *exchange) {
	user, status, message := a.authenticate(ex.req)
	if status != 0 {
		a.complete(ex, status, message, nil)
		return
	}
	if !ex.req.idOK {
		a.complete(ex, 400, bodyInvalidID, nil)
		return
	}
	ex.user = user
	a.writes.enqueue(writeJob{kind: writeLike, userID: user.id, postID: ex.req.id, ex: ex})
}

// writeDone completes a write once its group has committed, or failed.
func (a *application) writeDone(job writeJob, result writeResult) {
	ex := job.ex
	if job.kind == writeLike && result.notFound {
		a.complete(ex, 404, bodyPostNotFound, nil)
		return
	}
	if result.err != nil {
		a.complete(ex, 500, bodyInternal, nil)
		return
	}
	buffer := getBuffer()
	b := (*buffer)[:0]
	status := 201
	if job.kind == writeCreate {
		b = append(b, `{"post":`...)
		b = appendPost(b, result.postID, ex.text, result.createdMicros, ex.user.username, 0)
		b = append(b, '}')
	} else {
		b = append(b, `{"liked":true,"already_liked":`...)
		if result.inserted {
			b = append(b, "false"...)
		} else {
			status = 200
			b = append(b, "true"...)
		}
		b = append(b, `,"post_id":`...)
		b = strconv.AppendInt(b, job.postID, 10)
		b = append(b, '}')
	}
	*buffer = b
	a.complete(ex, status, b, buffer)
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

var errUnknownWrite = errors.New("unknown write kind")
