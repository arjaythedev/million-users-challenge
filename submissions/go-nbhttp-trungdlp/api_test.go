package main

import (
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
)

func tokenFor(t *testing.T, claims jwt.MapClaims, method jwt.SigningMethod, secret []byte) string {
	t.Helper()
	token, err := jwt.NewWithClaims(method, claims).SignedString(secret)
	if err != nil {
		t.Fatal(err)
	}
	return "Bearer " + token
}

func TestAuthenticate(t *testing.T) {
	a := newApp([]byte("test-secret"))
	validClaims := func() jwt.MapClaims {
		return jwt.MapClaims{"sub": "1", "username": "alice", "exp": time.Now().Add(time.Hour).Unix()}
	}
	for _, tc := range []struct {
		name   string
		change func(jwt.MapClaims)
		method jwt.SigningMethod
		secret []byte
		want   string
	}{
		{"valid", func(jwt.MapClaims) {}, jwt.SigningMethodHS256, a.jwtSecret, ""},
		{"expired", func(c jwt.MapClaims) { c["exp"] = time.Now().Add(-time.Hour).Unix() }, jwt.SigningMethodHS256, a.jwtSecret, "invalid or expired token"},
		{"missing expiry", func(c jwt.MapClaims) { delete(c, "exp") }, jwt.SigningMethodHS256, a.jwtSecret, "invalid or expired token"},
		{"bad signature", func(jwt.MapClaims) {}, jwt.SigningMethodHS256, []byte("wrong"), "invalid or expired token"},
		{"wrong algorithm", func(jwt.MapClaims) {}, jwt.SigningMethodHS384, a.jwtSecret, "invalid or expired token"},
		{"numeric sub", func(c jwt.MapClaims) { c["sub"] = 1 }, jwt.SigningMethodHS256, a.jwtSecret, "invalid token payload"},
		{"zero sub", func(c jwt.MapClaims) { c["sub"] = "0" }, jwt.SigningMethodHS256, a.jwtSecret, "invalid token payload"},
		{"missing username", func(c jwt.MapClaims) { delete(c, "username") }, jwt.SigningMethodHS256, a.jwtSecret, "invalid token payload"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			claims := validClaims()
			tc.change(claims)
			uid, name, status, body := a.authenticate([]byte(tokenFor(t, claims, tc.method, tc.secret)))
			if tc.want == "" {
				if status != 0 || uid != 1 || name != "alice" {
					t.Fatalf("uid=%d name=%q status=%d", uid, name, status)
				}
			} else if status != 401 || string(body) != "{\"error\":\""+tc.want+"\"}" {
				t.Fatalf("status=%d, body=%s", status, body)
			}
		})
	}
}

func TestVerifyJWTBoundaries(t *testing.T) {
	secret := []byte("test-secret")
	valid := func(claims jwt.MapClaims) string {
		return strings.TrimPrefix(tokenFor(t, claims, jwt.SigningMethodHS256, secret), "Bearer ")
	}
	validExp := time.Now().Add(time.Hour).Unix()
	for _, tc := range []struct {
		name       string
		token      string
		tokenOK    bool
		payloadOK  bool
		expectSub  string
		expectName string
	}{
		{
			name:       "empty username is still a string",
			token:      valid(jwt.MapClaims{"sub": "1", "username": "", "exp": validExp}),
			tokenOK:    true,
			payloadOK:  true,
			expectSub:  "1",
			expectName: "",
		},
		{
			name:      "numeric sub is a payload error after signature verification",
			token:     valid(jwt.MapClaims{"sub": 1, "username": "alice", "exp": validExp}),
			tokenOK:   true,
			payloadOK: false,
		},
		{
			name:      "string exp is a token error",
			token:     valid(jwt.MapClaims{"sub": "1", "username": "alice", "exp": "later"}),
			tokenOK:   false,
			payloadOK: false,
		},
		{
			name:      "oversized signature is malformed",
			token:     valid(jwt.MapClaims{"sub": "1", "username": "alice", "exp": validExp}) + strings.Repeat("A", 64),
			tokenOK:   false,
			payloadOK: false,
		},
		{
			name:      "extra segment is malformed",
			token:     valid(jwt.MapClaims{"sub": "1", "username": "alice", "exp": validExp}) + ".extra",
			tokenOK:   false,
			payloadOK: false,
		},
	} {
		t.Run(tc.name, func(t *testing.T) {
			sub, name, tokenOK, payloadOK := verifyJWT(tc.token, secret, &sync.Pool{})
			if tokenOK != tc.tokenOK || payloadOK != tc.payloadOK || sub != tc.expectSub || name != tc.expectName {
				t.Fatalf("got sub=%q name=%q tokenOK=%v payloadOK=%v", sub, name, tokenOK, payloadOK)
			}
		})
	}
}

func TestInputValidation(t *testing.T) {
	a := newApp([]byte("test-secret"))
	auth := tokenFor(t, jwt.MapClaims{"sub": "1", "username": "alice", "exp": time.Now().Add(time.Hour).Unix()}, jwt.SigningMethodHS256, a.jwtSecret)
	for _, tc := range []struct {
		method, path, auth, body string
		status                   int
		message                  string
	}{
		{"GET", "/nope", "", "", 404, "not found"},
		{"GET", "/posts/0", "", "", 400, "invalid post id"},
		{"GET", "/posts/-1", "", "", 400, "invalid post id"},
		{"GET", "/posts/1.5", "", "", 400, "invalid post id"},
		{"POST", "/posts/abc/like", "", "", 401, "missing bearer token"},
		{"POST", "/posts/abc/like", auth, "", 400, "invalid post id"},
		{"POST", "/posts", "Basic abc", "{}", 401, "missing bearer token"},
		{"POST", "/posts", "Bearer abc", "{}", 401, "invalid or expired token"},
		{"POST", "/posts", auth, "", 400, "body is required"},
		{"POST", "/posts", auth, "null", 400, "body is required"},
		{"POST", "/posts", auth, "[]", 400, "body is required"},
		{"POST", "/posts", auth, "{}", 400, "body is required"},
		{"POST", "/posts", auth, `{"body":123}`, 400, "body is required"},
		{"POST", "/posts", auth, `{"body":" \t\n "}`, 400, "body is required"},
		{"POST", "/posts", auth, `{bad`, 400, "malformed JSON body"},
		{"POST", "/posts", auth, `{"body":"hello"} {}`, 400, "malformed JSON body"},
		{"POST", "/posts", auth, `{"body":"` + strings.Repeat("a", 501) + `"}`, 400, "body must be at most 500 characters"},
		{"POST", "/posts", auth, `{"body":"` + strings.Repeat("😀", 251) + `"}`, 400, "body must be at most 500 characters"},
	} {
		t.Run(tc.path+"/"+tc.body[:min(len(tc.body), 20)], func(t *testing.T) {
			r := request{method: []byte(tc.method), path: []byte(tc.path), auth: []byte(tc.auth), body: []byte(tc.body)}
			var j job
			needsDB, status, body := a.route(&r, &j)
			if needsDB || status != tc.status || string(body) != "{\"error\":\""+tc.message+"\"}" {
				t.Fatalf("needsDB=%v status=%d, body=%s", needsDB, status, body)
			}
		})
	}
}

func TestFormatTime(t *testing.T) {
	value := time.Date(2026, 1, 1, 6, 59, 59, 999999999, time.FixedZone("UTC+7", 7*3600))
	if got := formatTime(value); got != "2025-12-31T23:59:59.999Z" {
		t.Fatal(got)
	}
}
