package main

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"time"
)

type config struct {
	dbUrl             string
	dbPool            int
	writeWorkers      int
	writeBatch        int
	jwtSecret         []byte
	host, port        string
	extraPort         string
	debugPort         string
	idleTimeout       time.Duration
	readTimeout       time.Duration
	keepAliveTimeout  time.Duration
	keepAlivesEnabled bool
}

func (c config) validate() error {
	if c.dbUrl == "" {
		return errors.New("config: missing required value: DB_URL")
	}

	if c.jwtSecret == nil {
		return errors.New("config: missing required value: JWT_SECRET")
	}

	return nil
}

type parser struct {
	err error
}

func (p *parser) str(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func (p *parser) bytes(key string, def []byte) []byte {
	v := p.str(key, string(def))
	if v == "" {
		return nil
	}
	return []byte(v)
}

func (p *parser) int(key string, def int) int {
	v := os.Getenv(key)
	if v == "" {
		return def
	}

	i, err := strconv.Atoi(v)
	if err != nil {
		p.record(key, err)
		return def
	}
	return i
}

func (p *parser) boolean(key string, def bool) bool {
	v := os.Getenv(key)
	if v == "" {
		return def
	}
	b, err := strconv.ParseBool(v)
	if err != nil {
		p.record(key, err)
		return def
	}
	return b
}

func (p *parser) dur(key string, def time.Duration) time.Duration {
	v := os.Getenv(key)
	if v == "" {
		return def
	}
	d, err := time.ParseDuration(v)
	if err != nil {
		p.record(key, err)
		return def
	}
	return d
}

func (p *parser) record(key string, err error) {
	p.err = errors.Join(p.err, fmt.Errorf("config: %s: %w", key, err))
}
