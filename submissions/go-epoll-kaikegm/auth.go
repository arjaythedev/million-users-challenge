package main

import (
	"errors"
	"strconv"

	"github.com/KaikeGM/zerojwt/v2"
)

type AuthChecker struct {
	verifier *zerojwt.Verifier
}

func NewAuthChecker(secret []byte) (*AuthChecker, error) {
	v, err := zerojwt.New(secret, zerojwt.Options{
		AllowShortSecret: true,
	})
	if err != nil {
		return nil, err
	}
	return &AuthChecker{verifier: v}, nil
}

type AuthResult struct {
	UserID   int64
	Username string
	Status   int
	ErrorMsg string
}

func (ac *AuthChecker) Check(authHeader []byte) (res AuthResult) {
	if len(authHeader) == 0 {
		res.Status = 401
		res.ErrorMsg = "missing bearer token"
		return
	}
	// Case-insensitive check for "Bearer " prefix
	if len(authHeader) < 7 ||
		(authHeader[0]|0x20 != 'b') ||
		(authHeader[1]|0x20 != 'e') ||
		(authHeader[2]|0x20 != 'a') ||
		(authHeader[3]|0x20 != 'r') ||
		(authHeader[4]|0x20 != 'e') ||
		(authHeader[5]|0x20 != 'r') ||
		(authHeader[6] != ' ') {
		res.Status = 401
		res.ErrorMsg = "missing bearer token"
		return
	}

	var claims zerojwt.Claims
	err := ac.verifier.VerifyBearerNow(authHeader, &claims)
	if err != nil {
		if errors.Is(err, zerojwt.ErrMissingBearer) {
			res.Status = 401
			res.ErrorMsg = "missing bearer token"
			return
		}
		if errors.Is(err, zerojwt.ErrInvalidPayload) {
			res.Status = 401
			res.ErrorMsg = "invalid token payload"
			return
		}
		res.Status = 401
		res.ErrorMsg = "invalid or expired token"
		return
	}

	if !claims.HasSub || !claims.HasUsername || len(claims.Username()) == 0 {
		res.Status = 401
		res.ErrorMsg = "invalid token payload"
		return
	}

	id := claims.SubjectID
	if id <= 0 {
		var err error
		id, err = strconv.ParseInt(string(claims.Subject()), 10, 64)
		if err != nil || id <= 0 {
			res.Status = 401
			res.ErrorMsg = "invalid token payload"
			return
		}
	}

	res.UserID = id
	res.Username = string(claims.Username())
	return
}
