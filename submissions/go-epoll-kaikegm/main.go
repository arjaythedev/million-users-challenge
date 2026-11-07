package main

import (
	"log"
	"os"
	"os/signal"
	"runtime"
	"strconv"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

const (
	// 64 leitores + 32 escritores = 96 conexões totais (Regra 5 permite até 512).
	defaultPGReaders = 64
	defaultPGWriters = 32
	defaultBatchUs   = 2000 // 2ms adaptive batching window on light iterations
)

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}

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
	ports := []string{envOr("PORT", "3000")}
	if extra := os.Getenv("EXTRA_PORT"); extra != "" {
		ports = append(ports, extra)
	}

	ip, err := resolveIPv4(host)
	if err != nil {
		log.Fatal(err)
	}

	// Raise soft RLIMIT_NOFILE to hard limit so 1M+ descriptors fit without issue.
	var lim unix.Rlimit
	if err := unix.Getrlimit(unix.RLIMIT_NOFILE, &lim); err == nil && lim.Cur < lim.Max {
		lim.Cur = lim.Max
		_ = unix.Setrlimit(unix.RLIMIT_NOFILE, &lim)
	}
	_ = unix.Getrlimit(unix.RLIMIT_NOFILE, &lim)
	// Virtual memory pages for slots are demand-paged by the OS only up to the highest open FD.
	slots = make([]slot, min(lim.Cur, 1<<24))

	authChecker, err := NewAuthChecker(secret)
	if err != nil {
		log.Fatalf("auth verifier: %v", err)
	}

	nloops := runtime.GOMAXPROCS(0)
	pgReaders := defaultPGReaders
	pgWriters := defaultPGWriters
	if v := os.Getenv("DB_POOL"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 4 {
			log.Fatalf("invalid DB_POOL %q", v)
		}
		pgReaders = max(2, (n*3)/4)
		pgWriters = max(2, n-pgReaders)
	}

	readersPerLoop := max(3, pgReaders/nloops)
	writersPerLoop := max(2, pgWriters/nloops)
	batchWindow := envMicros("BATCH_WINDOW_US", defaultBatchUs)
	started := time.Now()

	workers := make([]*Worker, nloops)
	for i := 0; i < nloops; i++ {
		w, err := newWorker(i, authChecker, dbURL, readersPerLoop, writersPerLoop, batchWindow, started)
		if err != nil {
			log.Fatal(err)
		}
		for _, portStr := range ports {
			p, err := strconv.Atoi(portStr)
			if err != nil {
				log.Fatalf("bad port %q", portStr)
			}
			if err := w.listen(ip, p); err != nil {
				log.Fatal(err)
			}
		}
		workers[i] = w
	}

	for _, w := range workers {
		go w.run()
	}

	log.Printf("go-epoll-kaikegm listening on %s:%v with %d unified epoll loops, %d+%d postgres conns per loop",
		host, ports, nloops, readersPerLoop, writersPerLoop)

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	<-sigCh
	log.Printf("Shutting down...")
}
