package main

import (
	"compress/gzip"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"log"
	"net"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/klauspost/compress/gzhttp"
	"github.com/lesismal/nbio"
	"github.com/lesismal/nbio/nbhttp"
)

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

func main() {
	if os.Getenv("DATABASE_URL") == "" || os.Getenv("JWT_SECRET") == "" {
		log.Fatal("DATABASE_URL and JWT_SECRET are required")
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg, err := pgxpool.ParseConfig(os.Getenv("DATABASE_URL"))
	if err != nil {
		log.Fatal(err)
	}
	// Bound the total even when DATABASE_URL includes a larger pool_max_conns.
	cfg.MaxConns = int32(min(128, 8*runtime.GOMAXPROCS(0)))
	db, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		log.Fatal(err)
	}
	defer db.Close()
	pingCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	err = db.Ping(pingCtx)
	cancel()
	if err != nil {
		log.Fatal(err)
	}

	secret := []byte(os.Getenv("JWT_SECRET"))
	a := &app{db: db, jwtSecret: secret, started: time.Now()}
	a.jwtMAC.New = func() any { return hmac.New(sha256.New, secret) }
	// Compress feeds and large posts; tiny error/like responses cost more CPU than bandwidth saved.
	wrap, err := gzhttp.NewWrapper(gzhttp.MinSize(1024), gzhttp.CompressionLevel(gzip.BestSpeed))
	if err != nil {
		log.Fatal(err)
	}
	addrs := []string{net.JoinHostPort(envOr("HOST", "127.0.0.1"), envOr("PORT", "3000"))}
	if extra := os.Getenv("EXTRA_PORT"); extra != "" {
		addrs = append(addrs, net.JoinHostPort(envOr("HOST", "127.0.0.1"), extra))
	}
	lc := net.ListenConfig{KeepAlive: -1}
	// nbio defaults to a 2M fd table; the challenge searches up to 3M clients.
	nbio.MaxOpenFiles = 4_194_304
	engine := nbhttp.NewEngine(nbhttp.Config{
		Name:                   "feed",
		Addrs:                  addrs,
		Handler:                wrap(a.routes()),
		IOMod:                  nbhttp.IOModNonBlocking,
		NPoller:                max(1, runtime.GOMAXPROCS(0)/4),
		MaxLoad:                4_000_000,
		MaxHTTPBodySize:        maxRequestBody,
		ReadLimit:              32 << 10,
		KeepaliveTime:          120 * time.Second,
		MessageHandlerPoolSize: 1024,
		SupportServerOnly:      true,
		Listen: func(network, addr string) (net.Listener, error) {
			return lc.Listen(ctx, network, addr)
		},
	})
	if err := engine.Start(); err != nil {
		log.Fatal(err)
	}
	log.Printf("listening on %v, GOMAXPROCS=%d, pool=%d", addrs, runtime.GOMAXPROCS(0), cfg.MaxConns)
	<-ctx.Done()
	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := engine.Shutdown(shutdownCtx); err != nil {
		log.Printf("[shutdown] %v", err)
		engine.Stop()
	}
}
