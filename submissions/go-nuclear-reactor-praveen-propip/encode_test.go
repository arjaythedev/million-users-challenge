package main

import (
	"bytes"
	"compress/gzip"
	"encoding/binary"
	"encoding/json"
	"hash/crc32"
	"io"
	"math/rand/v2"
	"strings"
	"testing"
	"time"
)

func TestEncodePostGolden(t *testing.T) {
	// Byte-exact against the test suite golden shape (ASCII-safe bodies are
	// identical in every encoding flavor).
	created := time.Date(2025, 1, 1, 0, 0, 34, 152_100_000, time.UTC)
	micros := (created.Unix()-946684800)*1_000_000 + int64(created.Nanosecond())/1_000
	b := append([]byte(nil), `{"post":`...)
	b = appendPost(b, 1, []byte("Fast tomorrow small bug big droplet. Weird cache really project rarely everything?"),
		[]byte("happy_nebula_10241"), micros, 11)
	b = append(b, '}')
	want := `{"post":{"id":1,"body":"Fast tomorrow small bug big droplet. Weird cache really project rarely everything?","created_at":"2025-01-01T00:00:34.152Z","author":"happy_nebula_10241","like_count":11}}`
	if !bytes.Equal(b, []byte(want)) {
		t.Fatalf("got  %s\nwant %s", b, want)
	}
}

func TestAppendTimestampTruncates(t *testing.T) {
	cases := []struct {
		t    time.Time
		want string
	}{
		{time.Date(2025, 12, 31, 23, 59, 19, 409_000_000, time.UTC), "2025-12-31T23:59:19.409Z"},
		{time.Date(2025, 12, 31, 23, 59, 19, 409_999_000, time.UTC), "2025-12-31T23:59:19.409Z"},
		{time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC), "2026-01-01T00:00:00.000Z"},
		{time.Date(1970, 1, 1, 0, 0, 1, 500_000_000, time.UTC), "1970-01-01T00:00:01.500Z"},
	}
	for _, tc := range cases {
		micros := (tc.t.Unix()-946684800)*1_000_000 + int64(tc.t.Nanosecond())/1_000
		got := string(appendTimestamp(nil, micros))
		if got != tc.want {
			t.Fatalf("appendTimestamp(%v) = %q, want %q", tc.t, got, tc.want)
		}
	}
}

func TestAppendJSONStringEscapes(t *testing.T) {
	cases := []struct{ in, want string }{
		{`plain`, `"plain"`},
		{`a"b`, `"a\"b"`},
		{`a\b`, `"a\\b"`},
		{"a\nb", `"a\nb"`},
		{"café ✓", `"café ✓"`},
		{`<b>&amp;</b>`, `"<b>&amp;</b>"`},
		{"a\x00b", `"a\u0000b"`},
	}
	for _, tc := range cases {
		got := string(appendJSONString(nil, []byte(tc.in)))
		if got != tc.want {
			t.Fatalf("appendJSONString(%q) = %s, want %s", tc.in, got, tc.want)
		}
	}
}

func TestAppendJSONStringEveryPosition(t *testing.T) {
	// The 8-byte scan must see an escape wherever it falls in a word.
	for n := 1; n < 40; n++ {
		for at := 0; at < n; at++ {
			for _, c := range []byte{'"', '\\', '\n', 0x1f, 0} {
				in := bytes.Repeat([]byte("a"), n)
				in[at] = c
				got := string(appendJSONString(nil, in))
				var back string
				if err := json.Unmarshal([]byte(got), &back); err != nil || back != string(in) {
					t.Fatalf("n=%d at=%d c=%x: %q (%v)", n, at, c, got, err)
				}
			}
		}
	}
}

func TestGzipRoundTrip(t *testing.T) {
	g := newGzEncoder()
	body := []byte(`{"posts":[{"id":1,"body":"` + strings.Repeat("hello world ", 40) + `"}]}`)
	for i := 0; i < 3; i++ { // the encoder is reused
		out := g.compress(body, nil)
		zr, err := gzip.NewReader(bytes.NewReader(out[hdrRoom:]))
		if err != nil {
			t.Fatal(err)
		}
		got, err := io.ReadAll(zr)
		if err != nil || !bytes.Equal(got, body) {
			t.Fatalf("round trip: %v", err)
		}
		if len(out)-hdrRoom >= len(body) {
			t.Fatal("gzip did not shrink the body")
		}
	}
}

func feedJSON(t *testing.T, bodies []string) ([]byte, []rowMark) {
	t.Helper()
	b := append([]byte(nil), `{"posts":[`...)
	var marks []rowMark
	for i, body := range bodies {
		if i > 0 {
			b = append(b, ',')
		}
		var m rowMark
		ts := int64(820_000_000_000_000 + i*9_123_456_789)
		b = appendPostMarked(b, int64(500000-i), []byte(body), []byte("golden_ember_"+strings.Repeat("1", i%5+1)), ts, int64(i*i), &m, 0)
		marks = append(marks, m)
	}
	return append(b, ']', '}'), marks
}

func gunzip(t *testing.T, gz []byte) []byte {
	t.Helper()
	zr, err := gzip.NewReader(bytes.NewReader(gz))
	if err != nil {
		t.Fatal(err)
	}
	got, err := io.ReadAll(zr)
	if err != nil {
		t.Fatal(err)
	}
	return got
}

