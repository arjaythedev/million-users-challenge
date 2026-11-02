package main

import (
	"errors"
	"runtime"
	"sync"
	"testing"
	"time"
)

// A slow database can leave many HTTP handlers queued. Their response payloads
// must not consume memory until a worker has admitted the database operation.
func TestQueuedReadsDoNotRetainResponsePayloads(t *testing.T) {
	const count = 1024
	reads := &readBatcher{jobs: make(chan readJob, count)}
	app := &application{reads: reads}
	req := &request{route: routeFeed}
	var done sync.WaitGroup
	done.Add(count)
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	for i := 0; i < count; i++ {
		go func() {
			defer done.Done()
			wire := app.respond(req)
			putBuffer(wire)
		}()
	}
	deadline := time.Now().Add(5 * time.Second)
	for len(reads.jobs) != count && time.Now().Before(deadline) {
		runtime.Gosched()
	}
	queued := len(reads.jobs)
	runtime.GC()
	runtime.ReadMemStats(&after)
	// Always release the real request path, including when the assertion fails.
	failure := errors.New("test database unavailable")
	for i := 0; i < count; i++ {
		job := <-reads.jobs
		job.reply <- readResult{err: failure}
	}
	done.Wait()
	if queued != count {
		t.Fatalf("only %d of %d reads entered the queue", queued, count)
	}
	retained := int64(after.HeapAlloc) - int64(before.HeapAlloc)
	t.Logf("queued reads retained %d bytes", retained)
	// Allow channels, goroutine records and runtime bookkeeping, but neither
	// 4-KiB response payload per queued handler.
	if retained > count*3072 {
		t.Fatalf("queued reads retained %d bytes, want at most %d", retained, count*3072)
	}
}
