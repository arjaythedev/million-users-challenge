package main

import (
	"strconv"
	"time"
	"unsafe"
)

const (
	createdAtLayout = "2006-01-02T15:04:05.000Z"
	// Postgres timestamps count microseconds from 2000-01-01 UTC.
	pgEpochUnixMicro = 946684800 * 1_000_000
	hexDigits        = "0123456789abcdef"
)

func appendJSONString(dst []byte, s string) []byte {
	return appendEscapedBytes(dst, unsafe.Slice(unsafe.StringData(s), len(s)))
}

// appendEscapedBytes writes s as the inside of a JSON string, copying clean spans in bulk.
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
		case '\b':
			b = append(b, '\\', 'b')
		case '\f':
			b = append(b, '\\', 'f')
		default:
			b = append(b, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xf])
		}
		start = i + 1
	}
	return append(b, s[start:]...)
}

var zeroLikes = []byte("0")

// appendWirePost writes one post object directly from wire columns:
// id and like_count as the ASCII digits Postgres sent, created_at as binary timestamptz (µs).
func appendWirePost(b []byte, id, body []byte, createdMicros int64, author, likes []byte) []byte {
	b = append(b, `{"id":`...)
	b = append(b, id...)
	b = append(b, `,"body":"`...)
	b = appendEscapedBytes(b, body)
	b = append(b, `","created_at":"`...)
	b = appendPgTime(b, createdMicros)
	b = append(b, `","author":"`...)
	b = appendEscapedBytes(b, author)
	b = append(b, `","like_count":`...)
	b = append(b, likes...)
	return append(b, '}')
}

// appendPgTime writes a Postgres timestamp (µs since 2000-01-01 UTC) as
// 2006-01-02T15:04:05.000Z, truncated to milliseconds, using integer math.
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

func formatHealthOK(dst []byte, uptimeS int64) []byte {
	dst = append(dst, `{"status":"ok","db":"ok","uptime_s":`...)
	dst = strconv.AppendInt(dst, uptimeS, 10)
	dst = append(dst, '}')
	return dst
}

func formatHealthError(dst []byte, errStr string) []byte {
	dst = append(dst, `{"status":"degraded","db":"unreachable","error":"`...)
	dst = appendJSONString(dst, errStr)
	dst = append(dst, `"}`...)
	return dst
}

func formatErrorJSON(dst []byte, msg string) []byte {
	dst = append(dst, `{"error":"`...)
	dst = appendJSONString(dst, msg)
	dst = append(dst, `"}`...)
	return dst
}

func errorBody(msg string) []byte {
	return formatErrorJSON(make([]byte, 0, len(msg)+16), msg)
}

var (
	errBodyNotFound     = errorBody("not found")
	errBodyPostNotFound = errorBody("post not found")
	errBodyInvalidID    = errorBody("invalid post id")
	errBodyBadRequest   = errorBody("malformed request")
	errBodyTooLarge     = errorBody("request too large")
	errBodyInternal     = errorBody("internal server error")
)

type Post struct {
	ID        int64
	Body      string
	CreatedAt time.Time
	Author    string
	LikeCount int64
}

func formatPostObj(dst []byte, p Post) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, p.ID, 10)
	dst = append(dst, `,"body":"`...)
	dst = appendJSONString(dst, p.Body)
	dst = append(dst, `","created_at":"`...)
	dst = p.CreatedAt.UTC().AppendFormat(dst, createdAtLayout)
	dst = append(dst, `","author":"`...)
	dst = appendJSONString(dst, p.Author)
	dst = append(dst, `","like_count":`...)
	dst = strconv.AppendInt(dst, p.LikeCount, 10)
	dst = append(dst, '}')
	return dst
}

func formatLikeJSON(dst []byte, alreadyLiked bool, postID int64) []byte {
	dst = append(dst, `{"liked":true,"already_liked":`...)
	if alreadyLiked {
		dst = append(dst, "true"...)
	} else {
		dst = append(dst, "false"...)
	}
	dst = append(dst, `,"post_id":`...)
	dst = strconv.AppendInt(dst, postID, 10)
	dst = append(dst, '}')
	return dst
}

func appendHTTPHead(dst []byte, status int, bodyLen int, isGzip bool, closeConn bool) []byte {
	switch status {
	case 200:
		dst = append(dst, "HTTP/1.1 200 OK\r\n"...)
	case 201:
		dst = append(dst, "HTTP/1.1 201 Created\r\n"...)
	case 400:
		dst = append(dst, "HTTP/1.1 400 Bad Request\r\n"...)
	case 401:
		dst = append(dst, "HTTP/1.1 401 Unauthorized\r\n"...)
	case 404:
		dst = append(dst, "HTTP/1.1 404 Not Found\r\n"...)
	case 413:
		dst = append(dst, "HTTP/1.1 413 Payload Too Large\r\n"...)
	case 500:
		dst = append(dst, "HTTP/1.1 500 Internal Server Error\r\n"...)
	case 503:
		dst = append(dst, "HTTP/1.1 503 Service Unavailable\r\n"...)
	default:
		dst = append(dst, "HTTP/1.1 "...)
		dst = strconv.AppendInt(dst, int64(status), 10)
		dst = append(dst, "\r\n"...)
	}
	dst = append(dst, "Content-Type: application/json\r\n"...)
	if isGzip {
		dst = append(dst, "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"...)
	}
	if closeConn {
		dst = append(dst, "Connection: close\r\n"...)
	}
	dst = append(dst, "Content-Length: "...)
	dst = strconv.AppendInt(dst, int64(bodyLen), 10)
	return append(dst, "\r\n\r\n"...)
}

func appendHTTPResponse(dst []byte, status int, body []byte, isGzip bool) []byte {
	dst = appendHTTPHead(dst, status, len(body), isGzip, false)
	return append(dst, body...)
}
