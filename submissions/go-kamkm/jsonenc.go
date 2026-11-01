package main

import (
	"strconv"
	"time"
)

const hexDigits = "0123456789abcdef"

// appendString appends s as a JSON string. Text from PostgreSQL and from the
// JSON decoder is valid UTF-8, so only quotes, backslashes and control
// characters need escapes.
func appendString[T string | []byte](dst []byte, s T) []byte {
	dst = append(dst, '"')
	start := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c >= 0x20 && c != '"' && c != '\\' {
			continue
		}
		dst = append(dst, s[start:i]...)
		switch c {
		case '"', '\\':
			dst = append(dst, '\\', c)
		case '\n':
			dst = append(dst, '\\', 'n')
		case '\r':
			dst = append(dst, '\\', 'r')
		case '\t':
			dst = append(dst, '\\', 't')
		default:
			dst = append(dst, '\\', 'u', '0', '0', hexDigits[c>>4], hexDigits[c&0xf])
		}
		start = i + 1
	}
	dst = append(dst, s[start:]...)
	return append(dst, '"')
}

// appendPost appends a post object with the API's key order.
func appendPost[B string | []byte, A string | []byte](dst []byte, id int64, body B, createdMicros int64, author A, likes int64) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, id, 10)
	dst = append(dst, `,"body":`...)
	dst = appendString(dst, body)
	dst = append(dst, `,"created_at":"`...)
	dst = appendCreated(dst, createdMicros)
	dst = append(dst, `","author":`...)
	dst = appendString(dst, author)
	dst = append(dst, `,"like_count":`...)
	dst = strconv.AppendInt(dst, likes, 10)
	return append(dst, '}')
}

// appendCreated formats Unix microseconds as UTC with millisecond precision,
// truncating rather than rounding: 2006-01-02T15:04:05.000Z.
func appendCreated(dst []byte, micros int64) []byte {
	t := time.UnixMicro(micros).UTC()
	year, month, day := t.Date()
	hour, minute, second := t.Clock()
	millis := t.Nanosecond() / int(time.Millisecond)
	if year < 0 || year > 9999 {
		return t.AppendFormat(dst, "2006-01-02T15:04:05.000Z")
	}
	dst = append(dst,
		byte('0'+year/1000), byte('0'+year/100%10), byte('0'+year/10%10), byte('0'+year%10), '-',
		byte('0'+int(month)/10), byte('0'+int(month)%10), '-',
		byte('0'+day/10), byte('0'+day%10), 'T',
		byte('0'+hour/10), byte('0'+hour%10), ':',
		byte('0'+minute/10), byte('0'+minute%10), ':',
		byte('0'+second/10), byte('0'+second%10), '.',
		byte('0'+millis/100), byte('0'+millis/10%10), byte('0'+millis%10), 'Z')
	return dst
}
