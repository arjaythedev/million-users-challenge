package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
	"time"
)

// standardRequest parses raw with net/http and maps it the way the fallback
// path does.
func standardRequest(t *testing.T, raw string) (*request, int) {
	t.Helper()
	reader := bufio.NewReader(strings.NewReader(raw))
	parsed, err := http.ReadRequest(reader)
	if err != nil {
		t.Fatalf("net/http rejected %q: %v", raw, err)
	}
	body, err := io.ReadAll(parsed.Body)
	if err != nil {
		t.Fatalf("net/http body %q: %v", raw, err)
	}
	if len(body) == 0 {
		body = nil
	}
	req := &request{close: parsed.Close, body: body}
	resolveRoute(req, []byte(parsed.Method), []byte(parsed.URL.Path))
	if values, ok := parsed.Header["Authorization"]; ok && len(values) > 0 {
		req.auth, req.hasAuth = values[0], true
	}
	req.gzip = acceptsGzip(parsed.Header.Get("Accept-Encoding"))
	return req, len(raw) - reader.Buffered()
}

func TestFastParserMatchesNetHTTP(t *testing.T) {
	cases := []string{
		"GET /feed HTTP/1.1\r\nHost: a\r\nAccept-Encoding: gzip\r\n\r\n",
		"GET /feed HTTP/1.1\r\nHost: a\r\naccept-encoding: deflate, gzip;q=0\r\n\r\n",
		"GET /feed HTTP/1.1\r\nHost: a\r\nAccept-Encoding: br\r\nAccept-Encoding: gzip\r\n\r\n",
		"GET /health HTTP/1.1\r\nHost: a\r\nConnection: keep-alive, Close\r\n\r\n",
		"GET /posts/42 HTTP/1.1\r\nHost: a\r\n\r\n",
		"GET /posts/0 HTTP/1.1\r\nHost: a\r\n\r\n",
		"GET /posts/abc HTTP/1.1\r\nHost: a\r\n\r\n",
		"GET /posts/ HTTP/1.1\r\nHost: a\r\n\r\n",
		"GET /posts/1/like HTTP/1.1\r\nHost: a\r\n\r\n",
		"GET /posts/99999999999999999999 HTTP/1.1\r\nHost: a\r\n\r\n",
		"HEAD /feed HTTP/1.1\r\nHost: a\r\n\r\n",
		"POST /posts HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer x.y.z\r\nAuthorization: Bearer second\r\nContent-Length: 13\r\n\r\n{\"body\":\"hi\"}",
		"POST /posts/7/like HTTP/1.1\r\nHost: a\r\nauthorization:   Bearer t  \r\nContent-Length: 0\r\n\r\n",
		"POST /posts//like HTTP/1.1\r\nHost: a\r\n\r\n",
		"POST /posts/a/b/like HTTP/1.1\r\nHost: a\r\n\r\n",
		"POST /posts/like HTTP/1.1\r\nHost: a\r\n\r\n",
		"POST /posts/-1/like HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer t\r\n\r\n",
		"GET /feed HTTP/1.1\r\nHost: a\r\n\r\nGET /health HTTP/1.1\r\n\r\n",
	}
	for _, raw := range cases {
		fast, used, status := parseRequest([]byte(raw))
		if status != parseOK {
			t.Fatalf("fast parser status %d for %q", status, raw)
		}
		want, wantUsed := standardRequest(t, raw)
		if fast.body != nil && len(fast.body) == 0 {
			fast.body = nil
		}
		if !(fast.route == want.route && fast.id == want.id && fast.idOK == want.idOK && fast.gzip == want.gzip &&
			fast.close == want.close && fast.hasAuth == want.hasAuth && bytes.Equal(fast.body, want.body)) {
			t.Errorf("%q:\n fast %+v\n want %+v", raw, *fast, *want)
		}
		if (fast.route == routeCreate || fast.route == routeLike) && fast.auth != want.auth {
			t.Errorf("%q: auth %q, want %q", raw, fast.auth, want.auth)
		}
		if used != wantUsed {
			t.Errorf("%q: consumed %d, want %d", raw, used, wantUsed)
		}
	}
}

