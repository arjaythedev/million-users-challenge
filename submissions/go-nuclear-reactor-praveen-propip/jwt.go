package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"hash"
)

// JWT error stages, matching the SPEC errors table.
const (
	jwtOK         = iota
	jwtBadToken   // malformed, wrong alg, bad signature, expired
	jwtBadPayload // valid token, unusable claims
	jwtNoBearer   // header absent or not "Bearer ..."
)

// jwtVerifier checks tokens without allocating. The HMAC object is only a
// precomputation of the key's two pad blocks; it holds nothing about any
// token, and every request runs the full verification (rules 7 and 9).
type jwtVerifier struct {
	mac  hash.Hash
	sum  [sha256.Size]byte
	hdr  [256]byte
	sig  [64]byte
	pay  [4096]byte
	name postBody
}

var b64url = base64.RawURLEncoding.Strict()

// verify checks `Authorization: Bearer <jwt>` per SPEC: HS256 only, signature
// and exp. The returned username is valid until the next call.
func (v *jwtVerifier) verify(header []byte, now int64) (int64, []byte, int) {
	if !hasBearerPrefix(header) {
		return 0, nil, jwtNoBearer
	}
	tok := trimSpaces(header[len("Bearer "):])

	dot1 := bytes.IndexByte(tok, '.')
	if dot1 < 0 {
		return 0, nil, jwtBadToken
	}
	dot2 := bytes.IndexByte(tok[dot1+1:], '.')
	if dot2 < 0 {
		return 0, nil, jwtBadToken
	}
	dot2 += dot1 + 1
	if bytes.IndexByte(tok[dot2+1:], '.') >= 0 {
		return 0, nil, jwtBadToken // more than three segments
	}

	// Header: must declare alg HS256.
	hdr, ok := b64URLDecode(v.hdr[:], tok[:dot1])
	if !ok {
		return 0, nil, jwtBadToken
	}
	if alg, ok := jsonField(hdr, "alg"); !ok || !bytesEqual(alg, "HS256") {
		return 0, nil, jwtBadToken
	}

	// Signature: HMAC-SHA256 over the signing input, constant-time compare.
	sig, ok := b64URLDecode(v.sig[:], tok[dot2+1:])
	if !ok {
		return 0, nil, jwtBadToken
	}
	if v.mac == nil {
		v.mac = hmac.New(sha256.New, jwtSecret)
	}
	v.mac.Reset()
	v.mac.Write(tok[:dot2])
	if !hmac.Equal(v.mac.Sum(v.sum[:0]), sig) {
		return 0, nil, jwtBadToken
	}

	payload, ok := b64URLDecode(v.pay[:], tok[dot1+1:dot2])
	if !ok {
		return 0, nil, jwtBadToken
	}

	// exp: JSON number (or digit string); expired or absent is invalid.
	if !jsonCheckExp(payload, now) {
		return 0, nil, jwtBadToken
	}

	sub, ok := jsonField(payload, "sub")
	if !ok {
		return 0, nil, jwtBadPayload
	}
	uid, ok := parsePositiveInt(sub)
	if !ok {
		return 0, nil, jwtBadPayload
	}
	name, ok := jsonField(payload, "username")
	if !ok {
		return 0, nil, jwtBadPayload
	}
	if bytes.IndexByte(name, '\\') >= 0 { // the rare escaped name: decode it
		v.name.n, v.name.units = 0, 0
		start := cap(payload) - cap(name) // name is a subslice of payload
		if _, ok := decodeJSONString(payload, start, &v.name); !ok {
			return 0, nil, jwtBadPayload
		}
		name = v.name.bytes[:v.name.n]
	}
	return uid, name, jwtOK
}

func hasBearerPrefix(h []byte) bool {
	const p = "Bearer "
	if len(h) < len(p) {
		return false
	}
	for i := 0; i < len(p); i++ {
		if h[i] != p[i] {
			return false
		}
	}
	return true
}

func trimSpaces(b []byte) []byte {
	for len(b) > 0 && (b[0] == ' ' || b[0] == '\t') {
		b = b[1:]
	}
	for len(b) > 0 && (b[len(b)-1] == ' ' || b[len(b)-1] == '\t') {
		b = b[:len(b)-1]
	}
	return b
}

// b64URLDecode decodes base64url into dst, tolerating trailing padding.
func b64URLDecode(dst, b []byte) ([]byte, bool) {
	n := len(b)
	for n > 0 && b[n-1] == '=' { // RawURLEncoding rejects padding
		n--
	}
	b = b[:n]
	if base64.RawURLEncoding.DecodedLen(len(b)) > len(dst) {
		return nil, false
	}
	n, err := b64url.Decode(dst, b)
	if err != nil {
		return nil, false
	}
	return dst[:n], true
}

