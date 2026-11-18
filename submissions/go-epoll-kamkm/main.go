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

	"github.com/jackc/pgx/v5/pgconn"
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

	// One event loop per core. A loop never shares a PostgreSQL session, so
	// there are at most as many loops as sessions.
	poolSize := intSetting("DB_POOL", 128, 1, 512)
	loops := minInt(intSetting("HTTP_LOOPS", runtime.GOMAXPROCS(0), 1, 256), poolSize)
	// READ_SESSIONS is the number of read sessions over all loops. A session
	// sends a batch and waits for its reply; a loop fills one session per
	// window and uses more only when batches are full or the first are still
	// out, so extra sessions cost nothing until the load needs them. When the
	// database is slow, though, every request that waits for a free session
	// past 40 ms costs the network two extra ACKs, so there are plenty.
	readers := (intSetting("READ_SESSIONS", 64, 1, 512) + loops - 1) / loops
	// WRITE_SESSIONS is per loop: each commits its loop's writes in groups,
	// the second only while the first group is still committing.
	writers := intSetting("WRITE_SESSIONS", 2, 1, 16)
	if each := poolSize / loops; readers+writers > each {
		// A small pool: one session per loop carries reads and writes.
		writers = minInt(writers, each-1)
		readers = maxInt(1, each-writers)
	}
	plan := sessionPlan{
		readers: readers,
		writers: writers,
		batch:   intSetting("READ_BATCH", 64, 1, 256),
		// Gather windows in microseconds: the least time between two batches
		// that a loop starts, unless a full batch is waiting. 0 sends whatever
		// is queued as soon as a session is free.
		readWindow:  int64(intSetting("READ_WINDOW_US", 3000, 0, 100000)) * int64(time.Microsecond),
		writeWindow: int64(intSetting("WRITE_WINDOW_US", 3000, 0, 100000)) * int64(time.Microsecond),
	}

	config, err := pgconn.ParseConfig(databaseURL)
	if err != nil {
		log.Fatalf("invalid DATABASE_URL: %v", err)
	}
	if err := plainOnly(config); err != nil {
		log.Fatal(err)
	}
	config.RuntimeParams["jit"] = "off"
	config.RuntimeParams["plan_cache_mode"] = "force_generic_plan"

	// The loops never yield to the Go scheduler while there is work. One
	// spare processor keeps the garbage collector and the connection dialers
	// from having to preempt a loop to run.
	if os.Getenv("GOMAXPROCS") == "" {
		runtime.GOMAXPROCS(loops + 1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	app := &application{
		secret:  []byte(jwtSecret),
		started: time.Now(),
		plan:    plan,
		stop:    ctx.Done(),
		dial:    func() (int, error) { return dialDatabase(config) },
		trace:   intSetting("LOOP_TRACE", 0, 0, 1) == 1,
	}
	host := valueOr("HOST", defaultHost)
	port := valueOr("PORT", defaultPort)
	ports := []string{port}
	if extra := os.Getenv("EXTRA_PORT"); extra != "" && extra != port {
		ports = append(ports, extra)
	}

	log.Printf("go-epoll-kamkm: %d event loops on %v; per loop %d read sessions (batch %d, window %v) and %d write sessions (window %v)",
		loops, ports, plan.readers, plan.batch, time.Duration(plan.readWindow), plan.writers, time.Duration(plan.writeWindow))
	if err := serve(app, host, ports, loops); err != nil {
		log.Fatalf("HTTP server: %v", err)
	}
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
