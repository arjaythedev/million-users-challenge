// Command feedapi serves the million-users-challenge feed API (SPEC.md) on the
// standard Go stack: net/http with the 1.22+ ServeMux, pgx/v5 pgxpool with
// prepared statements, gzhttp response compression and golang-jwt bearer auth.
//
// All configuration comes from the environment (SPEC.md runtime contract):
// DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT, plus DB_POOL/DB_TIMEOUT_MS.
package main

import (
	"context"
	"errors"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"
)

func main() {
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo})))

	cfg, err := LoadConfig()
	if err != nil {
		slog.Error("configuration", "err", err)
		os.Exit(2)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	pool, err := newPool(ctx, cfg)
	if err != nil {
		slog.Error("database", "err", err)
		os.Exit(1)
	}
	defer pool.Close()

	a := &app{cfg: cfg, pool: pool, started: time.Now()}
	compress, err := gzipWrapper()
	if err != nil {
		slog.Error("gzip wrapper", "err", err)
		os.Exit(1)
	}
	handler := compress(a.routes())

	addrs := []string{net.JoinHostPort(cfg.Host, cfg.Port)}
	if cfg.ExtraPort != "" {
		addrs = append(addrs, net.JoinHostPort(cfg.Host, cfg.ExtraPort))
	}
	// Bind every listener before serving so a bad address fails startup.
	listeners := make([]net.Listener, 0, len(addrs))
	servers := make([]*http.Server, 0, len(addrs))
	for _, addr := range addrs {
		ln, err := listen(ctx, addr)
		if err != nil {
			slog.Error("bind", "addr", addr, "err", err)
			os.Exit(1)
		}
		listeners = append(listeners, ln)
		servers = append(servers, newServer(addr, handler))
	}

	slog.Info("feedapi serving",
		"addrs", addrs,
		"pid", os.Getpid(),
		"GOMAXPROCS", runtime.GOMAXPROCS(0),
		"pool", cfg.PoolSize,
		"go", runtime.Version(),
	)

	errc := make(chan error, len(servers))
	for i := range servers {
		go func(i int) { errc <- serve(ctx, servers[i], listeners[i]) }(i)
	}

	// Run until SIGINT/SIGTERM (ctx cancelled) or a server fails. Serve does
	// not react to context cancellation by itself — the Shutdown below is
	// what closes the listeners and drains the connections.
	select {
	case <-ctx.Done():
	case err := <-errc:
		if err != nil {
			stop()
			slog.Error("server failed", "err", err)
			os.Exit(1)
		}
	}
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	for _, s := range servers {
		if err := s.Shutdown(shutdownCtx); err != nil && !errors.Is(err, context.DeadlineExceeded) {
			slog.Warn("shutdown", "err", err)
		}
	}
	slog.Info("feedapi stopped")
}
