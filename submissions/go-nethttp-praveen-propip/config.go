package main

import (
	"fmt"
	"os"
	"strconv"
)

// Config is the runtime contract from SPEC.md, plus optional tuning knobs.
type Config struct {
	DatabaseURL string
	JWTSecret   []byte
	Host        string
	Port        string
	ExtraPort   string
	PoolSize    int32
	DBTimeoutMs int
}

// LoadConfig reads the environment. DATABASE_URL and JWT_SECRET are required;
// HOST, PORT, EXTRA_PORT, DB_POOL and DB_TIMEOUT_MS have SPEC/benchmark defaults.
func LoadConfig() (Config, error) {
	var cfg Config

	cfg.DatabaseURL = os.Getenv("DATABASE_URL")
	if cfg.DatabaseURL == "" {
		return Config{}, fmt.Errorf("missing required environment variable DATABASE_URL")
	}

	secret := os.Getenv("JWT_SECRET")
	if secret == "" {
		return Config{}, fmt.Errorf("missing required environment variable JWT_SECRET")
	}
	cfg.JWTSecret = []byte(secret)

	cfg.Host = envOr("HOST", "127.0.0.1")
	cfg.Port = envOr("PORT", "3000")
	cfg.ExtraPort = os.Getenv("EXTRA_PORT")

	cfg.PoolSize = 256 // benchmark-box default; each pooled conn holds its prepared statements
	if v := os.Getenv("DB_POOL"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > 512 {
			return Config{}, fmt.Errorf("invalid DB_POOL %q (want 1..512)", v)
		}
		cfg.PoolSize = int32(n)
	}

	cfg.DBTimeoutMs = 5000 // bound waiting for a pool connection plus the statement
	if v := os.Getenv("DB_TIMEOUT_MS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			return Config{}, fmt.Errorf("invalid DB_TIMEOUT_MS %q", v)
		}
		cfg.DBTimeoutMs = n
	}

	return cfg, nil
}

func envOr(name, fallback string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return fallback
}
