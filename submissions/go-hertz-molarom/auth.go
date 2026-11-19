package main

import (
	"context"
	"net/http"
	"strconv"
	"strings"

	hertzapp "github.com/cloudwego/hertz/pkg/app"
	"github.com/golang-jwt/jwt/v5"
)

type ctxKey int

const (
	userKey ctxKey = iota
)

type principal struct {
	id       int64
	username string
}

func setUser(ctx context.Context, principal principal) context.Context {
	return context.WithValue(ctx, userKey, principal)
}

func getUser(ctx context.Context) (principal, bool) {
	v, ok := ctx.Value(userKey).(principal)
	return v, ok
}

func userId(sub string, err error) (int64, bool) {
	if err != nil {
		return 0, false
	}
	id, err := strconv.ParseInt(sub, 10, 64)
	return id, err == nil && id > 0
}

func (a *app) auth(next hertzapp.HandlerFunc) hertzapp.HandlerFunc {
	p := jwt.NewParser(jwt.WithValidMethods([]string{"HS256"}), jwt.WithExpirationRequired())

	return func(ctx context.Context, rc *hertzapp.RequestContext) {
		header := rc.Request.Header.Get("Authorization")
		tok, ok := strings.CutPrefix(header, "Bearer ")
		if !ok {
			a.error(rc, http.StatusUnauthorized, "missing bearer token")
			return
		}

		claims := jwt.MapClaims{}
		_, err := p.ParseWithClaims(tok, claims, func(t *jwt.Token) (any, error) {
			return a.jwtSecret, nil
		})
		if err != nil {
			a.error(rc, http.StatusUnauthorized, "invalid or expired token")
			return
		}

		id, ok := userId(claims.GetSubject())
		if !ok {
			a.error(rc, http.StatusUnauthorized, "invalid token payload")
			return
		}

		name, ok := claims["username"].(string)
		if !ok {
			a.error(rc, http.StatusUnauthorized, "invalid token payload")
			return
		}

		rctx := setUser(ctx, principal{id: id, username: name})
		next(rctx, rc)
	}
}
