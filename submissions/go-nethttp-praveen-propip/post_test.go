package main

import (
	"encoding/json"
	"testing"
)

// TestWireShapes pins the byte-level response format: compact JSON, key order
// exactly as SPEC.md shows, no HTML escaping of these ASCII-safe bodies (so the
// bytes are identical with or without the jsonv2 build experiment).
func TestWireShapes(t *testing.T) {
	cases := []struct {
		name string
		v    any
		want string
	}{
		{"error", errorBody{Error: "not found"}, `{"error":"not found"}`},
		{"like", likeResponse{Liked: true, AlreadyLiked: false, PostID: 7},
			`{"liked":true,"already_liked":false,"post_id":7}`},
		{"health ok", healthOK{"ok", "ok", 42}, `{"status":"ok","db":"ok","uptime_s":42}`},
		{"health degraded", healthDegraded{"degraded", "unreachable", "boom"},
			`{"status":"degraded","db":"unreachable","error":"boom"}`},
		{"post", postResponse{Post: Post{ID: 1, Body: "Fast tomorrow small bug big droplet.",
			CreatedAt: "2025-01-01T00:00:34.152Z", Author: "happy_nebula_10241", LikeCount: 11}},
			`{"post":{"id":1,"body":"Fast tomorrow small bug big droplet.","created_at":"2025-01-01T00:00:34.152Z","author":"happy_nebula_10241","like_count":11}}`},
		{"feed", feedResponse{Posts: []Post{
			{ID: 2, Body: "a", CreatedAt: "2025-01-01T00:00:00.000Z", Author: "u", LikeCount: 0},
			{ID: 1, Body: "b", CreatedAt: "2025-01-01T00:00:00.000Z", Author: "v", LikeCount: 3},
		}},
			`{"posts":[{"id":2,"body":"a","created_at":"2025-01-01T00:00:00.000Z","author":"u","like_count":0},` +
				`{"id":1,"body":"b","created_at":"2025-01-01T00:00:00.000Z","author":"v","like_count":3}]}`},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			b, err := marshalJSON(tc.v)
			if err != nil {
				t.Fatalf("marshal: %v", err)
			}
			if string(b) != tc.want {
				t.Fatalf("got  %s\nwant %s", b, tc.want)
			}
		})
	}
}

// TestMarshalEscapesRequiredCharacters covers bodies that do contain quotes,
// backslashes and control characters: the output must stay valid JSON that
// round-trips, whichever escaping flavor the build picked.
func TestMarshalEscapesRequiredCharacters(t *testing.T) {
	for _, body := range []string{
		`test: <b>&amp;</b> "quoted" / café ✓`,
		"back\\slash",
		"line\nbreak\ttab",
		"emoji \U0001F600 pair",
	} {
		b, err := marshalJSON(postResponse{Post: Post{ID: 1, Body: body, CreatedAt: "x", Author: "u", LikeCount: 0}})
		if err != nil {
			t.Fatalf("marshal %q: %v", body, err)
		}
		var back struct {
			Post Post `json:"post"`
		}
		if err := json.Unmarshal(b, &back); err != nil {
			t.Fatalf("output is not valid JSON: %v (%s)", err, b)
		}
		if back.Post.Body != body {
			t.Fatalf("round-trip changed body: got %q, want %q", back.Post.Body, body)
		}
	}
}
