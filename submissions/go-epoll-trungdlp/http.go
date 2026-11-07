package main

import (
	"bytes"
	"strconv"
)

// parseResult says what parseRequest found at the start of the buffer.
const (
	parseOK         = iota
	parseIncomplete // need more bytes
	parseBad        // answer 400 and close
	parseTooLarge   // answer 413 and close
)

const maxHeaderBytes = 16 << 10

var crlfcrlf = []byte("\r\n\r\n")

// parseRequest parses one HTTP/1.1 request from buf. On parseOK, n is the number of bytes
// it used; r's slices point into buf.
func parseRequest(buf []byte, r *request) (n int, result int) {
	end := bytes.Index(buf, crlfcrlf)
	if end < 0 {
		if len(buf) > maxHeaderBytes {
			return 0, parseTooLarge
		}
		return 0, parseIncomplete
	}
	head := buf[:end]
	line, rest, _ := bytes.Cut(head, []byte("\r\n"))
	sp1 := bytes.IndexByte(line, ' ')
	if sp1 <= 0 {
		return 0, parseBad
	}
	sp2 := bytes.LastIndexByte(line, ' ')
	if sp2 <= sp1 {
		return 0, parseBad
	}
	*r = request{method: line[:sp1]}
	target, proto := line[sp1+1:sp2], line[sp2+1:]
	if q := bytes.IndexByte(target, '?'); q >= 0 {
		target = target[:q]
	}
	r.path = target
	keepAlive := string(proto) == "HTTP/1.1"
	if !keepAlive && string(proto) != "HTTP/1.0" {
		return 0, parseBad
	}
	contentLength := 0
	for len(rest) > 0 {
		var h []byte
		h, rest, _ = bytes.Cut(rest, []byte("\r\n"))
		colon := bytes.IndexByte(h, ':')
		if colon <= 0 {
			return 0, parseBad
		}
		name, value := h[:colon], bytes.TrimSpace(h[colon+1:])
		switch len(name) {
		case len("authorization"):
			if bytes.EqualFold(name, []byte("authorization")) {
				r.auth = value
			}
		case len("content-length"):
			if bytes.EqualFold(name, []byte("content-length")) {
				v, err := strconv.Atoi(string(value))
				if err != nil || v < 0 {
					return 0, parseBad
				}
				contentLength = v
			}
		case len("accept-encoding"):
			if bytes.EqualFold(name, []byte("accept-encoding")) && bytes.Contains(value, []byte("gzip")) {
				r.gzip = true
			}
		case len("connection"):
			if bytes.EqualFold(name, []byte("connection")) {
				if bytes.EqualFold(value, []byte("close")) {
					keepAlive = false
				} else if bytes.EqualFold(value, []byte("keep-alive")) {
					keepAlive = true
				}
			}
		case len("transfer-encoding"):
			if bytes.EqualFold(name, []byte("transfer-encoding")) {
				return 0, parseBad // chunked request bodies are out of scope
			}
		}
	}
	if contentLength > maxRequestBody {
		return 0, parseTooLarge
	}
	total := end + len(crlfcrlf) + contentLength
	if len(buf) < total {
		return 0, parseIncomplete
	}
	r.body = buf[end+len(crlfcrlf) : total]
	r.close = !keepAlive
	return total, parseOK
}

var statusLines = map[int]string{
	200: "HTTP/1.1 200 OK\r\n",
	201: "HTTP/1.1 201 Created\r\n",
	400: "HTTP/1.1 400 Bad Request\r\n",
	401: "HTTP/1.1 401 Unauthorized\r\n",
	404: "HTTP/1.1 404 Not Found\r\n",
	413: "HTTP/1.1 413 Payload Too Large\r\n",
	500: "HTTP/1.1 500 Internal Server Error\r\n",
	503: "HTTP/1.1 503 Service Unavailable\r\n",
}

// appendHead writes the status line and headers for a body of n bytes.
func appendHead(b []byte, status, n int, gzipped, close bool) []byte {
	b = append(b, statusLines[status]...)
	b = append(b, "Content-Type: application/json\r\n"...)
	if gzipped {
		b = append(b, "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"...)
	}
	if close {
		b = append(b, "Connection: close\r\n"...)
	}
	b = append(b, "Content-Length: "...)
	b = strconv.AppendInt(b, int64(n), 10)
	return append(b, "\r\n\r\n"...)
}
