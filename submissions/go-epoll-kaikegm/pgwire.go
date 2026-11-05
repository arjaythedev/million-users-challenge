package main

import (
	"bytes"
	"encoding/binary"
)

// The SPEC's statements, prepared once per connection as "s0".."s4".
// Every request runs exactly one of them.
const (
	stmtHealth uint8 = iota
	stmtFeed
	stmtPost
	stmtInsertPost
	stmtLike
	stmtCount
)

var stmtSQL = [stmtCount]string{
	stmtHealth:     `SELECT 1`,
	stmtFeed:       feedSelect,
	stmtPost:       getPostSelect,
	stmtInsertPost: createPostQuery,
	stmtLike:       likePostQuery,
}

func isWriteStmt(stmt uint8) bool {
	return stmt == stmtInsertPost || stmt == stmtLike
}

// resultFormats are each statement's Bind result format codes (0 text, 1 binary), whichever
// is smaller per column: ids and like counts are shorter as digits than as 8- and 4-byte
// integers and go into the JSON as they are; created_at is 8 bytes binary against 29 as text.
var resultFormats = [stmtCount][]byte{
	stmtHealth:     {0, 0},
	stmtFeed:       {0, 5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
	stmtPost:       {0, 5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0},
	stmtInsertPost: {0, 2, 0, 0, 0, 1},
	stmtLike:       {0, 0},
}

// Pre-built Bind+Execute wire templates for fixed-size parameter queries to avoid
// rebuilding headers per request.
var (
	wireHealth = appendBindExecute(nil, stmtHealth)
	wireFeed   = appendBindExecute(nil, stmtFeed)
)

// appendBindExecute queues one prepared statement: Bind (binary parameters, results as in
// resultFormats) and Execute on the unnamed portal. No Describe: the row shapes are fixed.
func appendBindExecute(b []byte, stmt uint8, params ...[]byte) []byte {
	start := len(b)
	b = append(b, 'B', 0, 0, 0, 0)
	b = append(b, 0)                // unnamed portal
	b = append(b, 's', '0'+stmt, 0) // statement name
	b = append(b, 0, 1, 0, 1)       // one parameter format code: binary
	b = binary.BigEndian.AppendUint16(b, uint16(len(params)))
	for _, p := range params {
		b = binary.BigEndian.AppendUint32(b, uint32(len(p)))
		b = append(b, p...)
	}
	b = append(b, resultFormats[stmt]...)
	binary.BigEndian.PutUint32(b[start+1:], uint32(len(b)-start-1))
	// Execute: unnamed portal, no row limit.
	return append(b, 'E', 0, 0, 0, 9, 0, 0, 0, 0, 0)
}

func appendSync(b []byte) []byte { return append(b, 'S', 0, 0, 0, 4) }

// nextMessage splits one backend message off buf: its type, its payload, and the bytes used.
// n == 0 means the message is incomplete.
func nextMessage(buf []byte) (typ byte, payload []byte, n int) {
	if len(buf) < 5 {
		return 0, nil, 0
	}
	size := int(binary.BigEndian.Uint32(buf[1:5]))
	if size < 4 || len(buf) < 1+size {
		return 0, nil, 0
	}
	return buf[0], buf[5 : 1+size], 1 + size
}

// rowColumns splits a DataRow payload into its columns (a NULL is a nil slice).
func rowColumns(payload []byte, cols [][]byte) ([][]byte, bool) {
	if len(payload) < 2 {
		return nil, false
	}
	n := int(binary.BigEndian.Uint16(payload))
	p := payload[2:]
	cols = cols[:0]
	for range n {
		if len(p) < 4 {
			return nil, false
		}
		l := int(int32(binary.BigEndian.Uint32(p)))
		p = p[4:]
		if l < 0 {
			cols = append(cols, nil)
			continue
		}
		if len(p) < l {
			return nil, false
		}
		cols = append(cols, p[:l])
		p = p[l:]
	}
	return cols, true
}

// rowsAffected reads the count at the end of a CommandComplete tag ("INSERT 0 1").
func rowsAffected(payload []byte) int64 {
	tag := bytes.TrimRight(payload, "\x00")
	if len(tag) > 0 && tag[len(tag)-1] == '1' {
		return 1
	}
	return 0
}

// errorFields returns the SQLSTATE and message of an ErrorResponse.
func errorFields(payload []byte) (code, msg string) {
	for len(payload) > 0 && payload[0] != 0 {
		field := payload[0]
		end := bytes.IndexByte(payload[1:], 0)
		if end < 0 {
			break
		}
		value := payload[1 : 1+end]
		switch field {
		case 'C':
			code = string(value)
		case 'M':
			msg = string(value)
		}
		payload = payload[2+end:]
	}
	return code, msg
}
