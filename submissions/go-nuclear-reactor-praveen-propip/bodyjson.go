package main

// POST /posts body validation with the same observable contract as the
// production stack's encoding/json decode into {"body": any}: valid JSON with
// a string "body" member, trimmed and length-checked in UTF-16 code units.

const (
	bodyOK = iota
	bodyMalformed
	bodyRequired
	bodyTooLong
)

const (
	maxBodyUTF16 = 500  // SPEC: at most 500 characters after trimming
	maxBodyBytes = 1536 // 500 BMP chars of 3 UTF-8 bytes each
)

// postBody accumulates the decoded "body" string value.
type postBody struct {
	bytes [maxBodyBytes]byte
	n     int
	units int // UTF-16 code units, capped as we go
}

// parseScratch is the event loop's scratch space for request parsing: the
// token verifier and the decoded post body. now is the loop's clock, read once
// per wake-up; zero means read the clock on each request.
type parseScratch struct {
	now  int64
	jwt  jwtVerifier
	body postBody
}

// parsePostBody validates buf as {"body": "..."} and fills out with the
// trimmed value. Errors follow the SPEC table ordering: JSON syntax first
// ("malformed JSON body"), then presence/type/emptiness ("body is required"),
// then length ("body must be at most 500 characters").
func parsePostBody(buf []byte, out *postBody) int {
	out.n, out.units = 0, 0
	i := 0
	i = skipWS(buf, i)
	if i >= len(buf) {
		// An empty request is an EOF decode, which the spec folds into
		// "body is required" (no JSON was malformed).
		return bodyRequired
	}
	if buf[i] != '{' {
		return bodyMalformed
	}
	i++
	i = skipWS(buf, i)
	if i < len(buf) && buf[i] == '}' {
		return bodyRequired // {} has no body member
	}
	found := false
	for {
		i = skipWS(buf, i)
		if i >= len(buf) || buf[i] != '"' {
			return bodyMalformed
		}
		kStart := i + 1
		kEnd, ok := scanJSONString(buf, kStart)
		if !ok {
			return bodyMalformed
		}
		key := buf[kStart : kEnd-1]
		i = kEnd
		i = skipWS(buf, i)
		if i >= len(buf) || buf[i] != ':' {
			return bodyMalformed
		}
		i++
		i = skipWS(buf, i)
		if bytesEqualASCII(key, "body") {
			if i >= len(buf) || buf[i] != '"' {
				// present but not a string: "body is required", but only if
				// the value itself parses
				j, ok := skipJSONValue(buf, i)
				if !ok {
					return bodyMalformed
				}
				i = j
				found = true
				out.n, out.units = 0, 0
			} else {
				out.n, out.units = 0, 0 // duplicate keys: last one wins
				vStart := i + 1
				vEnd, ok := decodeJSONString(buf, vStart, out)
				if !ok {
					return bodyMalformed
				}
				i = vEnd
				found = true
			}
		} else {
			j, ok := skipJSONValue(buf, i)
			if !ok {
				return bodyMalformed
			}
			i = j
		}
		i = skipWS(buf, i)
		if i < len(buf) && buf[i] == ',' {
			i++
			continue
		}
		if i < len(buf) && buf[i] == '}' {
			i++
			break
		}
		return bodyMalformed
	}
	_ = i // trailing bytes after the value are ignored, like json.Decoder
	if !found || out.n == 0 {
		return bodyRequired
	}
	trimUnicodeSpace(out)
	if out.n == 0 {
		return bodyRequired
	}
	if out.units > maxBodyUTF16 {
		return bodyTooLong
	}
	return bodyOK
}

func skipWS(b []byte, i int) int {
	for i < len(b) {
		switch b[i] {
		case ' ', '\t', '\n', '\r':
			i++
		default:
			return i
		}
	}
	return i
}

func bytesEqualASCII(a []byte, s string) bool { return bytesEqual(a, s) }

// decodeJSONString decodes the JSON string starting at start (first content
// byte), appending decoded bytes and counting UTF-16 code units into out.
func decodeJSONString(b []byte, start int, out *postBody) (int, bool) {
	i := start
	for i < len(b) {
		c := b[i]
		switch {
		case c == '"':
			return i + 1, true
		case c < 0x20:
			return 0, false // raw control characters are invalid JSON
		case c != '\\':
			n := byteToRuneLen(b, i)
			if n > 1 {
				if i+n > len(b) {
					return 0, false
				}
				for k := 1; k < n; k++ {
					if b[i+k]&0xC0 != 0x80 {
						return 0, false // invalid UTF-8 continuation
					}
				}
			}
			if out.n+n > maxBodyBytes {
				// Overflow cannot happen for valid input (500 UTF-16 units are
				// at most 1500 bytes); treat as malformed rather than truncate.
				return 0, false
			}
			for k := 0; k < n; k++ {
				out.bytes[out.n] = b[i+k]
				out.n++
			}
			out.units += utf16Units(n)
			i += n
		default:
			if i+1 >= len(b) {
				return 0, false
			}
			switch b[i+1] {
			case '"', '\\', '/':
				out.bytes[out.n] = b[i+1]
				out.n++
				out.units++
				i += 2
			case 'b':
				out.bytes[out.n] = '\b'
				out.n++
				out.units++
				i += 2
			case 'f':
				out.bytes[out.n] = '\f'
				out.n++
				out.units++
				i += 2
			case 'n':
				out.bytes[out.n] = '\n'
				out.n++
				out.units++
				i += 2
			case 'r':
				out.bytes[out.n] = '\r'
				out.n++
				out.units++
				i += 2
			case 't':
				out.bytes[out.n] = '\t'
				out.n++
				out.units++
				i += 2
			case 'u':
				r, ni, ok := decodeUnicodeEscape(b, i)
				if !ok {
					return 0, false
				}
				out.n += appendRune(out.bytes[out.n:], r)
				out.units += utf16Units(runeLen(r))
				i = ni
			default:
				return 0, false
			}
		}
	}
	return 0, false
}

