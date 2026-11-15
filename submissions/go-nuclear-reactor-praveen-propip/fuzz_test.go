package main

import (
	"bytes"
	"testing"
)

// The parsers and the encoder see bytes from the network, so nothing here may
// panic on any input. Run with: go test -fuzz=FuzzRequest -fuzztime=60s

func FuzzRequest(f *testing.F) {
	jwtSecret = []byte(testSecret)
	tok := signTokenRaw(`{"alg":"HS256","typ":"JWT"}`, `{"sub":"7","username":"a\"b\u00e9","iat":1,"exp":4000000000}`)
	f.Add([]byte("GET /feed HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n"))
	f.Add([]byte("GET /posts/12 HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /nope HTTP/1.1\r\n\r\n"))
	f.Add([]byte("POST /posts/3/like HTTP/1.1\r\nAuthorization: Bearer " + tok + "\r\n\r\n"))
	f.Add([]byte("POST /posts HTTP/1.1\r\nAuthorization: Bearer " + tok + "\r\nContent-Length: 15\r\n\r\n{\"body\":\"\\ud83d\"}"))
	sc := &parseScratch{now: 1_800_000_000}
	f.Fuzz(func(t *testing.T, b []byte) {
		for len(b) > 0 {
			p, n, fatal := parseRequest(b, sc)
			if n < 0 || n > len(b) {
				t.Fatalf("consumed %d of %d", n, len(b))
			}
			if p.kind == rImmediate {
				immediateBody(&p)
			}
			if fatal || n == 0 {
				return
			}
			b = b[n:]
		}
	})
}

func FuzzPostBody(f *testing.F) {
	f.Add([]byte(`{"body":"hi \u00e9 \ud83d\ude00"}`))
	f.Add([]byte(`{"x":[1,{"y":null}],"body":"a\\b\n"}`))
	f.Add([]byte(`  {"body" : "\uD83D"} `))
	var out postBody
	f.Fuzz(func(t *testing.T, b []byte) {
		parsePostBody(b, &out)
	})
}

func FuzzGzipRows(f *testing.F) {
	f.Add([]byte("hello world hello world"), []byte("zesty_nebula_1"))
	f.Add(bytes.Repeat([]byte{0xff, 0, '"', '\\'}, 500), []byte{0x80})
	g := newGzEncoder()
	f.Fuzz(func(t *testing.T, text, name []byte) {
		json := append([]byte(nil), `{"posts":[`...)
		var marks []rowMark
		for i := 0; i < 20; i++ {
			if i > 0 {
				json = append(json, ',')
			}
			var m rowMark
			cut := len(text) * i / 20
			json = appendPostMarked(json, int64(9_000_000_000-i), text[cut:], name, int64(i)*1_234_567_891, int64(i), &m, 0)
			marks = append(marks, m)
		}
		json = append(json, ']', '}')
		if !bytes.Equal(gunzip(t, g.compress(json, marks)[hdrRoom:]), json) {
			t.Fatal("round trip differs")
		}
	})
}
