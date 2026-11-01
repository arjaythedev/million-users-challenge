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
	"golang.org/x/sys/unix"
)

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

// envMicros reads a non-negative microsecond count; 0 disables the linger.
func envMicros(name string, fallback int) time.Duration {
	us := fallback
	if v := os.Getenv(name); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			log.Fatalf("invalid %s %q", name, v)
		}
		us = n
	}
	return time.Duration(us) * time.Microsecond
}

func main() {
	// Ignore SIGPIPE to avoid dying on broken client connections
	signal.Ignore(syscall.SIGPIPE)

	dbURL := os.Getenv("DATABASE_URL")
	if dbURL == "" {
		log.Fatal("missing required environment variable DATABASE_URL")
	}
	secret := []byte(os.Getenv("JWT_SECRET"))
	if len(secret) == 0 {
		log.Fatal("missing required environment variable JWT_SECRET")
	}
	host := envOr("HOST", "127.0.0.1")
	port := envOr("PORT", "3000")
	extraPort := os.Getenv("EXTRA_PORT")

	procs := runtime.GOMAXPROCS(0)
	// 512 is the spec maximum. min(384, max(128, 24*procs)):
	// 16c -> 384, 8c -> 192, 4c -> 128.
	totalPool := min(384, max(128, 24*procs))
	if v := os.Getenv("DB_POOL"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 2 {
			log.Fatalf("invalid DB_POOL %q", v)
		}
		totalPool = n
	}

	readConns := max(2, (totalPool*3)/4)
	writeConns := max(2, totalPool-readConns)

	readCfg, err := pgxpool.ParseConfig(dbURL)
	if err != nil {
		log.Fatalf("DATABASE_URL parse: %v", err)
	}
	readCfg.MaxConns = int32(readConns)
	if readCfg.ConnConfig.RuntimeParams == nil {
		readCfg.ConnConfig.RuntimeParams = make(map[string]string)
	}
	readCfg.ConnConfig.RuntimeParams["plan_cache_mode"] = "force_generic_plan"
	readCfg.ConnConfig.RuntimeParams["jit"] = "off"
	readCfg.ConnConfig.RuntimeParams["max_parallel_workers_per_gather"] = "0"
	readCfg.ConnConfig.RuntimeParams["enable_seqscan"] = "off"

	writeCfg, err := pgxpool.ParseConfig(dbURL)
	if err != nil {
		log.Fatalf("DATABASE_URL parse: %v", err)
	}
	writeCfg.MaxConns = int32(writeConns)
	if writeCfg.ConnConfig.RuntimeParams == nil {
		writeCfg.ConnConfig.RuntimeParams = make(map[string]string)
	}
	writeCfg.ConnConfig.RuntimeParams["plan_cache_mode"] = "force_generic_plan"
	writeCfg.ConnConfig.RuntimeParams["jit"] = "off"
	writeCfg.ConnConfig.RuntimeParams["max_parallel_workers_per_gather"] = "0"

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	readDB, err := pgxpool.NewWithConfig(ctx, readCfg)
	if err != nil {
		log.Fatalf("read db connect: %v", err)
	}
	defer readDB.Close()

	writeDB, err := pgxpool.NewWithConfig(ctx, writeCfg)
	if err != nil {
		log.Fatalf("write db connect: %v", err)
	}
	defer writeDB.Close()

	if err := readDB.Ping(ctx); err != nil {
		log.Fatalf("read db ping: %v", err)
	}
	if err := writeDB.Ping(ctx); err != nil {
		log.Fatalf("write db ping: %v", err)
	}

	authChecker, err := NewAuthChecker(secret)
	if err != nil {
		log.Fatalf("auth verifier: %v", err)
	}

	readWorkers := min(48, max(2, 3*procs))
	writeWorkers := min(16, max(2, procs))
	readLinger := envMicros("READ_LINGER_US", 500)
	writeLinger := envMicros("WRITE_LINGER_US", 500)
	dbPool := NewDBPool(readDB, writeDB, readWorkers, writeWorkers, readLinger, writeLinger)

	// Create multi-reactor epoll workers across cores
	for i := 0; i < procs; i++ {
		epollFd, err := unix.EpollCreate1(unix.EPOLL_CLOEXEC)
		if err != nil {
			log.Fatalf("epoll_create1: %v", err)
		}

		l1, err := createListener(host, port)
		if err != nil {
			log.Fatalf("createListener port %s: %v", port, err)
		}
		ev1 := unix.EpollEvent{
			Events: unix.EPOLLIN | unix.EPOLLET,
			Fd:     int32(l1),
		}
		if err := unix.EpollCtl(epollFd, unix.EPOLL_CTL_ADD, l1, &ev1); err != nil {
			log.Fatalf("epoll_ctl l1: %v", err)
		}

		l2 := -1
		if extraPort != "" {
			var err error
			l2, err = createListener(host, extraPort)
			if err != nil {
				log.Fatalf("createListener extra port %s: %v", extraPort, err)
			}
			ev2 := unix.EpollEvent{
				Events: unix.EPOLLIN | unix.EPOLLET,
				Fd:     int32(l2),
			}
			if err := unix.EpollCtl(epollFd, unix.EPOLL_CTL_ADD, l2, &ev2); err != nil {
				log.Fatalf("epoll_ctl l2: %v", err)
			}
		}

		worker := &Worker{
			id:          i,
			epollFd:     epollFd,
			listener1:   l1,
			listener2:   l2,
			dbPool:      dbPool,
			authChecker: authChecker,
		}
		go worker.run()
	}

	log.Printf("go-epoll-kaikegm running on %s:%s (extra %s) with %d epoll workers, %d DB workers (%d read, %d write)", host, port, extraPort, procs, readWorkers+writeWorkers, readWorkers, writeWorkers)

	<-ctx.Done()
	log.Printf("Shutting down...")
}
