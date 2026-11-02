package main

import (
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestParsePostID(t *testing.T) {
	cases := []struct {
		in   string
		want int64
		ok   bool
	}{
		{"1", 1, true},
		{"500000", 500000, true},
		{"0", 0, false},
		{"-1", -1, false},
		{"abc", 0, false},
		{"1.5", 0, false},
		{"", 0, false},
		{" 1", 0, false},
		{"99999999999999999999", 0, false}, // overflows int64: ok is false, value unspecified
	}
	for _, tc := range cases {
		got, ok := parsePostID(tc.in)
		if ok != tc.ok || (ok && got != tc.want) {
			t.Errorf("parsePostID(%q) = (%d, %v), want (%d, %v)", tc.in, got, ok, tc.want, tc.ok)
		}
	}
}

func TestReadPostBody(t *testing.T) {
	long500 := strings.Repeat("b", 500)
	long501 := strings.Repeat("a", 501)

	cases := []struct {
		name     string
		body     string
		wantBody string
		wantErr  string // "" means success
	}{
		{"plain", `{"body":"hi"}`, "hi", ""},
		{"trimmed", `{"body":"  hi  "}`, "hi", ""},
		{"empty", `{"body":""}`, "", "body is required"},
		{"whitespace", `{"body":"   "}`, "", "body is required"},
		{"numeric body", `{"body":123}`, "", "body is required"},
		{"missing body", `{}`, "", "body is required"},
		{"absent body key", `{"text":"hi"}`, "", "body is required"},
		{"empty request", ``, "", "body is required"}, // io.EOF is not malformed
		{"malformed", `{bad json`, "", "malformed JSON body"},
		{"truncated string", `{"body":"hi`, "", "malformed JSON body"},
		{"501 chars", `{"body":"` + long501 + `"}`, "", "body must be at most 500 characters"},
		{"500 chars with spaces", `{"body":"  ` + long500 + ` "}`, long500, ""},
		{"unicode escapes", `{"body":"caf\u00e9 \u2713"}`, "café ✓", ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			r := httptest.NewRequest("POST", "/posts", strings.NewReader(tc.body))
			got, status, msg, ok := readPostBody(r)
			if tc.wantErr == "" {
				if !ok || got != tc.wantBody {
					t.Fatalf("got (%q, %d, %q, %v), want body %q", got, status, msg, ok, tc.wantBody)
				}
				return
			}
			if ok || msg != tc.wantErr {
				t.Fatalf("got (%q, %d, %q, %v), want error %q", got, status, msg, ok, tc.wantErr)
			}
		})
	}
}

func TestFormatCreatedAtTruncates(t *testing.T) {
	// Microsecond precision in the DB; truncate (not round) to milliseconds.
	tcs := []struct {
		in   time.Time
		want string
	}{
		{time.Date(2025, 12, 31, 23, 59, 19, 409_000_000, time.UTC), "2025-12-31T23:59:19.409Z"},
		{time.Date(2025, 12, 31, 23, 59, 19, 409_999_000, time.UTC), "2025-12-31T23:59:19.409Z"},
		{time.Date(2025, 1, 1, 0, 0, 34, 152_100_000, time.UTC), "2025-01-01T00:00:34.152Z"},
		{time.Date(2025, 1, 1, 0, 0, 0, 0, time.UTC), "2025-01-01T00:00:00.000Z"},
	}
	for _, tc := range tcs {
		if got := formatCreatedAt(tc.in); got != tc.want {
			t.Errorf("formatCreatedAt(%v) = %q, want %q", tc.in, got, tc.want)
		}
	}
}
