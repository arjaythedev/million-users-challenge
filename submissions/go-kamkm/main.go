package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"runtime"
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

	// DB_POOL is the shared ceiling for read pipelines, write lanes and health
	// checks.
	poolSize := intSetting("DB_POOL", 128, 1, 512)
	writeLanes := intSetting("WRITE_LANES", minInt(4, maxInt(1, poolSize/16)), 1, 64)
	readWorkers := intSetting("READ_WORKERS", minInt(16, maxInt(1, poolSize-writeLanes)), 1, 512)
	readBatch := intSetting("READ_BATCH", 64, 1, 256)
	loops := intSetting("HTTP_LOOPS", runtime.GOMAXPROCS(0), 1, 256)
	// Writers keep their connections only when at least one connection is
	// left for reads; with DB_POOL=1 they release it after every group.
	reserve := poolSize > writeLanes

	poolConfig, err := pgxpool.ParseConfig(databaseURL)
	if err != nil {
		log.Fatalf("invalid DATABASE_URL: %v", err)
	}
	poolConfig.MaxConns = int32(poolSize)
	poolConfig.ConnConfig.RuntimeParams["jit"] = "off"
	poolConfig.ConnConfig.RuntimeParams["plan_cache_mode"] = "force_generic_plan"
	// Warm the connections the pipelines use so the first requests do not
	// pay connection setup.
	poolConfig.MinConns = int32(minInt(poolSize, readWorkers+writeLanes))
	poolConfig.MaxConnIdleTime = 10 * time.Minute
	poolConfig.MaxConnLifetime = 30 * time.Minute

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	db, err := pgxpool.NewWithConfig(ctx, poolConfig)
	if err != nil {
		log.Fatalf("open postgres pool: %v", err)
	}
	defer db.Close()

	application := &application{
		db:      db,
		reads:   newReadBatcher(db, readWorkers, readBatch),
		writes:  newWriteBatcher(db, writeLanes, reserve),
		secret:  []byte(jwtSecret),
		started: time.Now(),
	}
	host := valueOr("HOST", defaultHost)
	port := valueOr("PORT", defaultPort)
	ports := []string{port}
	if extra := os.Getenv("EXTRA_PORT"); extra != "" && extra != port {
		ports = append(ports, extra)
	}

	log.Printf("go-kamkm: %d event loops on %v; postgres pool %d, %d read pipelines (batch %d), %d write lanes",
		loops, ports, poolSize, readWorkers, readBatch, writeLanes)
	if err := serveEventHTTP(ctx, application, host, ports, loops); err != nil {
		log.Printf("HTTP server stopped: %v", err)
	}

	application.writes.close()
}

func intSetting(name string, fallback, low, high int) int {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil || parsed < low || parsed > high {
		log.Fatalf("invalid %s=%q (must be %d..%d)", name, value, low, high)
	}
	return parsed
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

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}
