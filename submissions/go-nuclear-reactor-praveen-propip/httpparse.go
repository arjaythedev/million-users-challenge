package main

// Minimal HTTP/1.1 request parsing for the load generator's dialect plus
// curl (the test suite). One request per call; leftover bytes are re-parsed
// on the next event, so pipelining just works.

import (
	"bytes"
	"time"
)

// parseRequest parses one request from buf. Returns the request, how many
// bytes were consumed, and fatal=true when the stream is unrecoverable
// (garbage or oversized) and the connection must close after answering.
// consumed==0 means "incomplete: need more bytes".
func parseRequest(buf []byte, sc *parseScratch) (p parsed, consumed int, fatal bool) {
	p.kind = rImmediate

	line, rest, ok := readLine(buf)
	if !ok {
		return p, 0, false
	}
	method, target, http10, ok := splitRequestLine(line)
	if !ok {
		return badRequest(&p, len(buf))
	}
	p.closeAfter = http10

	// Headers.
	contentLen := -1
	var auth []byte
	for {
		line, rest, ok = readLine(rest)
		if !ok {
			return p, 0, false
		}
		if len(line) == 0 {
			break
		}
		col := bytes.IndexByte(line, ':')
		if col < 0 {
			continue
		}
		key, val := line[:col], line[col+1:]
		// The header name's length picks the only candidate, so the headers
		// we ignore cost one switch.
		switch len(key) {
		case 13:
			if asciiFoldEqual(key, "Authorization") {
				auth = trimSpaces(val)
			}
		case 14:
			if asciiFoldEqual(key, "Content-Length") {
				n, ok := parseContentLength(trimSpaces(val))
				if !ok {
					return badRequest(&p, len(buf))
				}
				contentLen = n
			}
		case 10:
			if asciiFoldEqual(key, "Connection") {
				val = trimSpaces(val)
				if asciiFoldEqual(val, "close") {
					p.closeAfter = true
				} else if asciiFoldEqual(val, "keep-alive") {
					p.closeAfter = false
				}
			}
		case 15:
			if asciiFoldEqual(key, "Accept-Encoding") && bytes.Contains(val, gzipToken) {
				p.wantsGzip = true
			}
		case 17:
			// The benchmark never sends chunked requests; refuse safely.
			if asciiFoldEqual(key, "Transfer-Encoding") && bytes.Contains(val, chunkedToken) {
				p.status, p.reason = 400, "malformed JSON body"
				p.closeAfter = true
				return p, len(buf), true
			}
		}
	}

	// Body framing: everything (headers + body) must be present.
	total := len(buf) - len(rest)
	if contentLen > 0 {
		if contentLen > 16384 {
			p.status, p.reason = 400, "malformed JSON body"
			p.closeAfter = true
			return p, len(buf), true
		}
		if len(rest) < contentLen {
			return p, 0, false
		}
		total += contentLen
	}

	// Route.
	if bytesEqual(method, "GET") {
		switch {
		case bytesEqual(target, "/health"):
			p.kind = rHealth
		case bytesEqual(target, "/feed"):
			p.kind = rFeed
		default:
			mid, ok := cutPath(target, "/posts/", "")
			if !ok {
				p.status, p.reason = 404, "not found"
				break
			}
			id, ok := allDigitsPositive(mid)
			if !ok {
				p.status, p.reason = 400, "invalid post id"
			} else {
				p.kind, p.id = rGetPost, id
			}
		}
	} else if bytesEqual(method, "POST") {
		var body []byte
		if contentLen > 0 {
			body = rest[:contentLen]
			rest = rest[contentLen:]
		}
		now := sc.now
		if now == 0 {
			now = time.Now().Unix()
		}
		uid, name, stage := sc.jwt.verify(auth, now)
		switch {
		case bytesEqual(target, "/posts"):
			if stage != jwtOK {
				setAuthError(&p, stage)
				break
			}
			switch parsePostBody(body, &sc.body) {
			case bodyOK:
				p.kind = rCreate
				p.uid, p.username = uid, name
				p.body = sc.body.bytes[:sc.body.n]
			case bodyTooLong:
				p.status, p.reason = 400, "body must be at most 500 characters"
			case bodyRequired:
				p.status, p.reason = 400, "body is required"
			default:
				p.status, p.reason = 400, "malformed JSON body"
			}
		case isLikePath(target):
			if stage != jwtOK { // auth is checked before the id (SPEC)
				setAuthError(&p, stage)
				break
			}
			mid, ok := cutPath(target, "/posts/", "/like")
			id, valid := allDigitsPositive(mid)
			if !ok || !valid {
				p.status, p.reason = 400, "invalid post id"
			} else {
				p.kind, p.id = rLike, id
				p.uid, p.username = uid, name
			}
		default:
			p.status, p.reason = 404, "not found"
		}
	} else {
		p.status, p.reason = 404, "not found"
	}
	return p, total, false
}

