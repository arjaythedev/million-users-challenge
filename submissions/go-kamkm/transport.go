package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/panjf2000/gnet/v2"
)

// Event loops own the socket state. Only active requests start a goroutine.
type eventServer struct {
	gnet.BuiltinEventEngine
	app    *application
	ready  chan gnet.Engine
	active sync.WaitGroup
}
type eventConnection struct {
	busy      bool
	reading   bool
	continued bool
	timer     *time.Timer
}

func (s *eventServer) OnBoot(e gnet.Engine) gnet.Action { s.ready <- e; return gnet.None }
func (s *eventServer) OnOpen(c gnet.Conn) ([]byte, gnet.Action) {
	state := &eventConnection{}
	state.timer = time.AfterFunc(180*time.Second, func() { _ = c.Close() })
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

var requestReaders = sync.Pool{New: func() any { return bufio.NewReaderSize(nil, 4096) }}

var malformedResponse = []byte("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\nConnection: close\r\n\r\n{\"error\":\"malformed JSON body\"}")

func (s *eventServer) dispatch(c gnet.Conn) gnet.Action {
	state, ok := c.Context().(*eventConnection)
	if !ok || state.busy || c.InboundBuffered() == 0 {
		return gnet.None
	}
	if !state.reading {
		state.timer.Reset(15 * time.Second)
		state.reading = true
	}
	data, _ := c.Peek(-1)
	head := bytes.Index(data, []byte("\r\n\r\n"))
	bad := func() gnet.Action { _, _ = c.Write(malformedResponse); return gnet.Close }
	if head < 0 {
		if len(data) > 16<<10 {
			return bad()
		}
		return gnet.None
	}
	if head+4 > 16<<10 {
		return bad()
	}
	input := bytes.NewReader(data)
	reader := requestReaders.Get().(*bufio.Reader)
	reader.Reset(input)
	defer func() { reader.Reset(nil); requestReaders.Put(reader) }()
	request, err := http.ReadRequest(reader)
	if err != nil {
		return bad()
	}
	if request.ContentLength > maxBodyBytes {
		return bad()
	}
	var body []byte
	if request.Body != http.NoBody {
		body, err = io.ReadAll(io.LimitReader(request.Body, maxBodyBytes+1))
	}
	if errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, io.EOF) {
		if !state.continued && request.Header.Get("Expect") == "100-continue" {
			_, _ = c.Write([]byte("HTTP/1.1 100 Continue\r\n\r\n"))
			state.continued = true
		}
		if len(data) > 36<<10 {
			return bad()
		}
		return gnet.None
	}
	if err != nil || len(body) > maxBodyBytes {
		return bad()
	}
	used := len(data) - input.Len() - reader.Buffered()
	_, _ = c.Discard(used)
	request.Body = io.NopCloser(bytes.NewReader(body))
	request.ContentLength = int64(len(body))
	request.RemoteAddr = c.RemoteAddr().String()
	state.busy = true
	state.reading = false
	state.continued = false
	s.active.Add(1)
	go func() {
		defer s.active.Done()
		ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
		defer cancel()
		output := &bufferedResponse{headers: make(http.Header)}
		s.app.ServeHTTP(output, request.WithContext(ctx))
		wire := output.encode(request.Close)
		_ = c.AsyncWrite(wire, func(c gnet.Conn, err error) error {
			current, ok := c.Context().(*eventConnection)
			if !ok || err != nil {
				return nil
			}
			if request.Close {
				_ = c.Close()
				return nil
			}
			current.busy = false
			current.timer.Reset(180 * time.Second)
			if s.dispatch(c) == gnet.Close {
				_ = c.Close()
			}
			return nil
		})
	}()
	return gnet.None
}

type bufferedResponse struct {
	headers http.Header
	body    bytes.Buffer
	status  int
}

func (r *bufferedResponse) Header() http.Header { return r.headers }
func (r *bufferedResponse) WriteHeader(status int) {
	if r.status == 0 {
		r.status = status
	}
}
func (r *bufferedResponse) Write(body []byte) (int, error) {
	if r.status == 0 {
		r.status = 200
	}
	return r.body.Write(body)
}
func (r *bufferedResponse) encode(close bool) []byte {
	if r.status == 0 {
		r.status = 200
	}
	var wire bytes.Buffer
	fmt.Fprintf(&wire, "HTTP/1.1 %d %s\r\n", r.status, http.StatusText(r.status))
	r.headers.Set("Content-Length", fmt.Sprint(r.body.Len()))
	if close {
		r.headers.Set("Connection", "close")
	}
	_ = r.headers.Write(&wire)
	wire.WriteString("\r\n")
	wire.Write(r.body.Bytes())
	return wire.Bytes()
}
func serveEventHTTP(ctx context.Context, app *application, host string, ports []string) error {
	server := &eventServer{app: app, ready: make(chan gnet.Engine, 1)}
	addresses := make([]string, len(ports))
	for i, port := range ports {
		addresses[i] = "tcp://" + net.JoinHostPort(host, port)
	}
	done := make(chan error, 1)
	go func() {
		done <- gnet.Rotate(server, addresses, gnet.WithNumEventLoop(4), gnet.WithReuseAddr(true), gnet.WithReusePort(true), gnet.WithTCPNoDelay(gnet.TCPNoDelay), gnet.WithTCPKeepAlive(0))
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
