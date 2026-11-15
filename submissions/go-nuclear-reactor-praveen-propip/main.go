//go:build linux

// Command feedreactor is the million-users-challenge feed API on a hand-rolled
// stack: one single-threaded Go process per core, each with its own epoll
// loop, SO_REUSEPORT listeners and pipelined Postgres sockets; hand-parsed
// HTTP/1.1, per-request HS256 and byte-level JSON.
//
// Configuration (SPEC.md runtime contract): DATABASE_URL, JWT_SECRET, HOST,
// PORT, EXTRA_PORT.
//
// The process that start.sh runs is a small supervisor. Each worker is the
// same binary with GOMAXPROCS=1: separate processes share no descriptor table,
// address space, allocator or scheduler, so the kernel's single-threaded fast
// paths apply and the Go runtime has nothing to coordinate.
package main

import (
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strconv"
	"syscall"
	"time"

	"golang.org/x/sys/unix"
)

const (
	workerEnv = "FEEDREACTOR_WORKER"
	dbPool    = 64 // Postgres connections in all; pipelining needs few, and rule 5 allows 512
)

func main() {
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo})))

	dsnURL := os.Getenv("DATABASE_URL")
	if dsnURL == "" {
		fatal("missing required environment variable DATABASE_URL")
	}
	secret := os.Getenv("JWT_SECRET")
	if secret == "" {
		fatal("missing required environment variable JWT_SECRET")
	}
	jwtSecret = []byte(secret)
	dsn, err := parseDSN(dsnURL)
	if err != nil {
		fatal("DATABASE_URL: " + err.Error())
	}
	if id := os.Getenv(workerEnv); id != "" {
		worker(id, dsn)
		return
	}
	supervise()
}

func fatal(msg string) {
	slog.Error(msg)
	os.Exit(2)
}

// supervise starts one worker per usable core (GOMAXPROCS already follows the
// CPU affinity mask and cgroup limit) and takes them all down on SIGTERM. A
// worker that dies is not replaced: the others keep serving, and the server
// exits with the last one.
func supervise() {
	// A worker's parent-death signal fires when the thread that forked it
	// exits, so all forks happen on this one locked thread.
	runtime.LockOSThread()
	n := runtime.GOMAXPROCS(0)
	exe, err := os.Executable()
	if err != nil {
		fatal(err.Error())
	}
	var si unix.Sysinfo_t
	_ = unix.Sysinfo(&si)
	memLimit := uint64(si.Totalram) * uint64(si.Unit) / 4 * 3 / uint64(n)

	type exit struct {
		i      int
		status syscall.WaitStatus
	}
	exits := make(chan exit, n)
	workers := make([]*exec.Cmd, n)
	for i := 0; i < n; i++ {
		cmd := exec.Command(exe)
		// No GC cycles on the request path: nothing there allocates, and the
		// memory limit still bounds the rare rest.
		cmd.Env = append(os.Environ(),
			workerEnv+"="+strconv.Itoa(i)+"/"+strconv.Itoa(n),
			"GOMAXPROCS=1", "GOGC=off", "GOMEMLIMIT="+strconv.FormatUint(memLimit, 10))
		cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
		cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
		if err := cmd.Start(); err != nil {
			fatal("start worker: " + err.Error())
		}
		workers[i] = cmd
		go func() {
			_ = cmd.Wait()
			exits <- exit{i, cmd.ProcessState.Sys().(syscall.WaitStatus)}
		}()
	}
	slog.Info("feedreactor serving", "workers", n, "pid", os.Getpid(), "go", runtime.Version())

	stopAll := func() {
		for _, w := range workers {
			_ = w.Process.Signal(syscall.SIGTERM)
		}
	}
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	for alive := n; alive > 0; alive-- {
		select {
		case <-sig:
			slog.Info("shutting down")
			stopAll()
			time.Sleep(100 * time.Millisecond)
			os.Exit(0)
		case e := <-exits:
			if e.status.Signaled() && e.status.Signal() == syscall.SIGTERM {
				stopAll() // someone is stopping the service through a worker
				os.Exit(0)
			}
			slog.Error("worker died", "worker", e.i, "status", int(e.status), "left", alive-1)
		}
	}
	os.Exit(1)
}

// worker runs one event loop. SIGTERM's default action ends it: it holds
// nothing that needs flushing, every commit is Postgres's.
func worker(id string, dsn *pgDSN) {
	var i, n int
	if _, err := fmt.Sscanf(id, "%d/%d", &i, &n); err != nil || n < 1 {
		fatal("bad " + workerEnv + ": " + id)
	}
	// The ring belongs to this thread.
	runtime.LockOSThread()
	// Each process's share of the pool. Group commit lets few connections
	// carry the writes: an eighth, at least one.
	conns := max(2, dbPool/n)
	writers := max(1, conns/8)

	host := os.Getenv("HOST")
	if host == "" {
		host = "127.0.0.1"
	}
	port := os.Getenv("PORT")
	if port == "" {
		port = "3000"
	}
	l, err := newLoop(dsn, conns-writers, writers)
	if err != nil {
		fatal(err.Error())
	}
	l.connectAll()
	for _, p := range []string{port, os.Getenv("EXTRA_PORT")} {
		if p == "" {
			continue
		}
		if err := l.listen(host, p); err != nil {
			fatal("bind " + p + ": " + err.Error())
		}
	}
	l.run()
}

// ---- bounded logging helpers (hot paths never log) ----

func slogError(msg string, err error, args ...any) {
	if err != nil {
		args = append(args, "err", err.Error())
	}
	slog.Error(msg, args...)
}

func slogWarn(msg string, args ...any) { slog.Warn(msg, args...) }
