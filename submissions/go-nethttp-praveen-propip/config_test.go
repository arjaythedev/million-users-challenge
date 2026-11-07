package main

import (
	"testing"
)

func TestLoadConfigDefaults(t *testing.T) {
	t.Setenv("DATABASE_URL", "postgres://feed:pw@db:5432/feed")
	t.Setenv("JWT_SECRET", "million-users-challenge")
	t.Setenv("HOST", "")
	t.Setenv("PORT", "")
	t.Setenv("EXTRA_PORT", "")
	t.Setenv("DB_POOL", "")
	t.Setenv("DB_TIMEOUT_MS", "")

	cfg, err := LoadConfig()
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if cfg.Host != "127.0.0.1" || cfg.Port != "3000" || cfg.ExtraPort != "" {
		t.Errorf("address defaults wrong: %+v", cfg)
	}
	if cfg.PoolSize != 256 || cfg.DBTimeoutMs != 5000 {
		t.Errorf("tuning defaults wrong: %+v", cfg)
	}
}

func TestLoadConfigOverrides(t *testing.T) {
	t.Setenv("DATABASE_URL", "postgres://feed:pw@db:5432/feed")
	t.Setenv("JWT_SECRET", "s")
	t.Setenv("HOST", "0.0.0.0")
	t.Setenv("PORT", "80")
	t.Setenv("EXTRA_PORT", "81")
	t.Setenv("DB_POOL", "512")
	t.Setenv("DB_TIMEOUT_MS", "2500")

	cfg, err := LoadConfig()
	if err != nil {
		t.Fatalf("LoadConfig: %v", err)
	}
	if cfg.Host != "0.0.0.0" || cfg.Port != "80" || cfg.ExtraPort != "81" {
		t.Errorf("address override wrong: %+v", cfg)
	}
	if cfg.PoolSize != 512 || cfg.DBTimeoutMs != 2500 {
		t.Errorf("tuning override wrong: %+v", cfg)
	}
}

func TestLoadConfigRejects(t *testing.T) {
	cases := []struct {
		name string
		env  map[string]string
	}{
		{"missing database url", map[string]string{"JWT_SECRET": "s"}},
		{"missing jwt secret", map[string]string{"DATABASE_URL": "postgres://x/y"}},
		{"pool too big", map[string]string{"DATABASE_URL": "postgres://x/y", "JWT_SECRET": "s", "DB_POOL": "513"}},
		{"pool not a number", map[string]string{"DATABASE_URL": "postgres://x/y", "JWT_SECRET": "s", "DB_POOL": "abc"}},
		{"bad timeout", map[string]string{"DATABASE_URL": "postgres://x/y", "JWT_SECRET": "s", "DB_TIMEOUT_MS": "0"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			for _, k := range []string{"DATABASE_URL", "JWT_SECRET", "DB_POOL", "DB_TIMEOUT_MS"} {
				t.Setenv(k, "")
			}
			for k, v := range tc.env {
				t.Setenv(k, v)
			}
			if _, err := LoadConfig(); err == nil {
				t.Fatal("LoadConfig succeeded, want error")
			}
		})
	}
}
