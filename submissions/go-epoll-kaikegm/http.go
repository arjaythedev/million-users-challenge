package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"strings"
)

var (
	methodGET  = []byte("GET")
	methodPOST = []byte("POST")
	hdrAuth    = []byte("Authorization")
	hdrCL      = []byte("Content-Length")
	hdrEnc     = []byte("Accept-Encoding")
	hdrConn    = []byte("Connection")
	valClose   = []byte("close")
	valGzip    = []byte("gzip")
)

type ParsedRequest struct {
	Method        string
	Path          string
	AuthHeader    []byte
	Body          []byte
	ContentLength int
	HeaderLen     int
	GzipAccepted  bool
	KeepAlive     bool
	Complete      bool
}

func parseHTTPRequest(data []byte) (req ParsedRequest, err error) {
	idx := bytes.Index(data, []byte("\r\n\r\n"))
	headerEnd := 0
	delimLen := 4
	if idx >= 0 {
		headerEnd = idx
	} else {
		idx = bytes.Index(data, []byte("\n\n"))
		if idx >= 0 {
			headerEnd = idx
			delimLen = 2
		} else {
			return req, nil
		}
	}

	headers := data[:headerEnd]
	req.HeaderLen = headerEnd + delimLen
	req.KeepAlive = true

	lineEnd := bytes.IndexByte(headers, '\n')
	var firstLine []byte
	if lineEnd >= 0 {
		firstLine = bytes.TrimRight(headers[:lineEnd], "\r")
		headers = headers[lineEnd+1:]
	} else {
		firstLine = bytes.TrimRight(headers, "\r")
		headers = nil
	}

	sp1 := bytes.IndexByte(firstLine, ' ')
	if sp1 < 0 {
		return req, errors.New("bad request line")
	}
	// Constant assignment, no string() allocation
	m := firstLine[:sp1]
	switch {
	case bytes.Equal(m, methodGET):
		req.Method = "GET"
	case bytes.Equal(m, methodPOST):
		req.Method = "POST"
	default:
		return req, errors.New("unsupported method")
	}

	rest := firstLine[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 >= 0 {
		req.Path = string(rest[:sp2])
	} else {
		req.Path = string(rest)
	}

	for len(headers) > 0 {
		var line []byte
		nl := bytes.IndexByte(headers, '\n')
		if nl >= 0 {
			line = bytes.TrimRight(headers[:nl], "\r")
			headers = headers[nl+1:]
		} else {
			line = bytes.TrimRight(headers, "\r")
			headers = nil
		}
		if len(line) == 0 {
			continue
		}
		colon := bytes.IndexByte(line, ':')
		if colon < 0 {
			continue
		}
		key := line[:colon]
		val := bytes.TrimSpace(line[colon+1:])

		// bytes.EqualFold, no string(key) conversion
		switch {
		case len(key) == len(hdrAuth) && bytes.EqualFold(key, hdrAuth):
			req.AuthHeader = val
		case len(key) == len(hdrCL) && bytes.EqualFold(key, hdrCL):
			if cl, ok := parseContentLength(val); ok {
				req.ContentLength = cl
			}
		case len(key) == len(hdrEnc) && bytes.EqualFold(key, hdrEnc):
			if bytes.Contains(val, valGzip) {
				req.GzipAccepted = true
			}
		case len(key) == len(hdrConn) && bytes.EqualFold(key, hdrConn):
			if bytes.EqualFold(val, valClose) {
				req.KeepAlive = false
			}
		}
	}

	totalRequired := req.HeaderLen + req.ContentLength
	if len(data) < totalRequired {
		return req, nil
	}

	if req.ContentLength > 0 {
		req.Body = data[req.HeaderLen : req.HeaderLen+req.ContentLength]
	}
	req.Complete = true
	return req, nil
}

// parseContentLength parses ASCII digits without allocating (no strconv.Atoi).
// Returns ok=false on overflow or non-digit.
func parseContentLength(v []byte) (int, bool) {
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

func parseID(s string) (int64, bool) {
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
// Falls back to encoding/json with Body any to handle type checks and escapes properly.
func parsePostBody(bodyBytes []byte) (body string, errCode int, errMsg string) {
	if s, ok := fastParseBody(bodyBytes); ok {
		s = strings.TrimSpace(s)
		if s == "" {
			return "", 400, "body is required"
		}
		if utf16Len(s) > 500 {
			return "", 400, "body must be at most 500 characters"
		}
		return s, 0, ""
	}

	var req struct {
		Body any `json:"body"`
	}
	if err := json.Unmarshal(bodyBytes, &req); err != nil {
		return "", 400, "malformed JSON body"
	}
	str, ok := req.Body.(string)
	if !ok {
		return "", 400, "body is required"
	}
	s := strings.TrimSpace(str)
	if s == "" {
		return "", 400, "body is required"
	}
	if utf16Len(s) > 500 {
		return "", 400, "body must be at most 500 characters"
	}
	return s, 0, ""
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
				return string(b[:i]), true
			}
			return "", false
		}
	}
	return "", false
}
