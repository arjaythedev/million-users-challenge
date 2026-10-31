package main

import (
	"log/slog"
	"math"
	"net/http"
	"strconv"
	"strings"

	"github.com/golang-jwt/jwt/v5"
)

// user is the authenticated caller extracted from a verified bearer token.
type user struct {
	id       int64
	username string
}

const bearerPrefix = "Bearer "

// requireAuth verifies `Authorization: Bearer <jwt>` (HS256 only, exp checked)
// before the wrapped handler runs, so auth errors take precedence over id or
// body validation, as SPEC.md requires.
func (a *app) requireAuth(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		header := r.Header.Get("Authorization")
		if !strings.HasPrefix(header, bearerPrefix) {
			writeError(w, http.StatusUnauthorized, "missing bearer token")
			return
		}
		claims := jwt.MapClaims{}
		_, err := jwt.ParseWithClaims(
			strings.TrimSpace(header[len(bearerPrefix):]),
			claims,
			func(*jwt.Token) (any, error) { return a.cfg.JWTSecret, nil },
			jwt.WithValidMethods([]string{"HS256"}),
		)
		if err != nil {
			writeError(w, http.StatusUnauthorized, "invalid or expired token")
			return
		}
		id, okID := userIDFromClaims(claims["sub"])
		name, okName := claims["username"].(string)
		if !okID || !okName {
			writeError(w, http.StatusUnauthorized, "invalid token payload")
			return
		}
		next(w, r.WithContext(withUser(r.Context(), user{id: id, username: name})))
	}
}

// userIDFromClaims accepts the sub claim as a positive-integer string, or as a
// JSON number that is a positive integer within float64's safe integer range.
func userIDFromClaims(sub any) (int64, bool) {
	switch v := sub.(type) {
	case string:
		id, err := strconv.ParseInt(strings.TrimSpace(v), 10, 64)
		return id, err == nil && id > 0
	case float64:
		return int64(v), v > 0 && v == math.Trunc(v) && v <= 1<<53
	default:
		return 0, false
	}
}

// internalError logs the cause (without secrets or payloads) and answers 500.
func internalError(w http.ResponseWriter, op string, err error) {
	slog.Error("request failed", "op", op, "err", err)
	writeError(w, http.StatusInternalServerError, "internal server error")
}
