package main

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/jackc/pgx/v5/pgxpool"
)

// newPool builds the pgx connection pool. Statement preparation is handled by
// pgx's statement cache (QueryExecModeCacheStatement, the default): each pooled
// connection prepares postSelect and the inserts once and reuses the plans,
// which rules 3 and 7 explicitly allow.
func newPool(ctx context.Context, cfg Config) (*pgxpool.Pool, error) {
	poolCfg, err := pgxpool.ParseConfig(cfg.DatabaseURL)
	if err != nil {
		return nil, fmt.Errorf("DATABASE_URL: %w", err)
	}
	poolCfg.MaxConns = cfg.PoolSize
	// Hold the pool at size from the start: on the benchmark box traffic ramps
	// in over 60 s and a cold pool would spend that window opening connections.
	poolCfg.MinConns = cfg.PoolSize
	pool, err := pgxpool.NewWithConfig(ctx, poolCfg)
	if err != nil {
		return nil, fmt.Errorf("connect: %w", err)
	}
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("ping: %w", err)
	}
	slog.Info("database pool ready", "size", cfg.PoolSize)
	return pool, nil
}
