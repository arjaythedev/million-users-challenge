package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"sync"
	"syscall"
	"time"

	"github.com/cloudwego/hertz/pkg/app/server"
	"github.com/cloudwego/hertz/pkg/common/hlog"
	"github.com/cloudwego/netpoll"
	"github.com/jackc/pgx/v5/pgxpool"
)

func init() {
	// netpoll preserves a connection buffer node indefinitely under 8Kb.
	netpoll.LinkBufferCap = 16384
	_ = netpoll.Configure(netpoll.Config{
		PollerNum:  4,
		BufferSize: 16384,
		Runner: func(ctx context.Context, f func()) {
			go f()
		},
	})
}

func main() {
	ctx := context.Background()
	if err := run(ctx); err != nil {
		log.Fatalln(err)
	}
}

func run(ctx context.Context) error {
	ctx, stop := signal.NotifyContext(ctx, os.Interrupt, syscall.SIGTERM)
	defer stop()

	var p parser
	cfg := config{
		dbUrl:             p.str("DATABASE_URL", ""),
		dbPool:            p.int("DB_POOL", 128),
		readWorkers:       p.int("READ_WORKERS", 16),
		writeWorkers:      p.int("WRITE_WORKERS", 8),
		maxBatch:          p.int("MAX_BATCH", 512),
		groupCommit:       p.boolean("GROUP_COMMIT", true),
		jwtSecret:         p.bytes("JWT_SECRET", nil),
		host:              p.str("HOST", "127.0.0.1"),
		port:              p.str("PORT", "3000"),
		extraPort:         p.str("EXTRA_PORT", ""),
		debugPort:         p.str("DEBUG_PORT", ""),
		idleTimeout:       p.dur("IDLE_TIMEOUT", 120*time.Second),
		keepAliveTimeout:  p.dur("KEEP_ALIVE_TIMEOUT", 90*time.Second),
		readTimeout:       p.dur("READ_TIMEOUT", 30*time.Second),
		keepAlivesEnabled: p.boolean("KEEP_ALIVES_ENABLED", true),
	}

	if err := cfg.validate(); err != nil {
		return err
	}

	dbCfg, err := pgxpool.ParseConfig(cfg.dbUrl)
	if err != nil {
		return err
	}
	dbCfg.MaxConns = int32(cfg.dbPool)

	db, err := pgxpool.NewWithConfig(ctx, dbCfg)
	if err != nil {
		return fmt.Errorf("db connect: %w", err)
	}

	if err := db.Ping(ctx); err != nil {
		return fmt.Errorf("db ping: %w", err)
	}

	pipe := NewPipe(db, PipeOptions{
		ReadWorkers:  cfg.readWorkers,
		WriteWorkers: cfg.writeWorkers,
		MaxBatch:     cfg.maxBatch,
		GroupCommit:  cfg.groupCommit,
	})
	defer pipe.Close()

	a := &app{
		jwtSecret: cfg.jwtSecret,
		reads:     pipe.Reads(),
		writes:    pipe.Writes(),
		started:   time.Now(),
	}

	hlog.SetSilentMode(true)
	newServer := func(host, p string) *server.Hertz {
		srv := server.New(
			server.WithHostPorts(net.JoinHostPort(host, p)),
			server.WithIdleTimeout(cfg.idleTimeout),
			server.WithReadTimeout(cfg.readTimeout),
			server.WithKeepAlive(cfg.keepAlivesEnabled),
			server.WithKeepAliveTimeout(cfg.keepAliveTimeout),
			server.WithMaxKeepBodySize(0),
		)
		a.routes(srv)
		return srv
	}

	servers := make([]*server.Hertz, 0, 1)
	servers = append(servers, newServer(cfg.host, cfg.port))
	if cfg.extraPort != "" {
		servers = append(servers, newServer(cfg.host, cfg.extraPort))
	}

	if cfg.debugPort != "" {
		dbg := &http.Server{Addr: net.JoinHostPort("127.0.0.1", cfg.debugPort), Handler: DebugMux()}
		go func() {
			if err := dbg.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
				log.Printf("debug server: %v", err)
			}
		}()
		defer dbg.Close()
	}

	srvErrs := make(chan error, len(servers))
	var wg sync.WaitGroup
	for _, srv := range servers {
		wg.Go(func() {
			if err := srv.Run(); err != nil {
				srvErrs <- err
			}
		})
	}

	select {
	case err := <-srvErrs:
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return fmt.Errorf("server: %w", err)
	case <-ctx.Done():
		log.Println("server shutting down")
		sctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		for _, srv := range servers {
			if err := srv.Shutdown(sctx); err != nil {
				_ = srv.Close()
			}
		}
	}

	wg.Wait()

	return nil
}
