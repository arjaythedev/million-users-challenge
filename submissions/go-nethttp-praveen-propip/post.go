package main

import (
	"time"

	jsonv2 "encoding/json/v2"
)

// createdAtLayout renders TIMESTAMPTZ on the wire: UTC, millisecond precision,
// Z suffix. Go's fractional-second layout truncates, it never rounds (SPEC.md).
const createdAtLayout = "2006-01-02T15:04:05.000Z"

// Post is the wire object; field order is the key order required by SPEC.md.
type Post struct {
	ID        int64  `json:"id"`
	Body      string `json:"body"`
	CreatedAt string `json:"created_at"`
	Author    string `json:"author"`
	LikeCount int64  `json:"like_count"`
}

func formatCreatedAt(t time.Time) string { return t.UTC().Format(createdAtLayout) }

type postResponse struct {
	Post Post `json:"post"`
}

type feedResponse struct {
	Posts []Post `json:"posts"`
}

type errorBody struct {
	Error string `json:"error"`
}

type likeResponse struct {
	Liked        bool  `json:"liked"`
	AlreadyLiked bool  `json:"already_liked"`
	PostID       int64 `json:"post_id"`
}

type healthOK struct {
	Status  string `json:"status"`
	DB      string `json:"db"`
	UptimeS int64  `json:"uptime_s"`
}

type healthDegraded struct {
	Status string `json:"status"`
	DB     string `json:"db"`
	Error  string `json:"error"`
}

// marshalJSON encodes v compactly with encoding/json/v2 (key order = field
// order; compact output; HTML characters are not escaped, which SPEC.md allows).
func marshalJSON(v any) ([]byte, error) {
	return jsonv2.Marshal(v)
}