func TestFastParserDefers(t *testing.T) {
	fallback := []string{
		"GET /feed HTTP/1.0\r\n\r\n",
		"GET /feed?x=1 HTTP/1.1\r\n\r\n",
		"GET /po%73ts/1 HTTP/1.1\r\n\r\n",
		"GET http://a/feed HTTP/1.1\r\n\r\n",
		"POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
		"POST /posts HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n",
		"POST /posts HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}",
		"POST /posts HTTP/1.1\r\nContent-Length: +2\r\n\r\n{}",
		"GET /feed HTTP/1.1\r\nX: a\r\n folded\r\n\r\n",
		"GET /feed HTTP/1.1\r\nBad Name: a\r\n\r\n",
	}
	for _, raw := range fallback {
		if _, _, status := parseRequest([]byte(raw)); status != parseFallback {
			t.Errorf("%q: status %d, want fallback", raw, status)
		}
	}
	incomplete := []string{
		"GET /feed HTTP/1.1\r\nHost: a\r\n",
		"POST /posts HTTP/1.1\r\nContent-Length: 20\r\n\r\n{\"body\"",
	}
	for _, raw := range incomplete {
		if _, _, status := parseRequest([]byte(raw)); status != parseIncomplete {
			t.Errorf("%q: status %d, want incomplete", raw, status)
		}
	}
	if _, _, status := parseRequest([]byte("POST /posts HTTP/1.1\r\nContent-Length: 999999\r\n\r\n")); status != parseBad {
		t.Errorf("oversized body: status %d, want bad", status)
	}
}

func TestAppendStringIsJSON(t *testing.T) {
	inputs := []string{"", "plain", `test: <b>&amp;</b> "quoted" / café ✓`, "tab\tnew\nline\rcr\\slash", "\x00\x01\x1f\x7f", "emoji 🚀 and \u2028"}
	for _, input := range inputs {
		encoded := appendString(nil, input)
		var decoded string
		if err := json.Unmarshal(encoded, &decoded); err != nil || decoded != input {
			t.Errorf("%q encoded as %s: %v (decoded %q)", input, encoded, err, decoded)
		}
	}
}

func TestAppendPostMatchesEncodingJSON(t *testing.T) {
	created := time.Date(2025, 12, 31, 23, 59, 19, 409999999, time.UTC)
	got := appendPost(nil, 500000, `say "hi"`, created.UnixMicro(), "swift_ember_22228", 5)
	want, _ := json.Marshal(struct {
		ID        int64  `json:"id"`
		Body      string `json:"body"`
		CreatedAt string `json:"created_at"`
		Author    string `json:"author"`
		LikeCount int64  `json:"like_count"`
	}{500000, `say "hi"`, created.Format("2006-01-02T15:04:05.000Z"), "swift_ember_22228", 5})
	if string(got) != string(want) {
		t.Fatalf("got  %s\nwant %s", got, want)
	}
	for _, micros := range []int64{0, 1, 999, 1735689634152123, 1767225559409999, -1} {
		at := time.UnixMicro(micros).UTC()
		if got, want := string(appendCreated(nil, micros)), at.Format("2006-01-02T15:04:05.000Z"); got != want {
			t.Errorf("%d: got %s want %s", micros, got, want)
		}
	}
}

func TestFastParserDefersInvalidRequests(t *testing.T) {
	cases := []string{
		"GET /feed\x00 HTTP/1.1\r\nHost: a\r\n\r\n",
		"GET /feed\x7f HTTP/1.1\r\nHost: a\r\n\r\n",
		"GET /feed HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
		"GET /feed HTTP/1.1\r\nHost: a\r\nX-Test: bad\x00value\r\n\r\n",
	}
	for _, raw := range cases {
		if _, err := http.ReadRequest(bufio.NewReader(strings.NewReader(raw))); err == nil {
			t.Fatalf("invalid fixture accepted by net/http: %q", raw)
		}
		if _, _, status := parseRequest([]byte(raw)); status != parseFallback && status != parseBad {
			t.Errorf("malformed request accepted by fast parser: %q", raw)
		}
	}
}

func TestTokenIdentityRequiresUsernameString(t *testing.T) {
	for _, value := range []string{"null", " false ", "123", "{}", "[]"} {
		claims := map[string]json.RawMessage{"sub": json.RawMessage(`"1"`), "username": json.RawMessage(value)}
		if _, _, ok := tokenIdentity(claims); ok {
			t.Errorf("accepted non-string username %s", value)
		}
	}
	claims := map[string]json.RawMessage{"sub": json.RawMessage(`"1"`), "username": json.RawMessage(`""`)}
	if _, username, ok := tokenIdentity(claims); !ok || username != "" {
		t.Fatal("empty string is a valid token username")
	}
}
