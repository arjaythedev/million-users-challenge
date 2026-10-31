package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

const (
	defaultHost = "0.0.0.0"
	defaultPort = "80"
)

func main() {
	databaseURL := os.Getenv("DATABASE_URL")
	if databaseURL == "" {
		log.Fatal("DATABASE_URL is required")
	}
	jwtSecret := os.Getenv("JWT_SECRET")
	if jwtSecret == "" {
		log.Fatal("JWT_SECRET is required")
	}

	// Read batch concurrency bounds active sessions. DB_POOL is the shared
	// ceiling for reads, the reserved writer, and profile lookups.
	poolSize := 128
	if value := os.Getenv("DB_POOL"); value != "" {
		parsed, err := strconv.Atoi(value)
		if err != nil || parsed < 1 || parsed > 512 {
			log.Fatalf("invalid DB_POOL=%q (must be 1..512)", value)
		}
		poolSize = parsed
	}

	poolConfig, err := pgxpool.ParseConfig(databaseURL)
	if err != nil {
		log.Fatalf("invalid DATABASE_URL: %v", err)
	}
	poolConfig.MaxConns = int32(poolSize)
	poolConfig.ConnConfig.RuntimeParams["jit"] = "off"
	poolConfig.ConnConfig.RuntimeParams["plan_cache_mode"] = "force_generic_plan"
	// Warm only a small set of connections so the first requests do not pay all
	// setup latency while avoiding a large startup spike on the database host.
	poolConfig.MinConns = int32(minInt(poolSize, 32))
	poolConfig.MaxConnIdleTime = 10 * time.Minute
	poolConfig.MaxConnLifetime = 30 * time.Minute

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	db, err := pgxpool.NewWithConfig(ctx, poolConfig)
	if err != nil {
		log.Fatalf("open postgres pool: %v", err)
	}
	defer db.Close()

	application := &application{db: db, reads: newReadBatcher(db), writes: newWriteBatcher(db), secret: []byte(jwtSecret), started: time.Now()}
	host := valueOr("HOST", defaultHost)
	port := valueOr("PORT", defaultPort)
	ports := []string{port}
	if extra := os.Getenv("EXTRA_PORT"); extra != "" && extra != port {
		ports = append(ports, extra)
	}

	log.Printf("go-kamkm event loops listening on %v (postgres pool %d)", ports, poolSize)
	if err := serveEventHTTP(ctx, application, host, ports); err != nil {
		log.Printf("HTTP server stopped: %v", err)
	}

	application.writes.close()
}

func valueOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
