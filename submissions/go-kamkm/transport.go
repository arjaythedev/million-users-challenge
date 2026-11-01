package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"net/http"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/panjf2000/gnet/v2"
)

const (
	idleTimeout    = 180 * time.Second
	readTimeout    = 15 * time.Second
	maxHeaderBytes = 16 << 10
	maxBufferBytes = 36 << 10
)

// Event loops own the socket state. Only active requests start a goroutine.
type eventServer struct {
	gnet.BuiltinEventEngine
	app    *application
	ready  chan gnet.Engine
	active sync.WaitGroup
}

// An idle connection holds this record and one runtime timer. The timer is not
// reset per request: requests only move the deadline, and an expiring timer
// re-arms itself for the remaining time. Only a partially received request
// pulls the timer forward to the 15-second read deadline.
type eventConnection struct {
	busy      bool
	reading   bool
	continued bool
	deadline  atomic.Int64
	timer     *time.Timer
}

var clockBase = time.Now()

// monotonic returns nanoseconds on the monotonic clock.
func monotonic() int64 { return int64(time.Since(clockBase)) }

func (s *eventServer) OnBoot(e gnet.Engine) gnet.Action { s.ready <- e; return gnet.None }
func (s *eventServer) OnOpen(c gnet.Conn) ([]byte, gnet.Action) {
	state := &eventConnection{}
	state.deadline.Store(monotonic() + int64(idleTimeout))
	state.timer = time.AfterFunc(idleTimeout, func() { state.expire(c) })
	c.SetContext(state)
	return nil, gnet.None
}
func (s *eventServer) OnClose(c gnet.Conn, _ error) gnet.Action {
	if state, ok := c.Context().(*eventConnection); ok {
		state.timer.Stop()
	}
	return gnet.None
}
func (s *eventServer) OnTraffic(c gnet.Conn) gnet.Action { return s.dispatch(c) }

func (state *eventConnection) expire(c gnet.Conn) {
	for {
		deadline := state.deadline.Load()
		remaining := deadline - monotonic()
		if remaining <= 0 {
			_ = c.Close()
			return
		}
		state.timer.Reset(time.Duration(remaining))
		// The event loop may have moved the deadline while this timer was being
		// re-armed; re-arm again so the earlier deadline is not lost.
		if state.deadline.Load() == deadline {
			return
		}
	}
}

func (state *eventConnection) startReading() {
	if state.reading {
		return
	}
	state.reading = true
	state.deadline.Store(monotonic() + int64(readTimeout))
	state.timer.Reset(readTimeout)
}

var requestReaders = sync.Pool{New: func() any { return bufio.NewReaderSize(nil, 4096) }}

var malformedResponse = []byte("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n{\"error\":\"malformed JSON body\"}")

func (s *eventServer) dispatch(c gnet.Conn) gnet.Action {
	state, ok := c.Context().(*eventConnection)
	if !ok || state.busy || c.InboundBuffered() == 0 {
		return gnet.None
	}
	data, _ := c.Peek(-1)
	req, used, status := parseRequest(data)
	if status == parseFallback {
		req, used, status = s.parseStandard(c, state, data)
	}
	switch status {
	case parseBad:
		_, _ = c.Write(malformedResponse)
		return gnet.Close
	case parseIncomplete:
		if len(data) > maxBufferBytes {
			_, _ = c.Write(malformedResponse)
			return gnet.Close
		}
		state.startReading()
		return gnet.None
	}
	_, _ = c.Discard(used)
	state.busy = true
	state.reading = false
	state.continued = false
	state.deadline.Store(monotonic() + int64(idleTimeout))
	s.active.Add(1)
	go s.serve(c, req)
	return gnet.None
}

func (s *eventServer) serve(c gnet.Conn, req *request) {
	defer s.active.Done()
	wire := s.app.respond(getBuffer(), req)
	closeAfter := req.close
	_ = c.AsyncWrite(*wire, func(c gnet.Conn, err error) error {
		putBuffer(wire)
		current, ok := c.Context().(*eventConnection)
		if !ok || err != nil {
			return nil
		}
		if closeAfter {
			_ = c.Close()
			return nil
		}
		current.busy = false
		current.deadline.Store(monotonic() + int64(idleTimeout))
		if s.dispatch(c) == gnet.Close {
			_ = c.Close()
		}
		return nil
	})
}

