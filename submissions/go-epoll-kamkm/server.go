package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"hash"
	"io"
	"math"
	"strings"
	"sync"
	"sync/atomic"
	"time"
	"unicode/utf8"
)

const (
	feedLimit    = 20
	maxBodyBytes = 16 << 10
	maxBodyChars = 500
	gzipMinBytes = 1024
)

type application struct {
	secret  []byte
	macs    sync.Pool
	started time.Time
	plan    sessionPlan
	// dial opens a PostgreSQL session and returns its socket.
	dial func() (int, error)
	// trace turns on the loops' slow-pass log (LOOP_TRACE=1).
	trace bool
	// stop closes when the process is asked to shut down; stopping tells the
	// loops, once woken, to stop accepting and finish what is in flight.
	stop     <-chan struct{}
	stopping atomic.Bool
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
