package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"hash"
	"math"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf16"
	"unsafe"

	"github.com/bytedance/sonic"
)

const (
	feedSize          = 20
	maxPostBodyLength = 500
	maxRequestBody    = 16 << 10
	// created_at on the wire: UTC, ms. Go's fractional-second layout truncates, never rounds.
	createdAtLayout = "2006-01-02T15:04:05.000Z"
	// Postgres timestamps count microseconds from 2000-01-01 UTC.
	pgEpochUnixMicro = 946684800 * 1_000_000
)

const postSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p
  JOIN users u ON u.id = p.user_id`

// The SPEC's statements, prepared once per connection. Every request runs exactly one of them.
const (
	stmtHealth = iota
	stmtFeed
	stmtPost
	stmtInsertPost
	stmtLike
	stmtCount
)

var stmtSQL = [stmtCount]string{
	stmtHealth:     `SELECT 1`,
	stmtFeed:       postSelect + ` ORDER BY p.created_at DESC, p.id DESC LIMIT $1`,
	stmtPost:       postSelect + ` WHERE p.id = $1`,
	stmtInsertPost: `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`,
	stmtLike:       `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`,
}

func isWrite(stmt uint8) bool { return stmt == stmtInsertPost || stmt == stmtLike }

// request is one parsed HTTP request. Its byte slices point into the loop's read buffer
// and are valid only until the request has been handled.
type request struct {
	method, path, auth, body []byte
	gzip, close              bool
}

// job is a request waiting for its statement. It holds no pointers into the read buffer.
type job struct {
	stmt     uint8
	gzip     bool
	close    bool
	fd       int32
	gen      uint32
	a, b     int64  // statement parameters
	body     string // stmtInsertPost: the trimmed body
	username string // stmtInsertPost: the token's username
}

type app struct {
	jwtSecret []byte
	jwtMAC    sync.Pool
	started   time.Time
}

func newApp(secret []byte) *app {
	a := &app{jwtSecret: secret, started: time.Now()}
	a.jwtMAC.New = func() any { return hmac.New(sha256.New, secret) }
	return a
}

// route answers r directly into out (errors), or returns the statement it needs.
func (a *app) route(r *request, j *job) (needsDB bool, status int, out []byte) {
	j.gzip, j.close = r.gzip, r.close
	get, post := string(r.method) == "GET", string(r.method) == "POST"
	path := r.path
	switch {
	case get && string(path) == "/health":
		j.stmt = stmtHealth
		return true, 0, nil
	case get && string(path) == "/feed":
		j.stmt, j.a = stmtFeed, feedSize
		return true, 0, nil
	case get && isPostPath(path, ""):
		id, ok := parseID(path, "")
		if !ok {
			return false, 400, errorBody("invalid post id")
		}
		j.stmt, j.a = stmtPost, id
		return true, 0, nil
	case post && string(path) == "/posts":
		uid, name, status, body := a.authenticate(r.auth)
		if status != 0 {
			return false, status, body
		}
		text, msg := parsePostBody(r.body)
		if msg != "" {
			return false, 400, errorBody(msg)
		}
		j.stmt, j.a, j.body, j.username = stmtInsertPost, uid, text, name
		return true, 0, nil
	case post && isPostPath(path, "/like"):
		uid, _, status, body := a.authenticate(r.auth)
		if status != 0 {
			return false, status, body
		}
		id, ok := parseID(path, "/like")
		if !ok {
			return false, 400, errorBody("invalid post id")
		}
		j.stmt, j.a, j.b = stmtLike, uid, id
		return true, 0, nil
	}
	return false, 404, errorBody("not found")
}

func isPostPath(path []byte, suffix string) bool {
	if !bytes.HasPrefix(path, []byte("/posts/")) || !bytes.HasSuffix(path, []byte(suffix)) {
		return false
	}
	if len(path)-len(suffix) <= len("/posts/") {
		return false
	}
	id := path[len("/posts/") : len(path)-len(suffix)]
	return len(id) > 0 && bytes.IndexByte(id, '/') < 0
}

func parseID(path []byte, suffix string) (int64, bool) {
	id := path[len("/posts/") : len(path)-len(suffix)]
	n, err := strconv.ParseInt(unsafe.String(unsafe.SliceData(id), len(id)), 10, 64)
	return n, err == nil && n > 0
}

var (
	errMissingToken = errorBody("missing bearer token")
	errInvalidToken = errorBody("invalid or expired token")
	errBadPayload   = errorBody("invalid token payload")
)

// authenticate checks the signature and expiry on every authenticated request.
func (a *app) authenticate(header []byte) (uid int64, name string, status int, body []byte) {
	if !bytes.HasPrefix(header, []byte("Bearer ")) {
		return 0, "", 401, errMissingToken
	}
	raw := strings.TrimSpace(unsafe.String(unsafe.SliceData(header), len(header))[len("Bearer "):])
	// verifyJWT returns strings decoded into new memory, never into raw.
	sub, name, tokenOK, payloadOK := verifyJWT(raw, a.jwtSecret, &a.jwtMAC)
	if !tokenOK {
		return 0, "", 401, errInvalidToken
	}
	if !payloadOK {
		return 0, "", 401, errBadPayload
	}
	id, err := strconv.ParseInt(sub, 10, 64)
	if err != nil || id <= 0 {
		return 0, "", 401, errBadPayload
	}
	return id, name, 0, nil
}

// verifyJWT checks the compact HS256 token without allocating jwt.Token/Header objects.
// It returns tokenOK=false for all signature, encoding, algorithm, and expiration failures;
// payloadOK=false when the claims have the wrong types.
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
	sigText := raw[second+1:]
	var sigBuf [sha256.Size + 3]byte
	if base64.RawURLEncoding.DecodedLen(len(sigText)) > len(sigBuf) {
		return "", "", false, false
	}
	sigLen, err := base64.RawURLEncoding.Decode(sigBuf[:], []byte(sigText))
	if err != nil || sigLen != sha256.Size {
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
	if subtle.ConstantTimeCompare(sigBuf[:sigLen], sum) != 1 {
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

// parsePostBody returns the trimmed body, or the error message for a 400.
func parsePostBody(data []byte) (string, string) {
	var value any
	if len(data) != 0 && sonic.ConfigStd.Unmarshal(data, &value) != nil {
		return "", "malformed JSON body"
	}
	obj, _ := value.(map[string]any)
	body, _ := obj["body"].(string)
	body = strings.TrimSpace(body)
	if body == "" {
		return "", "body is required"
	}
	if len(utf16.Encode([]rune(body))) > maxPostBodyLength {
		return "", "body must be at most 500 characters"
	}
	// The decoder may share memory with data, which is the loop's reusable read buffer.
	return strings.Clone(body), ""
}

func errorBody(msg string) []byte {
	b := append([]byte(`{"error":`), '"')
	b = appendEscaped(b, msg)
	return append(b, `"}`...)
}

