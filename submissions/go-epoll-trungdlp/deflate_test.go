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

func TestBuildLengths(t *testing.T) {
	// Fibonacci frequencies make an unlimited Huffman code far deeper than 15 bits.
	fib := make([]uint32, 286)
	a, b := uint32(1), uint32(1)
	for i := range 30 {
		fib[i] = a
		a, b = b, a+b
	}
	cases := []struct {
		freq []uint32
		max  int
	}{
		{fib, 15}, {fib[:19], 7}, {make([]uint32, 30), 15}, {[]uint32{0, 0, 5, 0}, 7},
		{[]uint32{9, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, 7},
	}
	for i, c := range cases {
		lens := make([]uint8, len(c.freq))
		buildLengths(c.freq, lens, c.max, nil, new([286]int))
		kraft, used := 0, 0
		for s, l := range lens {
			if int(l) > c.max || (c.freq[s] > 0 && l == 0) {
				t.Fatalf("case %d: symbol %d (freq %d) has length %d", i, s, c.freq[s], l)
			}
			if l > 0 {
				kraft += 1 << (c.max - int(l))
				used++
			}
		}
		if kraft != 1<<c.max || used < 2 {
			t.Fatalf("case %d: incomplete code (kraft %d of %d, %d codes)", i, kraft, 1<<c.max, used)
		}
	}
}

func TestGzipSmallerThanFixed(t *testing.T) {
	feed, err := os.ReadFile("testdata/feed-mload.json")
	if err != nil {
		t.Fatal(err)
	}
	var z deflater
	out := z.appendGzip(nil, feed)
	if !bytes.Equal(gunzip(t, out), feed) {
		t.Fatal("round trip mismatch")
	}
	// The fixed-table encoder made 902 bytes of this feed.
	if len(out) > 820 {
		t.Fatalf("feed compressed to %d bytes", len(out))
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
	for _, name := range []string{"feed.json", "feed-mload.json"} {
		b.Run(name, func(b *testing.B) {
			feed, err := os.ReadFile("testdata/" + name)
			if err != nil {
				b.Skip(err)
			}
			var z deflater
			var out []byte
			for b.Loop() {
				out = z.appendGzip(out[:0], feed)
			}
			b.ReportMetric(float64(len(out)), "bytes")
		})
	}
}
