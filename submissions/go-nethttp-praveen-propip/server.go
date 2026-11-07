package main

import (
	"compress/gzip"
	"context"
	"log/slog"
	"net"
	"net/http"
	"time"

	"github.com/klauspost/compress/gzhttp"
)

// newServer builds one http.Server bound to addr serving h. The load generator
// idles up to ~40 s between requests, so idle keep-alive connections must stay
// open well past that (SPEC.md runtime contract).
//
// TCP keep-alive probes are disabled: at hundreds of thousands of mostly idle
// connections Go's default (one probe every 15 s per connection) is a constant
// tens-of-thousands-packets-per-second tax that buys nothing here — the server
// enforces its own idle timeout instead.
func newServer(addr string, h http.Handler) *http.Server {
	return &http.Server{
		Addr:    addr,
		Handler: h,
		// IdleTimeout covers keep-alive idle time; ReadHeaderTimeout bounds
		// slowloris-style header dribbles. No ReadTimeout/WriteTimeout: request
		// bodies are tiny and DB calls can legitimately take a few seconds.
		IdleTimeout:       120 * time.Second,
		ReadHeaderTimeout: 30 * time.Second,
	}
}

// listen binds addr with TCP keep-alive probes disabled and SO_REUSEPORT-style
// defaults, so all ports are bound before anything starts serving.
func listen(ctx context.Context, addr string) (net.Listener, error) {
	lc := net.ListenConfig{KeepAlive: -1}
	return lc.Listen(ctx, "tcp", addr)
}

// gzipWrapper compresses every JSON response when the client sends
// Accept-Encoding: gzip (the benchmark does). MinSize(0) matches the measured
// configuration from examples/go; gzhttp only takes a pooled deflate writer
// once the handler actually writes, unlike a middleware that holds one per
// queued request.
func gzipWrapper() (func(http.Handler) http.HandlerFunc, error) {
	return gzhttp.NewWrapper(
		gzhttp.MinSize(0),
		gzhttp.CompressionLevel(gzip.DefaultCompression),
	)
}

// serve runs s on ln until ctx is cancelled or Serve fails, logging failures.
func serve(ctx context.Context, s *http.Server, ln net.Listener) error {
	err := s.Serve(ln)
	if ctx.Err() != nil || err == http.ErrServerClosed {
		return nil
	}
	slog.Error("server stopped", "addr", s.Addr, "err", err)
	return err
}