// parseStandard handles every request shape the fast parser declines (chunked
// bodies, Expect, HTTP/1.0, encoded or absolute targets, folded headers) with
// Go's HTTP parser.
func (s *eventServer) parseStandard(c gnet.Conn, state *eventConnection, data []byte) (*request, int, parseStatus) {
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
	input := bytes.NewReader(data)
	reader := requestReaders.Get().(*bufio.Reader)
	reader.Reset(input)
	defer func() { reader.Reset(nil); requestReaders.Put(reader) }()
	standard, err := http.ReadRequest(reader)
	if err != nil {
		return nil, 0, parseBad
	}
	if standard.ContentLength > maxBodyBytes {
		return nil, 0, parseBad
	}
	var body []byte
	if standard.Body != http.NoBody {
		body, err = io.ReadAll(io.LimitReader(standard.Body, maxBodyBytes+1))
	}
	if errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, io.EOF) {
		if !state.continued && standard.Header.Get("Expect") == "100-continue" {
			_, _ = c.Write([]byte("HTTP/1.1 100 Continue\r\n\r\n"))
			state.continued = true
		}
		return nil, 0, parseIncomplete
	}
	if err != nil || len(body) > maxBodyBytes {
		return nil, 0, parseBad
	}
	req := &request{close: standard.Close, body: body}
	resolveRoute(req, []byte(standard.Method), []byte(standard.URL.Path))
	if authorization, ok := standard.Header["Authorization"]; ok && len(authorization) > 0 {
		req.auth, req.hasAuth = authorization[0], true
	}
	req.gzip = acceptsGzip(standard.Header.Get("Accept-Encoding"))
	return req, len(data) - input.Len() - reader.Buffered(), parseOK
}

func serveEventHTTP(ctx context.Context, app *application, host string, ports []string, loops int) error {
	server := &eventServer{app: app, ready: make(chan gnet.Engine, 1)}
	addresses := make([]string, len(ports))
	for i, port := range ports {
		addresses[i] = "tcp://" + net.JoinHostPort(host, port)
	}
	done := make(chan error, 1)
	go func() {
		done <- gnet.Rotate(server, addresses, gnet.WithNumEventLoop(loops), gnet.WithReuseAddr(true), gnet.WithReusePort(true), gnet.WithTCPNoDelay(gnet.TCPNoDelay), gnet.WithTCPKeepAlive(0))
	}()
	var engine gnet.Engine
	select {
	case engine = <-server.ready:
	case err := <-done:
		return err
	case <-ctx.Done():
		return ctx.Err()
	}
	select {
	case <-ctx.Done():
	case err := <-done:
		return err
	}
	shutdown, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	_ = engine.Stop(shutdown)
	err := <-done
	server.active.Wait()
	return err
}

// Response buffers are pooled: a buffer returns to the pool once the event
// loop has written or queued its bytes.
var buffers = sync.Pool{New: func() any { b := make([]byte, 0, 4096); return &b }}

func getBuffer() *[]byte {
	b := buffers.Get().(*[]byte)
	*b = (*b)[:0]
	return b
}

func putBuffer(b *[]byte) {
	if b == nil || cap(*b) > 64<<10 {
		return
	}
	buffers.Put(b)
}

func statusLine(status int) string {
	switch status {
	case 200:
		return "HTTP/1.1 200 OK\r\n"
	case 201:
		return "HTTP/1.1 201 Created\r\n"
	case 400:
		return "HTTP/1.1 400 Bad Request\r\n"
	case 401:
		return "HTTP/1.1 401 Unauthorized\r\n"
	case 404:
		return "HTTP/1.1 404 Not Found\r\n"
	case 500:
		return "HTTP/1.1 500 Internal Server Error\r\n"
	case 503:
		return "HTTP/1.1 503 Service Unavailable\r\n"
	}
	return ""
}

func appendResponse(wire []byte, status int, body []byte, gzipped, closeAfter bool) []byte {
	if line := statusLine(status); line != "" {
		wire = append(wire, line...)
	} else {
		wire = append(wire, "HTTP/1.1 "...)
		wire = strconv.AppendInt(wire, int64(status), 10)
		wire = append(wire, ' ')
		wire = append(wire, http.StatusText(status)...)
		wire = append(wire, "\r\n"...)
	}
	wire = append(wire, "Content-Type: application/json\r\n"...)
	if gzipped {
		wire = append(wire, "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"...)
	}
	if closeAfter {
		wire = append(wire, "Connection: close\r\n"...)
	}
	wire = append(wire, "Content-Length: "...)
	wire = strconv.AppendInt(wire, int64(len(body)), 10)
	wire = append(wire, "\r\n\r\n"...)
	return append(wire, body...)
}
