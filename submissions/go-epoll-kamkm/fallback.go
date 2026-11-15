package main

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"net/http"
	"sync"
)

var requestReaders = sync.Pool{New: func() any { return bufio.NewReaderSize(nil, 4096) }}

var continueResponse = []byte("HTTP/1.1 100 Continue\r\n\r\n")

// parseStandard handles every request shape the fast parser declines (chunked
// bodies, Expect, HTTP/1.0, encoded or absolute targets, folded headers) with
// Go's HTTP parser.
func (l *loop) parseStandard(fd int32, c *conn, req *request, data []byte) (int, parseStatus) {
	head := bytes.Index(data, []byte("\r\n\r\n"))
	if head < 0 {
		if len(data) > maxHeaderBytes {
			return 0, parseBad
		}
		return 0, parseIncomplete
	}
	if head+4 > maxHeaderBytes {
		return 0, parseBad
	}
	input := bytes.NewReader(data)
	reader := requestReaders.Get().(*bufio.Reader)
	reader.Reset(input)
	defer func() { reader.Reset(nil); requestReaders.Put(reader) }()
	standard, err := http.ReadRequest(reader)
	if err != nil {
		return 0, parseBad
	}
	if standard.ContentLength > maxBodyBytes {
		return 0, parseBad
	}
	var body []byte
	if standard.Body != http.NoBody {
		body, err = io.ReadAll(io.LimitReader(standard.Body, maxBodyBytes+1))
	}
	if errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, io.EOF) {
		// The body is still on its way. A client that asked first is told to
		// send it, once.
		if c.flags&flagContinued == 0 && standard.Header.Get("Expect") == "100-continue" {
			_, _ = send(int(fd), continueResponse)
			c.flags |= flagContinued
		}
		return 0, parseIncomplete
	}
	if err != nil || len(body) > maxBodyBytes {
		return 0, parseBad
	}
	*req = request{close: standard.Close, body: body}
	resolveRoute(req, []byte(standard.Method), []byte(standard.URL.Path))
	if authorization, ok := standard.Header["Authorization"]; ok && len(authorization) > 0 {
		req.auth, req.hasAuth = authorization[0], true
	}
	req.gzip = acceptsGzip(standard.Header.Get("Accept-Encoding"))
	return len(data) - input.Len() - reader.Buffered(), parseOK
}
