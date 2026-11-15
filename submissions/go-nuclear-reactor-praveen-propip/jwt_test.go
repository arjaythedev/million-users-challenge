package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"
)

const testSecret = "million-users-challenge"

func b64urlT(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }

func signTokenRaw(header, payload string) string {
	h := b64urlT([]byte(header))
	p := b64urlT([]byte(payload))
	mac := hmac.New(sha256.New, []byte(testSecret))
	mac.Write([]byte(h + "." + p))
	return h + "." + p + "." + b64urlT(mac.Sum(nil))
}

func signToken(t *testing.T, payload string) string {
	t.Helper()
	return signTokenRaw(`{"alg":"HS256","typ":"JWT"}`, payload)
}

// buildTestToken signs a token with a far-future exp (2033).
func buildTestToken(sub, username string) string {
	payload := `{"sub":"` + sub + `","username":"` + username + `","iat":2000000000,"exp":2000003600}`
	return signTokenRaw(`{"alg":"HS256","typ":"JWT"}`, payload)
}

func TestVerifyJWTMatrix(t *testing.T) {
	now := time.Unix(1_800_000_000, 0)
	nowS := strconv.FormatInt(now.Unix(), 10)
	hourLater := strconv.FormatInt(now.Unix()+3600, 10)
	hourAgo := strconv.FormatInt(now.Unix()-3600, 10)
	twoHoursAgo := strconv.FormatInt(now.Unix()-7200, 10)

	valid := signToken(t, `{"sub":"1","username":"golden_ember_1","iat":`+nowS+`,"exp":`+hourLater+`}`)
	expired := signToken(t, `{"sub":"1","username":"golden_ember_1","iat":`+twoHoursAgo+`,"exp":`+hourAgo+`}`)
	badSub := signToken(t, `{"sub":"abc","username":"golden_ember_1","iat":`+nowS+`,"exp":`+hourLater+`}`)
	noName := signToken(t, `{"sub":"1","iat":`+nowS+`,"exp":`+hourLater+`}`)
	noneAlg := signTokenRaw(`{"alg":"none","typ":"JWT"}`, `{"sub":"1","username":"u","exp":`+hourLater+`}`)
	hs384 := signTokenRaw(`{"alg":"HS384","typ":"JWT"}`, `{"sub":"1","username":"u","exp":`+hourLater+`}`)
	emptySub := signToken(t, `{"sub":"","username":"u","iat":`+nowS+`,"exp":`+hourLater+`}`)
	numericSub := signToken(t, `{"sub":1,"username":"u","iat":`+nowS+`,"exp":`+hourLater+`}`)
	stringExp := signToken(t, `{"sub":"7","username":"golden_ember_1","iat":`+nowS+`,"exp":"`+hourLater+`"}`)

	sigStart := strings.LastIndexByte(valid, '.')
	badSignature := valid[:sigStart] + ".c2lnbmF0dXJlLWlzLW5vdC12YWxpZC1hdC1hbGwtYXQtYWxsLTEyMzQ"

	cases := []struct {
		name   string
		header string
		stage  int
		uid    int64
	}{
		{"no header", "", jwtNoBearer, 0},
		{"not bearer", "Basic abc", jwtNoBearer, 0},
		{"garbage", "Bearer abc", jwtBadToken, 0},
		{"empty token", "Bearer ", jwtBadToken, 0},
		{"two segments", "Bearer aaaa.bbbb", jwtBadToken, 0},
		{"four segments", "Bearer aaaa.bbbb.cccc.dddd", jwtBadToken, 0},
		{"bad signature", "Bearer " + badSignature, jwtBadToken, 0},
		{"alg none", "Bearer " + noneAlg, jwtBadToken, 0},
		{"alg hs384", "Bearer " + hs384, jwtBadToken, 0},
		{"expired", "Bearer " + expired, jwtBadToken, 0},
		{"sub not integer", "Bearer " + badSub, jwtBadPayload, 0},
		{"sub empty", "Bearer " + emptySub, jwtBadPayload, 0},
		{"sub numeric json", "Bearer " + numericSub, jwtBadPayload, 0},
		{"no username", "Bearer " + noName, jwtBadPayload, 0},
		{"string exp", "Bearer " + stringExp, jwtOK, 7},
		{"valid", "Bearer " + valid, jwtOK, 1},
		{"valid with spaces", "Bearer   " + valid + "  ", jwtOK, 1},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var v jwtVerifier
			uid, name, stage := v.verify([]byte(tc.header), now.Unix())
			if stage != tc.stage {
				t.Fatalf("stage = %d, want %d", stage, tc.stage)
			}
			if stage != jwtOK {
				return
			}
			if uid != tc.uid {
				t.Fatalf("uid = %d, want %d", uid, tc.uid)
			}
			if string(name) != "golden_ember_1" {
				t.Fatalf("username = %q", name)
			}
		})
	}
}

func TestVerifyJWTUsernames(t *testing.T) {
	exp := `,"exp":2000003600}`
	for raw, want := range map[string]string{
		`plain`:                  `plain`,
		``:                       ``,
		`a\"b`:                   `a"b`,
		`a\u00e9`:                "a\u00e9",
		strings.Repeat("x", 300): strings.Repeat("x", 300),
	} {
		var v jwtVerifier
		tok := signToken(t, `{"sub":"1","username":"`+raw+`"`+exp)
		_, name, stage := v.verify([]byte("Bearer "+tok), 1_800_000_000)
		if stage != jwtOK || string(name) != want {
			t.Fatalf("username %q: stage=%d got %q", raw, stage, name)
		}
	}
}

func TestVerifyJWTNoAllocs(t *testing.T) {
	hdr := []byte("Bearer " + buildTestToken("7", "swift_ember_22228"))
	var v jwtVerifier
	if n := testing.AllocsPerRun(100, func() { v.verify(hdr, 1_800_000_000) }); n != 0 {
		t.Fatalf("verify allocates %v times per run", n)
	}
}

// TestMain wires the global signing secret the request parser verifies with.
func TestMain(m *testing.M) {
	jwtSecret = []byte(testSecret)
	os.Exit(m.Run())
}
