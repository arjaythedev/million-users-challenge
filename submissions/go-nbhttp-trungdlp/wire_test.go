package main

import (
	"encoding/binary"
	"strings"
	"testing"
)

func TestParseRequest(t *testing.T) {
	raw := "POST /posts?x=1 HTTP/1.1\r\nHost: a\r\nauthorization: Bearer t\r\nAccept-Encoding: br, gzip\r\nContent-Length: 4\r\n\r\nbodyGET /feed HTTP/1.1\r\nConnection: close\r\n\r\n"
	var r request
	n, res := parseRequest([]byte(raw), &r)
	if res != parseOK || string(r.method) != "POST" || string(r.path) != "/posts" || string(r.auth) != "Bearer t" ||
		string(r.body) != "body" || !r.gzip || r.close {
		t.Fatalf("res=%d %+v", res, r)
	}
	n2, res := parseRequest([]byte(raw[n:]), &r)
	if res != parseOK || n+n2 != len(raw) || string(r.path) != "/feed" || !r.close || r.gzip {
		t.Fatalf("second: res=%d %+v", res, r)
	}
	for _, tc := range []struct {
		raw  string
		want int
	}{
		{"GET /feed HTTP/1.1\r\nHost: a\r\n", parseIncomplete},
		{"POST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\nshort", parseIncomplete},
		{"POST /posts HTTP/1.1\r\nContent-Length: 99999\r\n\r\n", parseTooLarge},
		{"GARBAGE\r\n\r\n", parseBad},
		{"GET /feed HTTP/2.0\r\n\r\n", parseBad},
		{"GET /feed HTTP/1.1\r\nContent-Length: x\r\n\r\n", parseBad},
		{"GET /feed HTTP/1.0\r\n\r\n", parseOK},
	} {
		if _, res := parseRequest([]byte(tc.raw), &r); res != tc.want {
			t.Errorf("%q: got %d, want %d", tc.raw, res, tc.want)
		}
	}
	if _, res := parseRequest([]byte("GET /feed HTTP/1.0\r\n\r\n"), &r); res != parseOK || !r.close {
		t.Error("HTTP/1.0 without keep-alive must close")
	}
}

func TestAppendHead(t *testing.T) {
	got := string(appendHead(nil, 201, 12, true, true))
	want := "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nVary: Accept-Encoding\r\nConnection: close\r\nContent-Length: 12\r\n\r\n"
	if got != want {
		t.Fatalf("%q", got)
	}
}

func TestBindExecute(t *testing.T) {
	b := appendBindExecute(nil, stmtLike, int8Param(7), int8Param(9))
	typ, payload, n := nextMessage(b)
	if typ != 'B' || !strings.HasPrefix(string(payload), "\x00s4\x00") {
		t.Fatalf("bind: %c %q", typ, payload)
	}
	if typ, _, m := nextMessage(b[n:]); typ != 'E' || n+m != len(b) {
		t.Fatalf("execute: %c", typ)
	}
}

func TestBackendMessages(t *testing.T) {
	row := []byte{0, 2}
	row = binary.BigEndian.AppendUint32(row, 3)
	row = append(row, "abc"...)
	row = binary.BigEndian.AppendUint32(row, 0xffffffff) // NULL
	cols, ok := rowColumns(row, nil)
	if !ok || len(cols) != 2 || string(cols[0]) != "abc" || cols[1] != nil {
		t.Fatalf("%q %v", cols, ok)
	}
	if _, ok := rowColumns(row[:5], nil); ok {
		t.Fatal("truncated row accepted")
	}
	if rowsAffected([]byte("INSERT 0 1\x00")) != 1 || rowsAffected([]byte("INSERT 0 0\x00")) != 0 {
		t.Fatal("rowsAffected")
	}
	code, msg := errorFields([]byte("SERROR\x00C23503\x00Mfk violation\x00\x00"))
	if code != "23503" || msg != "fk violation" {
		t.Fatalf("%q %q", code, msg)
	}
	if _, _, n := nextMessage([]byte{'Z', 0, 0, 0, 5}); n != 0 {
		t.Fatal("incomplete message accepted")
	}
}

func TestJSON(t *testing.T) {
	got := string(appendPost(nil, 5, []byte("a\"b\\c\n\x01é"), 0, []byte("bob"), 3))
	want := `{"id":5,"body":"a\"b\\c\n\u0001é","created_at":"2000-01-01T00:00:00.000Z","author":"bob","like_count":3}`
	if got != want {
		t.Fatalf("\n got %s\nwant %s", got, want)
	}
	// 1 µs before a whole millisecond truncates down.
	if got := string(appendPgTime(nil, 1999)); got != "2000-01-01T00:00:00.001Z" {
		t.Fatal(got)
	}
	if got := string(appendPgTime(nil, -1)); got != "1999-12-31T23:59:59.999Z" {
		t.Fatal(got)
	}
}

func TestRoutePaths(t *testing.T) {
	a := newApp([]byte("s"))
	for _, tc := range []struct {
		method, path string
		db           bool
		stmt         uint8
		status       int
	}{
		{"GET", "/health", true, stmtHealth, 0},
		{"GET", "/feed", true, stmtFeed, 0},
		{"GET", "/posts/42", true, stmtPost, 0},
		{"GET", "/posts/", false, 0, 404},
		{"GET", "/posts/1/2", false, 0, 404},
		{"POST", "/posts/like", false, 0, 404},
		{"POST", "/posts/x/like", false, 0, 401},
		{"POST", "/posts//like", false, 0, 404},
		{"GET", "/feed/", false, 0, 404},
	} {
		var j job
		db, status, _ := a.route(&request{method: []byte(tc.method), path: []byte(tc.path)}, &j)
		if db != tc.db || status != tc.status || (db && j.stmt != tc.stmt) {
			t.Errorf("%s %s: db=%v status=%d stmt=%d", tc.method, tc.path, db, status, j.stmt)
		}
	}
}
