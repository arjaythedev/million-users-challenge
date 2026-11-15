package main

import (
	"encoding/binary"
	"strconv"
	"time"
	"unsafe"
)

const hexDigits = "0123456789abcdef"

// appendString appends s as a JSON string. Text from PostgreSQL and from the
// JSON decoder is valid UTF-8, so only quotes, backslashes and control
// characters need escapes.
func appendString[T string | []byte](dst []byte, s T) []byte {
	dst = append(dst, '"')
	start, i := 0, 0
	if len(s) >= 8 {
		// A string and a slice both begin with a data pointer, so the text can
		// be read eight bytes at a time without copying it.
		p := *(*unsafe.Pointer)(unsafe.Pointer(&s))
		for ; i+8 <= len(s); i += 8 {
			if needsEscape(binary.LittleEndian.Uint64(unsafe.Slice((*byte)(unsafe.Add(p, i)), 8))) {
				break
			}
		}
	}
	for ; i < len(s); i++ {
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

// needsEscape reports whether any of the eight bytes in w is a control
// character, a quote or a backslash. Bytes from 0x80 up never match.
func needsEscape(w uint64) bool {
	const ones, highs = 0x0101010101010101, 0x8080808080808080
	quote := w ^ ones*'"'
	slash := w ^ ones*'\\'
	return ((w-ones*0x20)|(quote-ones)|(slash-ones))&^w&highs != 0
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
	seconds, fraction := floorDiv(micros, 1_000_000)
	days, daySecond := floorDiv(seconds, 86400)
	year, month, day := civilDate(days)
	if year < 0 || year > 9999 {
		return time.UnixMicro(micros).UTC().AppendFormat(dst, "2006-01-02T15:04:05.000Z")
	}
	hour, minute, second := daySecond/3600, daySecond/60%60, daySecond%60
	millis := fraction / 1000
	return append(dst,
		byte('0'+year/1000), byte('0'+year/100%10), byte('0'+year/10%10), byte('0'+year%10), '-',
		byte('0'+month/10), byte('0'+month%10), '-',
		byte('0'+day/10), byte('0'+day%10), 'T',
		byte('0'+hour/10), byte('0'+hour%10), ':',
		byte('0'+minute/10), byte('0'+minute%10), ':',
		byte('0'+second/10), byte('0'+second%10), '.',
		byte('0'+millis/100), byte('0'+millis/10%10), byte('0'+millis%10), 'Z')
}

// floorDiv divides rounding toward negative infinity, so the remainder is
// never negative.
func floorDiv(a, b int64) (int64, int64) {
	q, r := a/b, a%b
	if r < 0 {
		q, r = q-1, r+b
	}
	return q, r
}

// civilDate converts days since 1970-01-01 to a proleptic Gregorian date in
// one pass of integer arithmetic. Years are counted from March, so the leap
// day falls at the end of a year; a 400-year era has exactly 146,097 days.
func civilDate(days int64) (year, month, day int64) {
	era, dayOfEra := floorDiv(days+719468, 146097) // 719,468 days from 0000-03-01 to 1970-01-01
	yearOfEra := (dayOfEra - dayOfEra/1460 + dayOfEra/36524 - dayOfEra/146096) / 365
	dayOfYear := dayOfEra - (365*yearOfEra + yearOfEra/4 - yearOfEra/100)
	shifted := (5*dayOfYear + 2) / 153 // month counted from March
	day = dayOfYear - (153*shifted+2)/5 + 1
	month = shifted + 3
	if month > 12 {
		month -= 12
	}
	year = era*400 + yearOfEra
	if month <= 2 {
		year++
	}
	return year, month, day
}
