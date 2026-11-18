//go:build isal

package main

import (
	"bytes"
	"testing"
)

// The fallback writes valid gzip for empty, small and multi-block bodies.
func TestStoredGzip(t *testing.T) {
	for _, size := range []int{0, 1, 1000, 65535, 65536, 200000} {
		body := bytes.Repeat([]byte("stored "), size/7+1)[:size]
		out := storedGzip([]byte("prefix"), body)
		if !bytes.HasPrefix(out, []byte("prefix")) {
			t.Fatalf("size %d: dst prefix overwritten", size)
		}
		if got := gunzip(t, out[len("prefix"):]); !bytes.Equal(got, body) {
			t.Fatalf("size %d: round trip returned %d different bytes", size, len(got))
		}
	}
}