// appendPost writes one post object from a binary row of postSelect:
// int8 id, text body, timestamptz created_at, text username, int4 like_count.
func appendPost(b []byte, id int64, body []byte, createdMicros int64, author []byte, likes int64) []byte {
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, `,"body":"`...)
	b = appendEscapedBytes(b, body)
	b = append(b, `","created_at":"`...)
	b = appendPgTime(b, createdMicros)
	b = append(b, `","author":"`...)
	b = appendEscapedBytes(b, author)
	b = append(b, `","like_count":`...)
	b = strconv.AppendInt(b, likes, 10)
	return append(b, '}')
}

// appendPgTime writes a Postgres timestamp (µs since 2000-01-01 UTC) as
// 2006-01-02T15:04:05.000Z, truncated to milliseconds, without the time package's
// general-purpose formatter.
func appendPgTime(b []byte, micros int64) []byte {
	ms := floorDiv(micros+pgEpochUnixMicro, 1000)
	secs := floorDiv(ms, 1000)
	days := floorDiv(secs, 86400)
	sod := secs - days*86400
	y, m, d := civilFromDays(days)
	b = append(b, byte('0'+y/1000%10), byte('0'+y/100%10), byte('0'+y/10%10), byte('0'+y%10), '-')
	b = append2(b, m)
	b = append(b, '-')
	b = append2(b, d)
	b = append(b, 'T')
	b = append2(b, int(sod/3600))
	b = append(b, ':')
	b = append2(b, int(sod/60%60))
	b = append(b, ':')
	b = append2(b, int(sod%60))
	f := int(ms - secs*1000)
	return append(b, '.', byte('0'+f/100), byte('0'+f/10%10), byte('0'+f%10), 'Z')
}

func append2(b []byte, v int) []byte { return append(b, byte('0'+v/10), byte('0'+v%10)) }

func floorDiv(a, b int64) int64 {
	q := a / b
	if a%b != 0 && a < 0 {
		q--
	}
	return q
}

// civilFromDays converts days since 1970-01-01 to a proleptic Gregorian date
// (Howard Hinnant's algorithm).
func civilFromDays(z int64) (year, month, day int) {
	z += 719468
	era := floorDiv(z, 146097)
	doe := z - era*146097
	yoe := (doe - doe/1460 + doe/36524 - doe/146096) / 365
	y := yoe + era*400
	doy := doe - (365*yoe + yoe/4 - yoe/100)
	mp := (5*doy + 2) / 153
	d := doy - (153*mp+2)/5 + 1
	m := mp + 3
	if mp >= 10 {
		m = mp - 9
	}
	if m <= 2 {
		y++
	}
	return int(y), int(m), int(d)
}

func formatTime(t time.Time) string { return t.UTC().Format(createdAtLayout) }

const hexDigits = "0123456789abcdef"

func appendEscaped(b []byte, s string) []byte {
	return appendEscapedBytes(b, unsafe.Slice(unsafe.StringData(s), len(s)))
}

// appendEscapedBytes writes s as the inside of a JSON string. Non-ASCII is kept as UTF-8
// (the SPEC makes escaping it optional); quotes, backslashes and control characters are escaped.
func appendEscapedBytes(b []byte, s []byte) []byte {
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		b = append(b, s[start:i]...)
		switch c {
		case '"', '\\':
			b = append(b, '\\', c)
		case '\n':
			b = append(b, '\\', 'n')
		case '\r':
			b = append(b, '\\', 'r')
		case '\t':
			b = append(b, '\\', 't')
		default:
			b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xf])
		}
		start = i + 1
	}
	return append(b, s[start:]...)
}
