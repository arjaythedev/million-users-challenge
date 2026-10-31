package main

import (
	"context"
	"errors"
	"os"
	"sync"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgconn"
)

// TestPipelineDBConcurrent runs against a disposable copy of the seed database:
// TEST_DATABASE_URL=postgres://... go test -race -run PipelineDB
func TestPipelineDBConcurrent(t *testing.T) {
	url := os.Getenv("TEST_DATABASE_URL")
	if url == "" {
		t.Skip("TEST_DATABASE_URL not set")
	}
	ctx := context.Background()
	d, err := newPipelineDB(ctx, url, 4, time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	var wg sync.WaitGroup
	for i := range 2000 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			ctx, cancel := context.WithTimeout(ctx, dbTimeout)
			defer cancel()
			switch i % 4 {
			case 0:
				j := &dbJob{stmt: stmtFeed, a: feedSize, posts: make([]post, 0, feedSize)}
				if err := d.do(ctx, j); err != nil || len(j.posts) != feedSize {
					t.Errorf("feed: %v, %d posts", err, len(j.posts))
				}
			case 1:
				j := &dbJob{stmt: stmtPost, a: 1}
				if err := d.do(ctx, j); err != nil || j.post.ID != 1 || j.post.Author == "" {
					t.Errorf("post: %v, %+v", err, j.post)
				}
			case 2:
				// A like on a missing post fails alone, inside a batch of successful statements.
				err := d.do(ctx, &dbJob{stmt: stmtLike, a: 1, b: 1 << 40})
				var pe *pgconn.PgError
				if !errors.As(err, &pe) || pe.Code != "23503" {
					t.Errorf("like on missing post: %v", err)
				}
			case 3:
				if err := d.do(ctx, &dbJob{stmt: stmtPost, a: 1 << 40}); !errors.Is(err, errNoRows) {
					t.Errorf("missing post: %v", err)
				}
			}
		}()
	}
	wg.Wait()
}
