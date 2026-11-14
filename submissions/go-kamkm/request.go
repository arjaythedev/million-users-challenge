package main

import (
	"bytes"
	"strconv"
	"strings"
)

type routeKind uint8

const (
	routeNotFound routeKind = iota
	routeHealth
	routeFeed
	routeGetPost
	routeCreate
	routeLike
)

// request is what a handler needs from an HTTP request. The event loop copies
// only these fields out of its input buffer.
type request struct {
	route   routeKind
	id      int64
	idOK    bool
	gzip    bool
	close   bool
	hasAuth bool
	auth    string
	body    []byte
}

type parseStatus uint8

const (
	parseOK parseStatus = iota
	parseIncomplete
	parseBad
	parseFallback
)

// parseRequest parses the plain HTTP/1.1 requests that clients normally send:
// an origin-form target without query or escapes, CRLF lines, and an optional
// Content-Length body. Anything else (chunked bodies, Expect, HTTP/1.0,
// folded headers, duplicate lengths) returns parseFallback so Go's HTTP parser
// decides it.
func parseRequest(data []byte) (*request, int, parseStatus) {
	head := bytes.Index(data, []byte("\r\n\r\n"))
	if head < 0 {
		if len(data) > maxHeaderBytes {
			return nil, 0, parseBad
		}
		return nil, 0, parseIncomplete
	}
	if head+4 > maxHeaderBytes {
		return nil, 0, parseBad
	}
	lines := data[:head+2]
	end := bytes.Index(lines, []byte("\r\n"))
	line := lines[:end]
	lines = lines[end+2:]

	space := bytes.IndexByte(line, ' ')
	if space <= 0 {
		return nil, 0, parseFallback
	}
	method := line[:space]
	rest := line[space+1:]
	space = bytes.IndexByte(rest, ' ')
	if space <= 0 || string(rest[space+1:]) != "HTTP/1.1" {
		return nil, 0, parseFallback
	}
	target := rest[:space]
	if target[0] != '/' || bytes.IndexAny(target, "?#% \t\r\n") >= 0 {
		return nil, 0, parseFallback
	}
	for _, c := range target {
		if c <= ' ' || c >= 0x7f {
			return nil, 0, parseFallback
		}
	}
	for i := 0; i < len(method); i++ {
		if !isTokenByte(method[i]) {
			return nil, 0, parseFallback
		}
	}

	req := &request{}
	resolveRoute(req, method, target)
	needAuth := req.route == routeCreate || req.route == routeLike
	contentLength := -1
	seenEncoding := false
	seenHost := false
	for len(lines) > 0 {
		end = bytes.Index(lines, []byte("\r\n"))
		line = lines[:end]
		lines = lines[end+2:]
		colon := bytes.IndexByte(line, ':')
		if colon <= 0 {
			return nil, 0, parseFallback
		}
		name := line[:colon]
		for i := 0; i < len(name); i++ {
			if !isTokenByte(name[i]) {
				return nil, 0, parseFallback
			}
		}
		value := trimSpace(line[colon+1:])
		for _, c := range value {
			if c < 0x20 && c != '\t' || c == 0x7f {
				return nil, 0, parseFallback
			}
		}
		switch len(name) {
		case len("host"):
			if strings.EqualFold(string(name), "host") {
				if seenHost {
					return nil, 0, parseFallback
				}
				seenHost = true
			}
		case len("content-length"):
			if !strings.EqualFold(string(name), "content-length") {
				continue
			}
			if contentLength >= 0 || len(value) == 0 || len(value) > 6 {
				return nil, 0, parseFallback
			}
			n := 0
			for _, digit := range value {
				if digit < '0' || digit > '9' {
					return nil, 0, parseFallback
				}
				n = n*10 + int(digit-'0')
			}
			if n > maxBodyBytes {
				return nil, 0, parseBad
			}
			contentLength = n
		case len("transfer-encoding"):
			if strings.EqualFold(string(name), "transfer-encoding") {
				return nil, 0, parseFallback
			}
		case len("expect"):
			if strings.EqualFold(string(name), "expect") {
				return nil, 0, parseFallback
			}
		case len("connection"):
			if strings.EqualFold(string(name), "connection") && hasToken(value, "close") {
				req.close = true
			}
		case len("authorization"):
			if !req.hasAuth && strings.EqualFold(string(name), "authorization") {
				req.hasAuth = true
				if needAuth {
					req.auth = string(value)
				}
			}
		case len("accept-encoding"):
			if strings.EqualFold(string(name), "accept-encoding") {
				// Header.Get semantics: the first Accept-Encoding line decides.
				if !seenEncoding {
					seenEncoding = true
					req.gzip = string(value) == "gzip" || acceptsGzip(string(value))
				}
			}
		}
	}
	used := head + 4
	if contentLength > 0 {
		if len(data)-used < contentLength {
			return nil, 0, parseIncomplete
		}
		req.body = append([]byte(nil), data[used:used+contentLength]...)
		used += contentLength
	}
	return req, used, parseOK
}

