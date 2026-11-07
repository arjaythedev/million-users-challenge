package main

import (
	"bytes"
	"compress/gzip"
	"fmt"
	"io"
	"math/rand/v2"
	"strings"
	"testing"
)

func gunzip(t testing.TB, data []byte) []byte {
	t.Helper()
	r, err := gzip.NewReader(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("gzip header: %v", err)
	}
	r.Multistream(false)
	out, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("gzip body: %v", err)
	}
	if rest, _ := io.ReadAll(r); len(rest) != 0 {
		t.Fatalf("trailing data after the gzip member")
	}
	return out
}

func checkRoundTrip(t testing.TB, body []byte) []byte {
	t.Helper()
	// Keep a prefix in dst to check that the member is appended, not written
	// over what is already there.
	out := gzipAppend([]byte("prefix"), body)
	if !bytes.HasPrefix(out, []byte("prefix")) {
		t.Fatalf("dst prefix overwritten")
	}
	if got := gunzip(t, out[len("prefix"):]); !bytes.Equal(got, body) {
		t.Fatalf("round trip of %d bytes returned %d different bytes", len(body), len(got))
	}
	return out[len("prefix"):]
}

func feedLike(posts int) []byte {
	b := []byte(`{"posts":[`)
	for i := 0; i < posts; i++ {
		if i > 0 {
			b = append(b, ',')
		}
		text := strings.Repeat(fmt.Sprintf("word%d ", i%7), 3+i%11)
		b = appendPost(b, int64(500000-i), text, 1767225600000000+int64(i)*1000, fmt.Sprintf("user%d", 1000+i), int64(i%5))
	}
	return append(b, ']', '}')
}

func TestGzipRoundTrip(t *testing.T) {
	random := rand.New(rand.NewPCG(1, 2))
	inputs := [][]byte{
		{},
		[]byte("a"),
		[]byte("abcd"),
		[]byte("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
		bytes.Repeat([]byte("x"), 100000),         // matches at distance 1, longest length
		bytes.Repeat([]byte("0123456789"), 10000), // more than one window of repeats
		feedLike(20),
		feedLike(200),
	}
	all := make([]byte, 256*4)
	for i := range all {
		all[i] = byte(i)
	}
	inputs = append(inputs, all)
	for _, size := range []int{1, 7, 64, 1000, 4096, 40000, 70000} {
		noise := make([]byte, size)
		for i := range noise {
			noise[i] = byte(random.UintN(256))
		}
		inputs = append(inputs, noise)
		// Few distinct bytes: many short matches at varied distances.
		sparse := make([]byte, size)
		for i := range sparse {
			sparse[i] = "ab"[random.UintN(2)]
		}
		inputs = append(inputs, sparse)
	}
	// A repeat just inside and just outside the 32 KiB window.
	for _, gap := range []int{windowSize - 4, windowSize - 1, windowSize, windowSize + 1} {
		b := make([]byte, gap+64)
		for i := range b {
			b[i] = byte(random.UintN(256))
		}
		copy(b[gap:], b[:64])
		inputs = append(inputs, b)
	}
	for _, body := range inputs {
		checkRoundTrip(t, body)
	}
	// Encoders are pooled: a reused encoder must not match into earlier input.
	for i := 0; i < 50; i++ {
		checkRoundTrip(t, feedLike(1+i%20))
	}
}

// Every match length and every distance code must decode.
func TestGzipAllLengthsAndDistances(t *testing.T) {
	random := rand.New(rand.NewPCG(3, 4))
	for _, distance := range []int{1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 16, 17, 24, 25, 100, 1000, 5000, 20000, 32767, 32768} {
		for length := minMatch; length <= maxMatch+2; length += 1 + length/32 {
			b := make([]byte, distance+length+8)
			for i := 0; i < distance; i++ {
				b[i] = byte(random.UintN(256))
			}
			for i := distance; i < distance+length; i++ {
				b[i] = b[i-distance]
			}
			for i := distance + length; i < len(b); i++ {
				b[i] = byte(random.UintN(256))
			}
			checkRoundTrip(t, b)
		}
	}
}

func TestGzipCompressesFeed(t *testing.T) {
	body := feedLike(20)
	out := checkRoundTrip(t, body)
	if len(out)*2 > len(body) {
		t.Fatalf("feed of %d bytes compressed to %d", len(body), len(out))
	}
	t.Logf("feed %d -> %d bytes", len(body), len(out))
}

func FuzzGzipAppend(f *testing.F) {
	f.Add([]byte("hello hello hello"))
	f.Add(feedLike(3))
	f.Fuzz(func(t *testing.T, body []byte) { checkRoundTrip(t, body) })
}

func BenchmarkGzipFeed(b *testing.B) {
	body := feedLike(20)
	b.SetBytes(int64(len(body)))
	dst := make([]byte, 0, 8192)
	for b.Loop() {
		dst = gzipAppend(dst[:0], body)
	}
	b.ReportMetric(float64(len(dst)), "bytes/op-out")
}
