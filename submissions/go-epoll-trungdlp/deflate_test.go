package main

import (
	"bytes"
	"compress/gzip"
	"io"
	"math/rand/v2"
	"os"
	"strings"
	"testing"
	"time"
)

func gunzip(t testing.TB, b []byte) []byte {
	t.Helper()
	r, err := gzip.NewReader(bytes.NewReader(b))
	if err != nil {
		t.Fatal(err)
	}
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatal(err)
	}
	return out
}

func TestGzipRoundTrip(t *testing.T) {
	rng := rand.New(rand.NewPCG(1, 2))
	inputs := [][]byte{
		nil, []byte("a"), []byte("abcd"), bytes.Repeat([]byte("a"), 70000),
		[]byte(strings.Repeat(`{"id":1,"body":"hello world","author":"x"},`, 300)),
	}
	for range 200 {
		n := rng.IntN(20000)
		b := make([]byte, n)
		alphabet := 2 + rng.IntN(255)
		for i := range b {
			b[i] = byte(rng.IntN(alphabet))
		}
		inputs = append(inputs, b)
	}
	var z deflater
	for i, in := range inputs {
		out := z.appendGzip([]byte("prefix"), in)
		if !bytes.HasPrefix(out, []byte("prefix")) {
			t.Fatal("prefix lost")
		}
		if got := gunzip(t, out[len("prefix"):]); !bytes.Equal(got, in) {
			t.Fatalf("input %d (%d bytes): round trip mismatch", i, len(in))
		}
	}
	// The hash table is reused across calls; a stale entry must never become a match.
	z.base = 1<<32 - 1000
	in := []byte(strings.Repeat("stale entries ", 400))
	if got := gunzip(t, z.appendGzip(nil, in)); !bytes.Equal(got, in) {
		t.Fatal("base wrap-around")
	}
}

func TestDistSym(t *testing.T) {
	for d := uint32(1); d <= lzWindow; d++ {
		s := distSym(d)
		if d < distBase[s] || (s < 29 && d >= distBase[s+1]) || d-distBase[s] >= 1<<distExtra[s] {
			t.Fatalf("distance %d -> symbol %d", d, s)
		}
	}
}

func TestAppendPgTime(t *testing.T) {
	rng := rand.New(rand.NewPCG(3, 4))
	for range 100000 {
		us := rng.Int64N(400*365*86400*1_000_000) - 200*365*86400*1_000_000
		want := time.UnixMicro(us + pgEpochUnixMicro).UTC().Format(createdAtLayout)
		if got := string(appendPgTime(nil, us)); got != want {
			t.Fatalf("%d: got %s, want %s", us, got, want)
		}
	}
}

func BenchmarkGzipFeed(b *testing.B) {
	feed, err := os.ReadFile("testdata/feed.json")
	if err != nil {
		b.Skip("no testdata/feed.json")
	}
	var z deflater
	var out []byte
	for b.Loop() {
		out = z.appendGzip(out[:0], feed)
	}
	b.ReportMetric(float64(len(out)), "bytes")
}
