package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
	"time"
)

const testSecret = "million-users-challenge"

func b64url(t *testing.T, b []byte) string {
	t.Helper()
	return base64.RawURLEncoding.EncodeToString(b)
}

// signToken builds an HS256 JWT exactly like the test suite's openssl helper.
func signToken(t *testing.T, payload string) string {
	t.Helper()
	return signTokenRaw(t, `{"alg":"HS256","typ":"JWT"}`, payload)
}

func signTokenRaw(t *testing.T, header, payload string) string {
	t.Helper()
	h := b64url(t, []byte(header))
	p := b64url(t, []byte(payload))
	mac := hmac.New(sha256.New, []byte(testSecret))
	mac.Write([]byte(h + "." + p))
	return h + "." + p + "." + b64url(t, mac.Sum(nil))
}

// TestRequireAuth walks the auth matrix from test/test.sh: each row is a
// request the suite makes and the exact body it expects back.
func TestRequireAuth(t *testing.T) {
	nowS := time.Now().Unix()
	now := strconv.FormatInt(nowS, 10)
	hourLater := strconv.FormatInt(nowS+3600, 10)
	hourAgo := strconv.FormatInt(nowS-3600, 10)
	twoHoursAgo := strconv.FormatInt(nowS-7200, 10)

	valid := signToken(t, `{"sub":"1","username":"golden_ember_1","iat":`+now+`,"exp":`+hourLater+`}`)
	expired := signToken(t, `{"sub":"1","username":"golden_ember_1","iat":`+twoHoursAgo+`,"exp":`+hourAgo+`}`)
	badSub := signToken(t, `{"sub":"abc","username":"golden_ember_1","iat":`+now+`,"exp":`+hourLater+`}`)
	noName := signToken(t, `{"sub":"1","iat":`+now+`,"exp":`+hourLater+`}`)
	noneAlg := signTokenRaw(t, `{"alg":"none","typ":"JWT"}`, `{"sub":"1","username":"golden_ember_1","exp":`+hourLater+`}`)
	badSignature := valid[:strings.LastIndexByte(valid, '.')] +
		".c2lnbmF0dXJlLWlzLW5vdC12YWxpZC1hdC1hbGwtYXQtYWxsLTEyMzQ"

	cases := []struct {
		name       string
		auth       string // "" means no Authorization header
		wantStatus int
		wantBody   string
	}{
		{"no header", "", 401, `{"error":"missing bearer token"}`},
		{"not bearer", "Basic abc", 401, `{"error":"missing bearer token"}`},
		{"garbage token", "Bearer abc", 401, `{"error":"invalid or expired token"}`},
		{"bad signature", "Bearer " + badSignature, 401, `{"error":"invalid or expired token"}`},
		{"alg none", "Bearer " + noneAlg, 401, `{"error":"invalid or expired token"}`},
		{"expired", "Bearer " + expired, 401, `{"error":"invalid or expired token"}`},
		{"sub not integer", "Bearer " + badSub, 401, `{"error":"invalid token payload"}`},
		{"no username", "Bearer " + noName, 401, `{"error":"invalid token payload"}`},
		{"valid", "Bearer " + valid, 200, "ok"},
	}

	a := &app{cfg: Config{JWTSecret: []byte(testSecret)}}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var got user
			probe := a.requireAuth(func(w http.ResponseWriter, r *http.Request) {
				got = requestUser(r)
				w.WriteHeader(http.StatusOK)
				w.Write([]byte("ok"))
			})
			r := httptest.NewRequest(http.MethodPost, "/posts", nil)
			if tc.auth != "" {
				r.Header.Set("Authorization", tc.auth)
			}
			w := httptest.NewRecorder()
			probe(w, r)
			if w.Code != tc.wantStatus || w.Body.String() != tc.wantBody {
				t.Fatalf("got %d %q, want %d %q", w.Code, w.Body.String(), tc.wantStatus, tc.wantBody)
			}
			if tc.wantStatus == 200 && (got.id != 1 || got.username != "golden_ember_1") {
				t.Fatalf("probe user = %+v", got)
			}
		})
	}
}
