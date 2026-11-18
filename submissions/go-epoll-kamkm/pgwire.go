package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"strconv"
	"syscall"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
	"golang.org/x/sys/unix"
)

// The hot path speaks the PostgreSQL extended-query protocol directly. pgconn
// opens each connection (authentication, session settings) and prepares the
// statements; an event loop then takes the socket over (pgloop.go) and every
// batch is one write of Bind/Execute pairs closed by one Sync, and one pass
// over the reply.
//
// Nothing asks the server to describe a result, so no RowDescription comes
// back: the columns of each prepared statement are known here, and Bind asks
// for each in the format that is cheapest to turn into JSON (numbers as
// decimal text, created_at as a binary timestamp).

const postQuery = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id`

const (
	feedSQL   = postQuery + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
	postSQL   = postQuery + " WHERE p.id = $1"
	createSQL = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
	likeSQL   = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"

	// PostgreSQL timestamps count microseconds from 2000-01-01 UTC.
	postgresEpochMicros = 946684800 * 1000000

	writeBatchLimit = 64
	databaseTimeout = 5 * time.Second
)

const (
	statementFeed   = "f"
	statementPost   = "p"
	statementCreate = "c"
	statementLike   = "l"
	statementHealth = "h"

	wireReadBuffer = 256 << 10
	maxWireMessage = 1 << 20
	redialInterval = 250 * time.Millisecond

	formatText   = 0
	formatBinary = 1
)

var preparedStatements = [...]struct{ name, sql string }{
	{statementFeed, feedSQL},
	{statementPost, postSQL},
	{statementCreate, createSQL},
	{statementLike, likeSQL},
	{statementHealth, "SELECT 1"},
}

var (
	// id, body, created_at, username, like_count.
	postFormats = []byte{formatText, formatText, formatBinary, formatText, formatText}
	// id, created_at.
	createdFormats = []byte{formatText, formatBinary}

	feedMessage   = bindWithoutParameters(statementFeed, postFormats...)
	healthMessage = bindWithoutParameters(statementHealth, formatText)
	syncMessage   = []byte{'S', 0, 0, 0, 4}

	errProtocol  = errors.New("postgres: unexpected protocol message")
	errPlainOnly = errors.New("postgres: TLS sessions are not supported; use sslmode=disable or sslmode=prefer")
)

// serverError is an ErrorResponse: the statement failed and the server skips
// the rest of the batch up to its Sync.
type serverError struct {
	code       string
	constraint string
	message    string
}

func (e *serverError) Error() string { return "postgres: " + e.message + " (SQLSTATE " + e.code + ")" }

func parseServerError(body []byte) *serverError {
	e := &serverError{}
	for len(body) > 1 {
		field := body[0]
		end := bytes.IndexByte(body[1:], 0)
		if end < 0 {
			break
		}
		value := string(body[1 : 1+end])
		body = body[end+2:]
		switch field {
		case 'C':
			e.code = value
		case 'n':
			e.constraint = value
		case 'M':
			e.message = value
		}
	}
	return e
}

// endsSession reports whether an ErrorResponse is FATAL or PANIC: the server
// closes the connection after sending it.
func endsSession(body []byte) bool {
	for len(body) > 1 {
		field := body[0]
		end := bytes.IndexByte(body[1:], 0)
		if end < 0 {
			break
		}
		value := body[1 : 1+end]
		body = body[end+2:]
		// 'V' is the severity in English; 'S' may be translated.
		if field == 'V' {
			return string(value) == "FATAL" || string(value) == "PANIC"
		}
	}
	return false
}

// dialDatabase opens a session, prepares the statements and returns the
// connected socket, non-blocking, for an event loop to drive.
func dialDatabase(config *pgconn.Config) (int, error) {
	ctx, cancel := context.WithTimeout(context.Background(), databaseTimeout)
	defer cancel()
	session, err := pgconn.ConnectConfig(ctx, config)
	if err != nil {
		return -1, err
	}
	for _, statement := range preparedStatements {
		if _, err := session.Prepare(ctx, statement.name, statement.sql, nil); err != nil {
			_ = session.Close(ctx)
			return -1, err
		}
	}
	// Nothing may be left unread in pgconn's buffers before the socket is
	// taken over.
	if err := session.SyncConn(ctx); err != nil {
		_ = session.Close(ctx)
		return -1, err
	}
	hijacked, err := session.Hijack()
	if err != nil {
		_ = session.Close(ctx)
		return -1, err
	}
	defer hijacked.Conn.Close()
	socket, ok := hijacked.Conn.(syscall.Conn)
	if !ok {
		return -1, errPlainOnly
	}
	raw, err := socket.SyscallConn()
	if err != nil {
		return -1, err
	}
	// The loop keeps its own descriptor for the socket; closing pgconn's
	// leaves the connection open.
	fd := -1
	if controlErr := raw.Control(func(descriptor uintptr) {
		fd, err = unix.FcntlInt(descriptor, unix.F_DUPFD_CLOEXEC, 0)
	}); controlErr != nil {
		return -1, controlErr
	}
	if err != nil {
		return -1, err
	}
	if err := unix.SetNonblock(fd, true); err != nil {
		_ = unix.Close(fd)
		return -1, err
	}
	return fd, nil
}

// plainOnly makes config connect without TLS. The loops read and write the
// socket themselves, so a session cannot be wrapped in TLS; a connection
// string that insists on TLS is refused instead of silently downgraded.
func plainOnly(config *pgconn.Config) error {
	if config.TLSConfig == nil {
		config.Fallbacks = nil
		return nil
	}
	for _, fallback := range config.Fallbacks {
		if fallback.TLSConfig == nil {
			config.Host, config.Port, config.TLSConfig, config.Fallbacks = fallback.Host, fallback.Port, nil, nil
			return nil
		}
	}
	return errPlainOnly
}

// appendBindHead starts a Bind of a prepared statement to the unnamed portal
// with text parameters. It returns the offset of the message length, which
// appendBindTail fills in.
func appendBindHead(out []byte, statement string, parameters int) ([]byte, int) {
	out = append(out, 'B', 0, 0, 0, 0)
	start := len(out) - 4
	out = append(out, 0)
	out = append(out, statement...)
	out = append(out, 0, 0, 0)
	out = binary.BigEndian.AppendUint16(out, uint16(parameters))
	return out, start
}

func appendIntParameter(out []byte, value int64) []byte {
	out = append(out, 0, 0, 0, 0)
	start := len(out)
	out = strconv.AppendInt(out, value, 10)
	binary.BigEndian.PutUint32(out[start-4:], uint32(len(out)-start))
	return out
}

func appendTextParameter(out []byte, value string) []byte {
	out = binary.BigEndian.AppendUint32(out, uint32(len(value)))
	return append(out, value...)
}

// appendBindTail names the result formats, closes the Bind and appends an
// Execute of the unnamed portal with no row limit.
func appendBindTail(out []byte, start int, formats ...byte) []byte {
	out = binary.BigEndian.AppendUint16(out, uint16(len(formats)))
	for _, format := range formats {
		out = append(out, 0, format)
	}
	binary.BigEndian.PutUint32(out[start:], uint32(len(out)-start))
	return append(out, 'E', 0, 0, 0, 9, 0, 0, 0, 0, 0)
}

func bindWithoutParameters(statement string, formats ...byte) []byte {
	out, start := appendBindHead(nil, statement, 0)
	return appendBindTail(out, start, formats...)
}

func appendPostQuery(out []byte, id int64) []byte {
	out, start := appendBindHead(out, statementPost, 1)
	out = appendIntParameter(out, id)
	return appendBindTail(out, start, postFormats...)
}

func appendCreateQuery(out []byte, userID int64, body string) []byte {
	out, start := appendBindHead(out, statementCreate, 2)
	out = appendIntParameter(out, userID)
	out = appendTextParameter(out, body)
	return appendBindTail(out, start, createdFormats...)
}

func appendLikeQuery(out []byte, userID, postID int64) []byte {
	out, start := appendBindHead(out, statementLike, 2)
	out = appendIntParameter(out, userID)
	out = appendIntParameter(out, postID)
	return appendBindTail(out, start)
}

// rowColumn splits the next column off a DataRow body. A NULL has ok false.
func rowColumn(row []byte) (value, rest []byte, ok bool) {
	if len(row) < 4 {
		return nil, nil, false
	}
	size := int(int32(binary.BigEndian.Uint32(row)))
	if size < 0 || size > len(row)-4 {
		return nil, nil, false
	}
	return row[4 : 4+size], row[4+size:], true
}

func isDecimal(value []byte) bool {
	if len(value) == 0 || len(value) > 20 {
		return false
	}
	for i, c := range value {
		if (c < '0' || c > '9') && (i != 0 || c != '-' || len(value) == 1) {
			return false
		}
	}
	return true
}

// appendPostRow appends one post row (see postFormats) as a JSON object with
// the API's key order.
func appendPostRow(dst, row []byte) ([]byte, bool) {
	if len(row) < 2 || binary.BigEndian.Uint16(row) != 5 {
		return dst, false
	}
	id, row, ok1 := rowColumn(row[2:])
	text, row, ok2 := rowColumn(row)
	created, row, ok3 := rowColumn(row)
	author, row, ok4 := rowColumn(row)
	likes, _, ok5 := rowColumn(row)
	if !(ok1 && ok2 && ok3 && ok4 && ok5) || len(created) != 8 || !isDecimal(id) || !isDecimal(likes) {
		return dst, false
	}
	dst = append(dst, `{"id":`...)
	dst = append(dst, id...)
	dst = append(dst, `,"body":`...)
	dst = appendString(dst, text)
	dst = append(dst, `,"created_at":"`...)
	dst = appendCreated(dst, int64(binary.BigEndian.Uint64(created))+postgresEpochMicros)
	dst = append(dst, `","author":`...)
	dst = appendString(dst, author)
	dst = append(dst, `,"like_count":`...)
	dst = append(dst, likes...)
	return append(dst, '}'), true
}
