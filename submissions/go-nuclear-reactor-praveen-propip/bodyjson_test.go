package main

import (
	"strings"
	"testing"
)

func parseBody(t *testing.T, s string) (int, string) {
	t.Helper()
	sc := &parseScratch{}
	res := parsePostBody([]byte(s), &sc.body)
	return res, string(sc.body.bytes[:sc.body.n])
}

func TestParsePostBodyMatrix(t *testing.T) {
	long500 := strings.Repeat("b", 500)
	long501 := strings.Repeat("a", 501)

	cases := []struct {
		name     string
		body     string
		wantRes  int
		wantText string
	}{
		{"plain", `{"body":"hi"}`, bodyOK, "hi"},
		{"trimmed", `{"body":"  hi  "}`, bodyOK, "hi"},
		{"empty", `{"body":""}`, bodyRequired, ""},
		{"whitespace", `{"body":"   "}`, bodyRequired, ""},
		{"numeric body", `{"body":123}`, bodyRequired, ""},
		{"null body", `{"body":null}`, bodyRequired, ""},
		{"object body", `{"body":{}}`, bodyRequired, ""},
		{"missing member", `{}`, bodyRequired, ""},
		{"other member", `{"text":"hi"}`, bodyRequired, ""},
		{"empty input", ``, bodyRequired, ""},
		{"empty object ws", `  {}  `, bodyRequired, ""},
		{"malformed", `{bad json`, bodyMalformed, ""},
		{"truncated string", `{"body":"hi`, bodyMalformed, ""},
		{"trailing junk ignored", `{"body":"x"} garbage`, bodyOK, "x"},
		{"two members", `{"extra":1,"body":"x"}`, bodyOK, "x"},
		{"duplicate keys last wins", `{"body":"a","body":"bb"}`, bodyOK, "bb"},
		{"501 chars", `{"body":"` + long501 + `"}`, bodyTooLong, ""},
		{"500 chars ws", `{"body":"  ` + long500 + ` "}`, bodyOK, long500},
		{"unicode escape", `{"body":"caf\u00e9 \u2713"}`, bodyOK, "café ✓"},
		{"surrogate pair", `{"body":"emoji \ud83d\ude00 here"}`, bodyOK, "emoji \U0001F600 here"},
		{"raw control invalid", "{\"body\":\"a\nb\"}", bodyMalformed, ""},
		{"nul escape ok", `{"body":"a\u0000b"}`, bodyOK, "a\x00b"},
		{"lone surrogate invalid", `{"body":"\ud83d"`, bodyMalformed, ""},
		{"500 bmp 3-byte chars", `{"body":"` + strings.Repeat("✓", 500) + `"}`, bodyOK, strings.Repeat("✓", 500)},
		{"250 emoji ok", `{"body":"` + strings.Repeat("\U0001F600", 250) + `"}`, bodyOK, strings.Repeat("\U0001F600", 250)},
		{"251 emoji too long", `{"body":"` + strings.Repeat("\U0001F600", 251) + `"}`, bodyTooLong, ""},
		{"unicode ws trim", `{"body":" x "}`, bodyOK, "x"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			res, text := parseBody(t, tc.body)
			if res != tc.wantRes {
				t.Fatalf("res = %d, want %d (%q)", res, tc.wantRes, text)
			}
			if tc.wantText != "" || res == bodyOK && tc.wantText != "" {
				if text != tc.wantText {
					t.Fatalf("text = %q, want %q", text, tc.wantText)
				}
			}
		})
	}

	// escapes case body separately (the table string can't contain raw escapes
	// cleanly): a\"b\\c\/d\b\f\n\r\t
	sc := &parseScratch{}
	res := parsePostBody([]byte(`{"body":"a\"b\\c\/d\b\f\n\r\t"}`), &sc.body)
	if res != bodyOK || string(sc.body.bytes[:sc.body.n]) != "a\"b\\c/d\b" { // trailing \f\n\r\t are Unicode whitespace, trimmed
		t.Fatalf("escapes: res=%d text=%q", res, string(sc.body.bytes[:sc.body.n]))
	}
}
