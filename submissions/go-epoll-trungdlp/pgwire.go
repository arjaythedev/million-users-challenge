package main

import (
	"bytes"
	"encoding/binary"
	"strconv"
)

// The PostgreSQL extended-query messages this server sends, after pgconn has connected,
// authenticated and prepared the statements as "s0".."s4".

// appendBindExecute queues one prepared statement: Bind (binary parameters and results)
// and Execute on the unnamed portal. No Describe: the row shapes are fixed.
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
	b = append(b, 0, 1, 0, 1) // one result format code: binary
	binary.BigEndian.PutUint32(b[start+1:], uint32(len(b)-start-1))
	// Execute: unnamed portal, no row limit.
	return append(b, 'E', 0, 0, 0, 9, 0, 0, 0, 0, 0)
}

func appendSync(b []byte) []byte { return append(b, 'S', 0, 0, 0, 4) }

func int8Param(v int64) []byte { return binary.BigEndian.AppendUint64(nil, uint64(v)) }

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
	i := bytes.LastIndexByte(tag, ' ')
	n, _ := strconv.ParseInt(string(tag[i+1:]), 10, 64)
	return n
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
