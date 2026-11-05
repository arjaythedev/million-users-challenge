package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"unsafe"
)

const (
	parseOK = iota
	parseIncomplete
	parseBad
	parseTooLarge
)

const (
	maxHeaderBytes = 16 << 10
	maxRequestBody = 16 << 10
)

var (
	methodGET  = []byte("GET")
	methodPOST = []byte("POST")
	hdrAuth    = []byte("Authorization")
	hdrCL      = []byte("Content-Length")
	hdrEnc     = []byte("Accept-Encoding")
	hdrConn    = []byte("Connection")
	hdrTE      = []byte("Transfer-Encoding")
	valClose   = []byte("close")
	valKeep    = []byte("keep-alive")
	valGzip    = []byte("gzip")
	crlfcrlf   = []byte("\r\n\r\n")
	lflf       = []byte("\n\n")
)

type ParsedRequest struct {
	Method        []byte
	Path          []byte
	AuthHeader    []byte
	Body          []byte
	ContentLength int
	HeaderLen     int
	GzipAccepted  bool
	KeepAlive     bool
	Complete      bool
}

func parseRequest(data []byte, req *ParsedRequest) (used int, res int) {
	end := bytes.Index(data, crlfcrlf)
	delimLen := 4
	if end < 0 {
		end = bytes.Index(data, lflf)
		delimLen = 2
		if end < 0 {
			if len(data) > maxHeaderBytes {
				return 0, parseTooLarge
			}
			return 0, parseIncomplete
		}
	}
	if end > maxHeaderBytes {
		return 0, parseTooLarge
	}

	head := data[:end]
	headerLen := end + delimLen
	var firstLine, rest []byte
	if delimLen == 4 {
		firstLine, rest, _ = bytes.Cut(head, []byte("\r\n"))
	} else {
		firstLine, rest, _ = bytes.Cut(head, []byte("\n"))
	}

	sp1 := bytes.IndexByte(firstLine, ' ')
	if sp1 <= 0 {
		return 0, parseBad
	}
	sp2 := bytes.LastIndexByte(firstLine, ' ')
	if sp2 <= sp1 {
		return 0, parseBad
	}

	m := firstLine[:sp1]
	target := firstLine[sp1+1 : sp2]
	proto := firstLine[sp2+1:]
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}

	keepAlive := string(proto) == "HTTP/1.1"
	if !keepAlive && string(proto) != "HTTP/1.0" {
		return 0, parseBad
	}

	*req = ParsedRequest{
		Method:    m,
		Path:      target,
		HeaderLen: headerLen,
		KeepAlive: keepAlive,
	}

	contentLength := 0
	for len(rest) > 0 {
		var line []byte
		if delimLen == 4 {
			line, rest, _ = bytes.Cut(rest, []byte("\r\n"))
		} else {
			line, rest, _ = bytes.Cut(rest, []byte("\n"))
		}
		if len(line) == 0 {
			continue
		}
		colon := bytes.IndexByte(line, ':')
		if colon <= 0 {
			return 0, parseBad
		}
		key := line[:colon]
		val := bytes.TrimSpace(line[colon+1:])

		switch len(key) {
		case len(hdrAuth):
			if bytes.EqualFold(key, hdrAuth) {
				req.AuthHeader = val
			}
		case len(hdrCL):
			if bytes.EqualFold(key, hdrCL) {
				cl, ok := parseContentLength(val)
				if !ok {
					return 0, parseBad
				}
				contentLength = cl
			}
		case len(hdrEnc):
			if bytes.EqualFold(key, hdrEnc) && bytes.Contains(val, valGzip) {
				req.GzipAccepted = true
			}
		case len(hdrConn):
			if bytes.EqualFold(key, hdrConn) {
				if bytes.EqualFold(val, valClose) {
					req.KeepAlive = false
				} else if bytes.EqualFold(val, valKeep) {
					req.KeepAlive = true
				}
			}
		case len(hdrTE):
			if bytes.EqualFold(key, hdrTE) {
				return 0, parseBad
			}
		}
	}

	if contentLength > maxRequestBody {
		return 0, parseTooLarge
	}
	req.ContentLength = contentLength
	totalRequired := headerLen + contentLength
	if len(data) < totalRequired {
		return 0, parseIncomplete
	}

	if contentLength > 0 {
		req.Body = data[headerLen:totalRequired]
	}
	req.Complete = true
	return totalRequired, parseOK
}

// parseContentLength parses ASCII digits without allocating (no strconv.Atoi).
// Returns ok=false on overflow or non-digit.
func parseContentLength(v []byte) (int, bool) {
	if len(v) == 0 {
		return 0, false
	}
	n := 0
	for _, c := range v {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int(c-'0')
		if n > 1<<20 {
			return 0, false
		}
	}
	return n, true
}

func parseIDBytes(s []byte) (int64, bool) {
	if len(s) == 0 {
		return 0, false
	}
	var id int64
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return 0, false
		}
		id = id*10 + int64(s[i]-'0')
		if id > 1<<62 {
			return 0, false
		}
	}
	return id, id > 0
}

func parseID(s string) (int64, bool) {
	return parseIDBytes(unsafe.Slice(unsafe.StringData(s), len(s)))
}

func utf16Len(s string) int {
	n := 0
	for _, r := range s {
		if r >= 0x10000 {
			n += 2
		} else {
			n++
		}
	}
	return n
}

// fast-path scanner for {"body":"..."} without escapes (the 99.9% case).
// Falls back to encoding/json with any to handle type checks and escapes properly.
func parsePostBody(bodyBytes []byte) (body string, errCode int, errMsg string) {
	if s, ok := fastParseBody(bodyBytes); ok {
		s = strings.TrimSpace(s)
		if s == "" {
			return "", 400, "body is required"
		}
		if utf16Len(s) > 500 {
			return "", 400, "body must be at most 500 characters"
		}
		return strings.Clone(s), 0, ""
	}

	var value any
	if len(bodyBytes) != 0 && json.Unmarshal(bodyBytes, &value) != nil {
		return "", 400, "malformed JSON body"
	}
	obj, _ := value.(map[string]any)
	str, _ := obj["body"].(string)
	s := strings.TrimSpace(str)
	if s == "" {
		return "", 400, "body is required"
	}
	if utf16Len(s) > 500 {
		return "", 400, "body must be at most 500 characters"
	}
	return strings.Clone(s), 0, ""
}

func fastParseBody(b []byte) (string, bool) {
	b = bytes.TrimSpace(b)
	if len(b) < 11 || b[0] != '{' {
		return "", false
	}
	b = b[1:]
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t' || b[0] == '\n' || b[0] == '\r') {
		b = b[1:]
	}
	if len(b) < 6 || string(b[:6]) != `"body"` {
		return "", false
	}
	b = b[6:]
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t' || b[0] == '\n' || b[0] == '\r') {
		b = b[1:]
	}
	if len(b) < 1 || b[0] != ':' {
		return "", false
	}
	b = b[1:]
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t' || b[0] == '\n' || b[0] == '\r') {
		b = b[1:]
	}
	if len(b) < 2 || b[0] != '"' {
		return "", false
	}
	b = b[1:]
	for i := 0; i < len(b); i++ {
		if b[i] == '\\' {
			return "", false // has escapes, fall back to encoding/json
		}
		if b[i] == '"' {
			tail := bytes.TrimSpace(b[i+1:])
			if len(tail) == 1 && tail[0] == '}' {
				return unsafe.String(unsafe.SliceData(b[:i]), i), true
			}
			return "", false
		}
	}
	return "", false
}