// jsonField finds a top-level string value for key in a flat JSON object.
// Only flat token payloads are expected ({"sub","username","iat","exp"}), but
// strings are parsed correctly (escapes) and nested objects/arrays are skipped
// without misreading their contents. Returns the raw decoded string value.
func jsonField(obj []byte, key string) ([]byte, bool) {
	i := 0
	for i < len(obj) {
		// next key: expect '"' at object member position
		for i < len(obj) && obj[i] != '"' {
			i++
		}
		if i >= len(obj) {
			return nil, false
		}
		kStart := i + 1
		kEnd, ok := scanJSONString(obj, kStart)
		if !ok {
			return nil, false
		}
		i = kEnd
		for i < len(obj) && (obj[i] == ' ' || obj[i] == ':') {
			i++
		}
		if i >= len(obj) || obj[i] != '"' {
			// non-string value: skip it structurally
			j, ok := skipJSONValue(obj, i)
			if !ok {
				return nil, false
			}
			i = j
			continue
		}
		vStart := i + 1
		vEnd, ok := scanJSONString(obj, vStart)
		if !ok {
			return nil, false
		}
		if string(obj[kStart:kEnd-1]) == key {
			return obj[vStart : vEnd-1], true
		}
		i = vEnd
		for i < len(obj) && (obj[i] == ',' || obj[i] == ' ') {
			i++
		}
	}
	return nil, false
}

// scanJSONString returns the index just past the closing quote of the string
// starting at start (which must point at the first content byte).
func scanJSONString(b []byte, start int) (int, bool) {
	i := start
	for i < len(b) {
		c := b[i]
		if c == '"' {
			return i + 1, true
		}
		if c == '\\' {
			i += 2
			continue
		}
		i++
	}
	return 0, false
}

// skipJSONValue skips one JSON value (for "iat":1234 and any nested payload).
func skipJSONValue(b []byte, i int) (int, bool) {
	if i >= len(b) {
		return 0, false
	}
	switch b[i] {
	case '"':
		end, ok := scanJSONString(b, i+1)
		return end, ok
	case '{', '[':
		open := b[i]
		closeC := byte('}')
		if open == '[' {
			closeC = ']'
		}
		depth := 0
		for i < len(b) {
			c := b[i]
			if c == '"' {
				end, ok := scanJSONString(b, i+1)
				if !ok {
					return 0, false
				}
				i = end
				continue
			}
			if c == open {
				depth++
			} else if c == closeC {
				depth--
				if depth == 0 {
					return i + 1, true
				}
			}
			i++
		}
		return 0, false
	default: // number / true / false / null
		for i < len(b) && b[i] != ',' && b[i] != '}' && b[i] != ']' && b[i] != ' ' {
			i++
		}
		return i, true
	}
}

// jsonCheckExp verifies the exp claim against now: a JSON number or digit
// string, strictly in the future. Missing or malformed exp is invalid.
func jsonCheckExp(payload []byte, now int64) bool {
	i := 0
	for i < len(payload) {
		for i < len(payload) && payload[i] != '"' {
			i++
		}
		if i >= len(payload) {
			return false
		}
		kStart := i + 1
		kEnd, ok := scanJSONString(payload, kStart)
		if !ok {
			return false
		}
		j := kEnd
		for j < len(payload) && (payload[j] == ' ' || payload[j] == ':') {
			j++
		}
		if string(payload[kStart:kEnd-1]) == "exp" {
			quoted := j < len(payload) && payload[j] == '"'
			if quoted {
				j++
			}
			start := j
			for j < len(payload) && payload[j] >= '0' && payload[j] <= '9' {
				j++
			}
			if j == start {
				return false
			}
			if quoted && (j >= len(payload) || payload[j] != '"') {
				return false
			}
			sec, ok := parsePositiveInt(payload[start:j])
			return ok && now < sec
		}
		skipped, ok := skipJSONValue(payload, j)
		if !ok {
			return false
		}
		i = skipped
		for i < len(payload) && (payload[i] == ',' || payload[i] == ' ') {
			i++
		}
	}
	return false
}

// parsePositiveInt parses a positive decimal integer, as
// strconv.ParseInt(strings.TrimSpace(s), 10, 64) with result > 0.
func parsePositiveInt(s []byte) (int64, bool) {
	b := trimSpaces(s)
	if len(b) == 0 {
		return 0, false
	}
	if b[0] == '+' {
		b = b[1:]
	}
	if len(b) == 0 {
		return 0, false
	}
	var v int64
	for _, c := range b {
		if c < '0' || c > '9' || v > (1<<63-1)/10 {
			return 0, false
		}
		v = v*10 + int64(c-'0')
	}
	return v, v > 0
}
