package main

import (
	"strings"
	"testing"
)

func parse(t *testing.T, req string) (parsed, int, bool) {
	t.Helper()
	sc := &parseScratch{}
	return parseRequest([]byte(req), sc)
}

func authed(t *testing.T) string {
	t.Helper()
	jwt := buildTestToken("1", "golden_ember_1")
	return "Authorization: Bearer " + jwt + "\r\n"
}

func TestParseGetMatrix(t *testing.T) {
	cases := []struct {
		name       string
		req        string
		kind       reqKind
		status     int
		id         int64
		wantsGzip  bool
		closeAfter bool
	}{
		{"health", "GET /health HTTP/1.1\r\nHost: x\r\n\r\n", rHealth, 0, 0, false, false},
		{"feed gzip", "GET /feed HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n", rFeed, 0, 0, true, false},
		{"post id", "GET /posts/42 HTTP/1.1\r\nHost: x\r\n\r\n", rGetPost, 0, 42, false, false},
		{"post id 1.5", "GET /posts/1.5 HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 400, 0, false, false},
		{"post id zero", "GET /posts/0 HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 400, 0, false, false},
		{"post id neg", "GET /posts/-1 HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 400, 0, false, false},
		{"post id abc", "GET /posts/abc HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 400, 0, false, false},
		{"missing post", "GET /posts/999999999 HTTP/1.1\r\nHost: x\r\n\r\n", rGetPost, 0, 999999999, false, false},
		{"unknown", "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 404, 0, false, false},
		{"root", "GET / HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 404, 0, false, false},
		{"posts slash", "GET /posts/ HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 404, 0, false, false},
		{"query string", "GET /feed?page=2 HTTP/1.1\r\nHost: x\r\n\r\n", rImmediate, 404, 0, false, false},
		{"connection close", "GET /feed HTTP/1.1\r\nConnection: close\r\n\r\n", rFeed, 0, 0, false, true},
		{"http/1.0 closes", "GET /feed HTTP/1.0\r\n\r\n", rFeed, 0, 0, false, true},
		{"http/1.0 keep-alive", "GET /feed HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", rFeed, 0, 0, false, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			p, n, fatal := parse(t, tc.req)
			if fatal {
				t.Fatalf("unexpected fatal")
			}
			if n != len(tc.req) {
				t.Fatalf("consumed %d of %d", n, len(tc.req))
			}
			if p.kind != tc.kind || p.status != int16(tc.status) || p.id != tc.id ||
				p.wantsGzip != tc.wantsGzip || p.closeAfter != tc.closeAfter {
				t.Fatalf("got %+v, want kind=%d status=%d id=%d gzip=%v close=%v",
					p, tc.kind, tc.status, tc.id, tc.wantsGzip, tc.closeAfter)
			}
		})
	}
}

func TestParsePostMatrix(t *testing.T) {
	jwt := buildTestToken("1", "golden_ember_1")
	jwt7 := buildTestToken("7", "swift_ember_22228")
	a := "Authorization: Bearer " + jwt + "\r\n"

	t.Run("create ok", func(t *testing.T) {
		req := "POST /posts HTTP/1.1\r\n" + a + "Content-Type: application/json\r\nContent-Length: 17\r\n\r\n{\"body\":\"  hi  \"}\r\nEXTRA"
		p, n, fatal := parse(t, req)
		if fatal || p.kind != rCreate || n != len(req)-len("\r\nEXTRA") {
			t.Fatalf("p=%+v n=%d fatal=%v", p, n, fatal)
		}
		if string(p.body) != "hi" || p.uid != 1 || string(p.username) != "golden_ember_1" {
			t.Fatalf("body=%q uid=%d name=%q", p.body, p.uid, p.username)
		}
	})

	t.Run("like auth before id", func(t *testing.T) {
		p, _, fatal := parse(t, "POST /posts/abc/like HTTP/1.1\r\n\r\n")
		if fatal || p.status != 401 || p.reason != "missing bearer token" {
			t.Fatalf("p=%+v fatal=%v", p, fatal)
		}
	})

	t.Run("like with token bad id", func(t *testing.T) {
		p, _, fatal := parse(t, "POST /posts/abc/like HTTP/1.1\r\nAuthorization: Bearer "+jwt7+"\r\n\r\n")
		if fatal || p.status != 400 || p.reason != "invalid post id" {
			t.Fatalf("p=%+v fatal=%v", p, fatal)
		}
	})

	t.Run("like ok", func(t *testing.T) {
		p, n, fatal := parse(t, "POST /posts/123/like HTTP/1.1\r\nAuthorization: Bearer "+jwt7+"\r\n\r\n")
		if fatal || p.kind != rLike || p.id != 123 || n != len("POST /posts/123/like HTTP/1.1\r\nAuthorization: Bearer "+jwt7+"\r\n\r\n") {
			t.Fatalf("p=%+v n=%d fatal=%v", p, n, fatal)
		}
		if p.uid != 7 || string(p.username) != "swift_ember_22228" {
			t.Fatalf("uid=%d name=%q", p.uid, p.username)
		}
	})

	t.Run("like missing post id is a job not an error", func(t *testing.T) {
		p, _, fatal := parse(t, "POST /posts/999999999/like HTTP/1.1\r\nAuthorization: Bearer "+jwt7+"\r\n\r\n")
		if fatal || p.kind != rLike || p.id != 999999999 {
			t.Fatalf("p=%+v fatal=%v", p, fatal)
		}
	})

	t.Run("malformed json", func(t *testing.T) {
		p, _, fatal := parse(t, "POST /posts HTTP/1.1\r\n"+a+"Content-Length: 10\r\n\r\n{bad json!")
		if fatal || p.status != 400 || p.reason != "malformed JSON body" {
			t.Fatalf("p=%+v fatal=%v", p, fatal)
		}
	})

	t.Run("body required", func(t *testing.T) {
		p, _, _ := parse(t, "POST /posts HTTP/1.1\r\n"+a+"Content-Length: 12\r\n\r\n{\"body\":123}")
		if p.status != 400 || p.reason != "body is required" {
			t.Fatalf("p=%+v", p)
		}
	})

	t.Run("create without token", func(t *testing.T) {
		p, _, fatal := parse(t, "POST /posts HTTP/1.1\r\nContent-Length: 12\r\n\r\n{\"body\":\"x\"}")
		if fatal || p.status != 401 || p.reason != "missing bearer token" {
			t.Fatalf("p=%+v fatal=%v", p, fatal)
		}
	})

	t.Run("post to unknown path with body", func(t *testing.T) {
		p, n, fatal := parse(t, "POST /nope HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello")
		if fatal || p.status != 404 || p.closeAfter || n != len("POST /nope HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello") {
			t.Fatalf("p=%+v n=%d fatal=%v", p, n, fatal)
		}
	})

	t.Run("chunked refused", func(t *testing.T) {
		p, _, fatal := parse(t, "POST /posts HTTP/1.1\r\n"+a+"Transfer-Encoding: chunked\r\n\r\n")
		if !fatal || p.status != 400 {
			t.Fatalf("p=%+v fatal=%v", p, fatal)
		}
	})
}

func TestParseFraming(t *testing.T) {
	sc := &parseScratch{}
	full := "GET /feed HTTP/1.1\r\nHost: x\r\n\r\nGET /health HTTP/1.1\r\nHost: x\r\n\r\n"

	t.Run("pipelined two requests", func(t *testing.T) {
		p, n, fatal := parseRequest([]byte(full), sc)
		if fatal || p.kind != rFeed || n != strings.Index(full, "GET /health") {
			t.Fatalf("p=%+v n=%d fatal=%v", p, n, fatal)
		}
		p2, n2, fatal := parseRequest([]byte(full[n:]), sc)
		if fatal || p2.kind != rHealth || n2 != len(full)-n {
			t.Fatalf("p=%+v n=%d fatal=%v", p2, n2, fatal)
		}
	})

	t.Run("incomplete needs more", func(t *testing.T) {
		p, n, fatal := parse(t, "GET /feed HTTP/1.1\r\nHost: x\r")
		if fatal || n != 0 {
			t.Fatalf("p=%+v n=%d fatal=%v", p, n, fatal)
		}
	})

	t.Run("incomplete body", func(t *testing.T) {
		p, n, fatal := parse(t, "POST /posts HTTP/1.1\r\nContent-Length: 20\r\n\r\n{\"body\":\"hi\"}")
		if fatal || n != 0 {
			t.Fatalf("p=%+v n=%d fatal=%v", p, n, fatal)
		}
	})

	t.Run("garbage line", func(t *testing.T) {
		_, _, fatal := parse(t, "GARBAGE\r\n\r\n")
		if !fatal {
			t.Fatal("expected fatal")
		}
	})

	t.Run("http/2 preface refused", func(t *testing.T) {
		_, _, fatal := parse(t, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")
		if !fatal {
			t.Fatal("expected fatal")
		}
	})
}