// decodeUnicodeEscape parses \uXXXX (with surrogate pair support) at b[i],
// returning the rune and the index past the escape.
func decodeUnicodeEscape(b []byte, i int) (rune, int, bool) {
	r, ni, ok := hex4(b, i+2)
	if !ok {
		return 0, 0, false
	}
	if r >= 0xD800 && r <= 0xDBFF { // high surrogate: require the low half
		if ni+2 <= len(b) && b[ni] == '\\' && b[ni+1] == 'u' {
			r2, ni2, ok := hex4(b, ni+2)
			if !ok || r2 < 0xDC00 || r2 > 0xDFFF {
				return 0, 0, false
			}
			return 0x10000 + (r-0xD800)<<10 + (r2 - 0xDC00), ni2, true
		}
		return 0, 0, false
	}
	if r >= 0xDC00 && r <= 0xDFFF {
		return 0, 0, false // lone low surrogate
	}
	return r, ni, true
}

func hex4(b []byte, i int) (rune, int, bool) {
	if i+4 > len(b) {
		return 0, 0, false
	}
	var v rune
	for k := 0; k < 4; k++ {
		c := b[i+k]
		switch {
		case c >= '0' && c <= '9':
			v = v<<4 | rune(c-'0')
		case c >= 'a' && c <= 'f':
			v = v<<4 | rune(c-'a'+10)
		case c >= 'A' && c <= 'F':
			v = v<<4 | rune(c-'A'+10)
		default:
			return 0, 0, false
		}
	}
	return v, i + 4, true
}

// byteToRuneLen returns the UTF-8 rune length starting at b[i] (1 for ASCII).
func byteToRuneLen(b []byte, i int) int {
	c := b[i]
	n := 1
	if c >= 0xF0 {
		n = 4
	} else if c >= 0xE0 {
		n = 3
	} else if c >= 0xC0 {
		n = 2
	}
	return n
}

func utf16Units(runeLen int) int {
	if runeLen >= 4 {
		return 2
	}
	return 1
}

func runeLen(r rune) int {
	switch {
	case r > 0xFFFF:
		return 4
	case r > 0x7FF:
		return 3
	case r > 0x7F:
		return 2
	default:
		return 1
	}
}

// appendRune encodes r as UTF-8 into dst, returning bytes written.
func appendRune(dst []byte, r rune) int {
	switch {
	case r > 0xFFFF:
		dst[0] = byte(0xF0 | r>>18)
		dst[1] = byte(0x80 | r>>12&0x3F)
		dst[2] = byte(0x80 | r>>6&0x3F)
		dst[3] = byte(0x80 | r&0x3F)
		return 4
	case r > 0x7FF:
		dst[0] = byte(0xE0 | r>>12)
		dst[1] = byte(0x80 | r>>6&0x3F)
		dst[2] = byte(0x80 | r&0x3F)
		return 3
	case r > 0x7F:
		dst[0] = byte(0xC0 | r>>6)
		dst[1] = byte(0x80 | r&0x3F)
		return 2
	default:
		dst[0] = byte(r)
		return 1
	}
}

// trimUnicodeSpace trims Unicode whitespace from both ends of out.bytes, the
// same set strings.TrimSpace uses.
func trimUnicodeSpace(out *postBody) {
	b := out.bytes[:out.n]
	start := 0
	for start < len(b) {
		r, size := decodeRune(b[start:])
		if !isUnicodeSpace(r) {
			break
		}
		start += size
	}
	end := len(b)
	for end > start {
		r, size := decodeLastRune(b[:end])
		if !isUnicodeSpace(r) {
			break
		}
		end -= size
	}
	if start > 0 {
		copy(b, b[start:end])
	}
	out.n = end - start
	out.units = countUTF16(out.bytes[:out.n])
}

func decodeRune(b []byte) (rune, int) {
	c := b[0]
	if c < 0x80 {
		return rune(c), 1
	}
	n := byteToRuneLen(b, 0)
	var r rune
	switch n {
	case 2:
		r = rune(c & 0x1F)
	case 3:
		r = rune(c & 0x0F)
	case 4:
		r = rune(c & 0x07)
	}
	for k := 1; k < n && k < len(b); k++ {
		r = r<<6 | rune(b[k]&0x3F)
	}
	return r, n
}

func decodeLastRune(b []byte) (rune, int) {
	i := len(b) - 1
	if b[i] < 0x80 {
		return rune(b[i]), 1
	}
	start := i
	for start > 0 && b[start]&0xC0 == 0x80 {
		start--
	}
	return decodeRune(b[start:])
}

func countUTF16(b []byte) int {
	n := 0
	i := 0
	for i < len(b) {
		size := byteToRuneLen(b, i)
		if size >= 4 {
			n += 2
		} else {
			n++
		}
		i += size
	}
	return n
}

// isUnicodeSpace matches strings.TrimSpace's set.
func isUnicodeSpace(r rune) bool {
	switch r {
	case '\t', '\n', '\v', '\f', '\r', ' ', 0x85, 0xA0:
		return true
	case 0x1680:
		return true
	}
	if r >= 0x2000 && r <= 0x200A {
		return true
	}
	switch r {
	case 0x2028, 0x2029, 0x202F, 0x205F, 0x3000:
		return true
	}
	return false
}