func resolveRoute(req *request, method, path []byte) {
	get := string(method) == "GET"
	post := string(method) == "POST"
	switch {
	case get && string(path) == "/health":
		req.route = routeHealth
	case get && string(path) == "/feed":
		req.route = routeFeed
	case post && string(path) == "/posts":
		req.route = routeCreate
	case post && bytes.HasPrefix(path, []byte("/posts/")) && len(path) >= len("/posts/")+len("/like") && bytes.HasSuffix(path, []byte("/like")):
		// A route parameter is exactly one non-empty path segment. Unknown
		// paths remain 404; authentication-before-ID applies once this
		// endpoint shape has matched.
		raw := path[len("/posts/") : len(path)-len("/like")]
		if len(raw) == 0 || bytes.IndexByte(raw, '/') >= 0 {
			req.route = routeNotFound
			return
		}
		req.route = routeLike
		req.id, req.idOK = positiveID(raw)
	case get && bytes.HasPrefix(path, []byte("/posts/")):
		raw := path[len("/posts/"):]
		if len(raw) == 0 || bytes.IndexByte(raw, '/') >= 0 {
			req.route = routeNotFound
			return
		}
		req.route = routeGetPost
		req.id, req.idOK = positiveID(raw)
	default:
		req.route = routeNotFound
	}
}

func positiveID(value []byte) (int64, bool) {
	if len(value) == 0 {
		return 0, false
	}
	for _, c := range value {
		if c < '0' || c > '9' {
			return 0, false
		}
	}
	id, err := strconv.ParseInt(string(value), 10, 64)
	return id, err == nil && id > 0
}

func trimSpace(b []byte) []byte {
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t') {
		b = b[1:]
	}
	for len(b) > 0 && (b[len(b)-1] == ' ' || b[len(b)-1] == '\t') {
		b = b[:len(b)-1]
	}
	return b
}

// hasToken reports whether a comma-separated header value contains token,
// compared case-insensitively.
func hasToken(value []byte, token string) bool {
	for len(value) > 0 {
		item := value
		if comma := bytes.IndexByte(value, ','); comma >= 0 {
			item, value = value[:comma], value[comma+1:]
		} else {
			value = nil
		}
		if strings.EqualFold(string(trimSpace(item)), token) {
			return true
		}
	}
	return false
}

func isTokenByte(c byte) bool {
	switch {
	case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
		return true
	}
	switch c {
	case '!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~':
		return true
	}
	return false
}

func acceptsGzip(value string) bool {
	for _, item := range strings.Split(value, ",") {
		parts := strings.Split(item, ";")
		if !strings.EqualFold(strings.TrimSpace(parts[0]), "gzip") {
			continue
		}
		for _, parameter := range parts[1:] {
			key, raw, found := strings.Cut(strings.TrimSpace(parameter), "=")
			if found && strings.EqualFold(strings.TrimSpace(key), "q") {
				quality, err := strconv.ParseFloat(strings.TrimSpace(raw), 64)
				if err == nil && quality <= 0 {
					return false
				}
			}
		}
		return true
	}
	return false
}