func setAuthError(p *parsed, stage int) {
	p.status = 401
	switch stage {
	case jwtNoBearer:
		p.reason = "missing bearer token"
	case jwtBadPayload:
		p.reason = "invalid token payload"
	default:
		p.reason = "invalid or expired token"
	}
}

func badRequest(p *parsed, n int) (parsed, int, bool) {
	p.status, p.reason, p.closeAfter = 400, "not found", true
	return *p, n, true
}

// cutPath matches target = prefix + middle + suffix and returns middle.
func cutPath(target []byte, prefix, suffix string) ([]byte, bool) {
	if len(target) <= len(prefix) || !bytesEqual(target[:len(prefix)], prefix) {
		return nil, false
	}
	rest := target[len(prefix):]
	if suffix != "" {
		if len(rest) <= len(suffix) || !bytesEqual(rest[len(rest)-len(suffix):], suffix) {
			return nil, false
		}
		rest = rest[:len(rest)-len(suffix)]
	}
	return rest, true
}

func isLikePath(target []byte) bool {
	_, ok := cutPath(target, "/posts/", "/like")
	return ok
}

// allDigitsPositive accepts 1+ ASCII digits with value > 0 (SPEC: "0" and
// non-digits are invalid post ids).
func allDigitsPositive(s []byte) (int64, bool) {
	if len(s) == 0 || len(s) > 19 {
		return 0, false
	}
	var v int64
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < '0' || c > '9' {
			return 0, false
		}
		v = v*10 + int64(c-'0')
	}
	return v, v > 0
}

// ---- byte helpers (allocation-free) ----

// readLine returns the next line without its terminator ("\\r\\n" or "\\n").
func readLine(b []byte) (line, rest []byte, ok bool) {
	i := bytes.IndexByte(b, '\n')
	if i < 0 {
		return nil, nil, false
	}
	end := i
	if end > 0 && b[end-1] == '\r' {
		end--
	}
	return b[:end], b[i+1:], true
}

var (
	gzipToken    = []byte("gzip")
	chunkedToken = []byte("chunked")
)

// parseContentLength accepts plain digits; anything over 7 digits is far past
// the 16 KB body limit.
func parseContentLength(b []byte) (int, bool) {
	if len(b) == 0 || len(b) > 7 {
		return 0, false
	}
	n := 0
	for _, c := range b {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int(c-'0')
	}
	return n, true
}

func splitRequestLine(line []byte) (method, target []byte, http10, ok bool) {
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return nil, nil, false, false
	}
	rest := line[sp1+1:]
	sp2 := bytes.IndexByte(rest, ' ')
	if sp2 <= 0 || bytes.IndexByte(rest[sp2+1:], ' ') >= 0 {
		return nil, nil, false, false
	}
	ver := rest[sp2+1:]
	if len(ver) < 6 || !bytesEqual(ver[:6], "HTTP/1") {
		return nil, nil, false, false
	}
	return line[:sp1], rest[:sp2], bytesEqual(ver, "HTTP/1.0"), true
}

func bytesEqual(a []byte, s string) bool {
	if len(a) != len(s) {
		return false
	}
	for i := 0; i < len(a); i++ {
		if a[i] != s[i] {
			return false
		}
	}
	return true
}

// asciiFoldEqual compares a header key with a canonical name, ASCII-only.
func asciiFoldEqual(a []byte, b string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := 0; i < len(a); i++ {
		if a[i]|0x20 != b[i]|0x20 {
			return false
		}
	}
	return true
}
