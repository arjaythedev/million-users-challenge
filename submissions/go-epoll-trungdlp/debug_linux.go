//go:build debug

// A debug build (go build -tags debug) logs every loop's state on SIGUSR1:
// busy clients, buffered input and output, and each Postgres connection's queue.
package main

import (
	"log"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
)

var dumpGen atomic.Int64

func init() {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGUSR1)
	go func() {
		for range ch {
			dumpGen.Add(1)
		}
	}()
	seen := map[int]int64{}
	debugHook = func(l *loop) {
		g := dumpGen.Load()
		if seen[l.id] == g {
			return
		}
		seen[l.id] = g
		busy := 0
		for fd := range slots {
			s := slots[fd]
			if s.kind == kindClient && int(s.loop) == l.id && s.flags&flagBusy != 0 {
				busy++
			}
		}
		log.Printf("[debug] loop %d: busy clients %d, inbuf %d, outbuf %d, dirty %d, ndead %d",
			l.id, busy, len(l.inbuf), len(l.outbuf), len(l.dirty), l.ndead)
		for _, c := range append(append([]*pgc{}, l.readers...), l.writers...) {
			log.Printf("[debug]   pg fd=%d write=%v dead=%v dirty=%v inBatch=%v queued=%d inflight=%d out=%d in=%d",
				c.fd, c.write, c.dead, c.dirty, c.inBatch, len(c.q)-c.qhead, c.inflight, len(c.out), len(c.in))
		}
	}
}