func TestGzipFeedRoundTrip(t *testing.T) {
	g := newGzEncoder()
	rng := rand.New(rand.NewPCG(1, 2))
	words := []string{"Fast", "tomorrow", "small", "bug", "big", "droplet.", "Weird", "cache", "really", "project", "rarely", "everything?", "\"q\"", "a\\b", "line\nbreak", "caf\u00e9", "\u2713", "\U0001F600", "\x01"}
	for run := 0; run < 2000; run++ {
		bodies := make([]string, rng.IntN(21))
		for i := range bodies {
			var sb strings.Builder
			for w := rng.IntN(30); w >= 0; w-- {
				sb.WriteString(words[rng.IntN(len(words))])
				sb.WriteByte(' ')
			}
			bodies[i] = sb.String()
		}
		body, marks := feedJSON(t, bodies)
		out := g.compress(body, marks)
		if got := gunzip(t, out[hdrRoom:]); !bytes.Equal(got, body) {
			t.Fatalf("run %d: round trip differs\n got %q\nwant %q", run, got, body)
		}
		// Same bytes without the row hints.
		out = g.compress(body, nil)
		if got := gunzip(t, out[hdrRoom:]); !bytes.Equal(got, body) {
			t.Fatalf("run %d: plain round trip differs", run)
		}
	}
}

func TestGzipEveryMatchShape(t *testing.T) {
	g := newGzEncoder()
	for n := 3; n <= 258; n++ {
		for _, dist := range []int{1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 13, 24, 25, 128, 129, 192, 193, 1024, 1025, 4096, 4097, 16384, 16385, 24576, 24577, 32768} {
			plain := make([]byte, dist+n)
			for i := range plain {
				plain[i] = byte('a' + (i*7+i/5)%26)
			}
			copy(plain[dist:], plain[:n]) // overlap repeats forward
			for i := dist; i < dist+n; i++ {
				plain[i] = plain[i-dist]
			}
			if len(g.out) < hdrRoom+len(gzPrefix)+len(plain)*2+64 {
				g.out = make([]byte, 2*(hdrRoom+len(gzPrefix)+len(plain)*2+64))
			}
			copy(g.out[hdrRoom:], gzPrefix)
			g.o, g.acc, g.nb = hdrRoom+len(gzPrefix), gzTail, gzTailN
			g.lits(plain[:dist])
			g.match(n, dist)
			acc := g.acc | uint64(eobEnc.code)<<g.nb
			nb := g.nb + uint(eobEnc.n)
			binary.LittleEndian.PutUint64(g.out[g.o:], acc)
			g.o += int(nb+7) >> 3
			binary.LittleEndian.PutUint32(g.out[g.o:], crc32.ChecksumIEEE(plain))
			binary.LittleEndian.PutUint32(g.out[g.o+4:], uint32(len(plain)))
			if got := gunzip(t, g.out[hdrRoom:g.o+8]); !bytes.Equal(got, plain) {
				t.Fatalf("n=%d dist=%d differs", n, dist)
			}
		}
	}
}

func TestGzipNoAllocs(t *testing.T) {
	g := newGzEncoder()
	body, marks := feedJSON(t, []string{"Fast tomorrow small bug big droplet.", "Weird cache really project rarely everything?", "small bug big cache"})
	if n := testing.AllocsPerRun(100, func() { g.compress(body, marks) }); n != 0 {
		t.Fatalf("compress allocates %v times per run", n)
	}
}

func BenchmarkGzipFeed(b *testing.B) {
	t := &testing.T{}
	words := strings.Fields("Fast tomorrow small bug big droplet. Weird cache really project rarely everything? Quiet river sings under golden moon while tiny robots debug the ancient server.")
	var bodies []string
	for i := 0; i < 20; i++ {
		var sb strings.Builder
		for w := 0; w < 12; w++ {
			sb.WriteString(words[(i*7+w*w*3)%len(words)])
			sb.WriteByte(' ')
		}
		bodies = append(bodies, sb.String())
	}
	body, marks := feedJSON(t, bodies)
	g := newGzEncoder()
	var size int
	b.SetBytes(int64(len(body)))
	for b.Loop() {
		size = len(g.compress(body, marks)) - hdrRoom
	}
	b.ReportMetric(float64(size), "gz-bytes")
	b.ReportMetric(float64(len(body)), "plain-bytes")
}

func TestFrame(t *testing.T) {
	cases := []struct {
		code      int
		gz, close bool
		want      string
	}{
		{404, false, false, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}"},
		{200, true, false, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}"},
		{500, false, true, "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}"},
		{503, false, true, "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}"},
	}
	for _, tc := range cases {
		b := append(make([]byte, hdrRoom), errNotFound...)
		if got := string(frame(b, tc.code, tc.gz, tc.close)); got != tc.want {
			t.Fatalf("got  %q\nwant %q", got, tc.want)
		}
	}
}

func TestImmediateBodies(t *testing.T) {
	cases := []struct {
		p    parsed
		want []byte
	}{
		{parsed{status: 404, reason: "not found"}, errNotFound},
		{parsed{status: 400, reason: "invalid post id"}, errInvalidID},
		{parsed{status: 401, reason: "missing bearer token"}, errMissingBearer},
		{parsed{status: 401, reason: "invalid or expired token"}, errBadToken},
	}
	for _, tc := range cases {
		if got := immediateBody(&tc.p); !bytes.Equal(got, tc.want) {
			t.Fatalf("got %s, want %s", got, tc.want)
		}
	}
}
