package main

import (
	"bytes"
	"errors"
	"runtime"
	"testing"
)

// A slow database can leave many requests queued. Their response payloads must
// not consume memory until a worker has admitted the database operation.
func TestQueuedReadsDoNotRetainResponsePayloads(t *testing.T) {
	const count = 1024
	app := &application{responses: make(chan *exchange, count)}
	app.reads = &readBatcher{jobs: make(chan readJob, count), done: app.readDone}
	req := &request{route: routeFeed}
	runtime.GC()
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	for i := 0; i < count; i++ {
		app.start(&exchange{req: req})
	}
	queued := len(app.reads.jobs)
	runtime.GC()
	runtime.ReadMemStats(&after)
	// Always release the real completion path, including when the assertion
	// fails.
	failure := errors.New("test database unavailable")
	for i := 0; i < queued; i++ {
		job := <-app.reads.jobs
		app.reads.done(job.ex, readResult{err: failure})
	}
	for i := 0; i < queued; i++ {
		wire := encode(<-app.responses)
		if !bytes.HasPrefix(*wire, []byte("HTTP/1.1 500 ")) || !bytes.HasSuffix(*wire, bodyInternal) {
			t.Fatalf("failed read answered %q", *wire)
		}
		putBuffer(wire)
	}
	if queued != count {
		t.Fatalf("only %d of %d reads entered the queue", queued, count)
	}
	retained := int64(after.HeapAlloc) - int64(before.HeapAlloc)
	t.Logf("queued reads retained %d bytes", retained)
	// Allow the exchange records, but no 4-KiB response payload per queued
	// request.
	if retained > count*1024 {
		t.Fatalf("queued reads retained %d bytes, want at most %d", retained, count*1024)
	}
}
