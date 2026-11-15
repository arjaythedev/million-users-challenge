package main

// Response assembly: status line, headers, and compact JSON with the exact
// key order from SPEC.md. Every body is built hdrRoom bytes into its buffer so
// the header lands right in front of it: one contiguous block, no copy.

import (
	"encoding/binary"
	"strconv"
	"time"
)

const (
	hdrRoom      = 192  // reserved header space in front of every body
	gzipMinBytes = 1024 // only the feed; a single post saves no TCP segment
)

var statusLines = map[int]string{
	200: "200 OK", 201: "201 Created", 400: "400 Bad Request", 401: "401 Unauthorized",
	404: "404 Not Found", 500: "500 Internal Server Error", 503: "503 Service Unavailable",
}

// headerPrefix[status index][gzip][close] is every header up to the
// Content-Length digits.
var headerPrefix = func() (t [7][2][2]string) {
	for i, code := range []int{200, 201, 400, 401, 404, 500, 503} {
		for gz := 0; gz < 2; gz++ {
			for cl := 0; cl < 2; cl++ {
				t[i][gz][cl] = buildPrefix(code, gz == 1, cl == 1)
			}
		}
	}
	return t
}()

func buildPrefix(code int, gz, closeConn bool) string {
	s := "HTTP/1.1 " + statusLines[code] + "\r\nContent-Type: application/json\r\n"
	if gz {
		s += "Content-Encoding: gzip\r\n"
	}
	if closeConn {
		s += "Connection: close\r\n"
	}
	return s + "Content-Length: "
}

func b2i(v bool) int {
	if v {
		return 1
	}
	return 0
}

func prefixFor(code int, gz, closeConn bool) string {
	i := 0
	switch code {
	case 200:
	case 201:
		i = 1
	case 400:
		i = 2
	case 401:
		i = 3
	case 404:
		i = 4
	case 503:
		i = 6
	default:
		i = 5
	}
	return headerPrefix[i][b2i(gz)][b2i(closeConn)]
}

// frame writes the headers into the room in front of the body b[hdrRoom:] and
// returns the contiguous response.
func frame(b []byte, code int, gz, closeConn bool) []byte {
	n := len(b) - hdrRoom
	at := hdrRoom - 4
	copy(b[at:], "\r\n\r\n")
	for {
		at--
		b[at] = byte('0' + n%10)
		n /= 10
		if n == 0 {
			break
		}
	}
	p := prefixFor(code, gz, closeConn)
	at -= len(p)
	copy(b[at:], p)
	return b[at:]
}

// errorResponse frames a fixed body into dst's memory.
func errorResponse(dst []byte, code int, body []byte, closeConn bool) ([]byte, []byte) {
	dst = append(dst[:hdrRoom], body...)
	return dst, frame(dst, code, false, closeConn)
}

// immediateBody picks the fixed error body of an error-path request.
func immediateBody(p *parsed) []byte {
	switch p.status {
	case 400:
		switch p.reason {
		case "invalid post id":
			return errInvalidID
		case "malformed JSON body":
			return errBadJSON
		case "body is required":
			return errBodyRequired
		case "body must be at most 500 characters":
			return errBodyTooLong
		}
	case 401:
		switch p.reason {
		case "missing bearer token":
			return errMissingBearer
		case "invalid token payload":
			return errBadPayload
		}
		return errBadToken
	}
	return errNotFound
}

// ---- JSON building ----

// needsEscape reports whether any byte of the 8 is a control character, a
// quote or a backslash (false positives possible only after a true one).
func needsEscape(w uint64) bool {
	const lo, hi = 0x0101010101010101, 0x8080808080808080
	ctl := (w - lo*0x20) &^ w
	q := w ^ (lo * '"')
	bs := w ^ (lo * '\\')
	return (ctl|(q-lo)&^q|(bs-lo)&^bs)&hi != 0
}

func appendJSONString(b []byte, s []byte) []byte {
	b = append(b, '"')
	// Almost every string has nothing to escape: scan 8 bytes at a time and
	// copy it whole.
	i := 0
	for i+8 <= len(s) && !needsEscape(binary.LittleEndian.Uint64(s[i:])) {
		i += 8
	}
	start := 0
	for ; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		b = append(b, s[start:i]...)
		switch c {
		case '"':
			b = append(b, '\\', '"')
		case '\\':
			b = append(b, '\\', '\\')
		case '\n':
			b = append(b, '\\', 'n')
		case '\r':
			b = append(b, '\\', 'r')
		case '\t':
			b = append(b, '\\', 't')
		case '\b':
			b = append(b, '\\', 'b')
		case '\f':
			b = append(b, '\\', 'f')
		default:
			b = append(b, '\\', 'u', '0', '0',
				hexDigit(c>>4), hexDigit(c&0xF))
		}
		start = i + 1
	}
	b = append(b, s[start:]...)
	return append(b, '"')
}

func hexDigit(v byte) byte {
	if v < 10 {
		return '0' + v
	}
	return 'a' + v - 10
}

func appendInt(b []byte, v int64) []byte { return strconv.AppendInt(b, v, 10) }

func appendPost(b []byte, id int64, body, author []byte, createdAtMicros int64, likeCount int64) []byte {
	var m rowMark
	return appendPostMarked(b, id, body, author, createdAtMicros, likeCount, &m, 0)
}

// appendPostMarked also records where the row's fields landed, relative to
// base, for the gzip encoder.
func appendPostMarked(b []byte, id int64, body, author []byte, createdAtMicros, likeCount int64, m *rowMark, base int) []byte {
	m.start = int32(len(b) - base)
	b = append(b, `{"id":`...)
	b = strconv.AppendInt(b, id, 10)
	b = append(b, `,"body":`...)
	m.bodyStart = int32(len(b) + 1 - base)
	b = appendJSONString(b, body)
	m.bodyEnd = int32(len(b) - 1 - base)
	b = append(b, `,"created_at":"`...)
	b = appendTimestamp(b, createdAtMicros)
	b = append(b, `","author":`...)
	m.nameStart = int32(len(b) + 1 - base)
	b = appendJSONString(b, author)
	m.nameEnd = int32(len(b) - 1 - base)
	b = append(b, `,"like_count":`...)
	b = strconv.AppendInt(b, likeCount, 10)
	b = append(b, '}')
	m.end = int32(len(b) - base)
	return b
}

const digitPairs = "00010203040506070809101112131415161718192021222324" +
	"25262728293031323334353637383940414243444546474849" +
	"50515253545556575859606162636465666768697071727374" +
	"75767778798081828384858687888990919293949596979899"

// put2 writes the two digits of v (0-99).
func put2(b []byte, v int64) {
	b[0], b[1] = digitPairs[v*2], digitPairs[v*2+1]
}

// appendTimestamp renders micros since 2000-01-01 UTC as
// 2025-12-31T23:59:19.409Z, truncating to milliseconds.
func appendTimestamp(b []byte, micros int64) []byte {
	const unix2000 = 946684800 // seconds from 1970-01-01 to 2000-01-01
	q := micros / 1_000_000
	r := micros % 1_000_000
	if r < 0 { // floor division: pre-2000 timestamps stay correct
		q--
		r += 1_000_000
	}
	total := q + unix2000
	ms := r / 1000
	days := total / 86400
	secs := total % 86400
	y, m, d := civilFromDays(days)

	n := len(b)
	var t [24]byte
	b = append(b, t[:]...)
	o := b[n : n+24 : n+24]
	put2(o[0:], y/100)
	put2(o[2:], y%100)
	o[4] = '-'
	put2(o[5:], m)
	o[7] = '-'
	put2(o[8:], d)
	o[10] = 'T'
	put2(o[11:], secs/3600)
	o[13] = ':'
	put2(o[14:], secs%3600/60)
	o[16] = ':'
	put2(o[17:], secs%60)
	o[19] = '.'
	o[20] = byte('0' + ms/100)
	put2(o[21:], ms%100)
	o[23] = 'Z'
	return b
}

// civilFromDays converts days since 1970-01-01 to (year, month, day)
// (Howard Hinnant's algorithm).
func civilFromDays(z int64) (y, m, d int64) {
	z += 719468
	era := z / 146097
	if z%146097 < 0 {
		era--
	}
	doe := z - era*146097
	yoe := (doe - doe/1460 + doe/36524 - doe/146096) / 365
	yy := yoe + era*400
	doy := doe - (365*yoe + yoe/4 - yoe/100)
	mp := (5*doy + 2) / 153
	dd := doy - (153*mp+2)/5 + 1
	mm := mp + 3
	if mp >= 10 {
		mm -= 12
		yy++
	}
	return yy, mm, dd
}

// ---- health ----

var serverStart = time.Now()

func appendHealthOK(b []byte) []byte {
	b = append(b[:hdrRoom], `{"status":"ok","db":"ok","uptime_s":`...)
	b = strconv.AppendInt(b, int64(time.Since(serverStart).Seconds()), 10)
	return append(b, '}')
}

func appendHealthDegraded(b []byte, errText string) []byte {
	b = append(b[:hdrRoom], `{"status":"degraded","db":"unreachable","error":`...)
	b = appendJSONString(b, []byte(errText))
	return append(b, '}')
}
