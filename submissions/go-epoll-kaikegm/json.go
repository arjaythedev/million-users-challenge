package main

import (
	"strconv"
	"time"
)

const createdAtLayout = "2006-01-02T15:04:05.000Z"

func formatTime(t time.Time) string {
	return t.UTC().Format(createdAtLayout)
}

func appendJSONString(dst []byte, s string) []byte {
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '"':
			dst = append(dst, '\\', '"')
		case '\\':
			dst = append(dst, '\\', '\\')
		case '\n':
			dst = append(dst, '\\', 'n')
		case '\r':
			dst = append(dst, '\\', 'r')
		case '\t':
			dst = append(dst, '\\', 't')
		case '\b':
			dst = append(dst, '\\', 'b')
		case '\f':
			dst = append(dst, '\\', 'f')
		default:
			if c < 0x20 {
				const hex = "0123456789abcdef"
				dst = append(dst, '\\', 'u', '0', '0', hex[c>>4], hex[c&0xf])
			} else {
				dst = append(dst, c)
			}
		}
	}
	return dst
}

func formatHealthOK(dst []byte, uptimeS int64) []byte {
	dst = append(dst, `{"status":"ok","db":"ok","uptime_s":`...)
	dst = strconv.AppendInt(dst, uptimeS, 10)
	dst = append(dst, '}')
	return dst
}

func formatHealthError(dst []byte, errStr string) []byte {
	dst = append(dst, `{"status":"degraded","db":"unreachable","error":"`...)
	dst = appendJSONString(dst, errStr)
	dst = append(dst, `"}`...)
	return dst
}

func formatErrorJSON(dst []byte, msg string) []byte {
	dst = append(dst, `{"error":"`...)
	dst = appendJSONString(dst, msg)
	dst = append(dst, `"}`...)
	return dst
}

type Post struct {
	ID        int64
	Body      string
	CreatedAt string
	Author    string
	LikeCount int64
}

func formatPostObj(dst []byte, p Post) []byte {
	dst = append(dst, `{"id":`...)
	dst = strconv.AppendInt(dst, p.ID, 10)
	dst = append(dst, `,"body":"`...)
	dst = appendJSONString(dst, p.Body)
	dst = append(dst, `","created_at":"`...)
	dst = append(dst, p.CreatedAt...)
	dst = append(dst, `","author":"`...)
	dst = appendJSONString(dst, p.Author)
	dst = append(dst, `","like_count":`...)
	dst = strconv.AppendInt(dst, p.LikeCount, 10)
	dst = append(dst, '}')
	return dst
}

func formatGetPostJSON(dst []byte, p Post) []byte {
	dst = append(dst, `{"post":`...)
	dst = formatPostObj(dst, p)
	dst = append(dst, '}')
	return dst
}

func formatFeedJSON(dst []byte, posts []Post) []byte {
	dst = append(dst, `{"posts":[`...)
	for i, p := range posts {
		if i > 0 {
			dst = append(dst, ',')
		}
		dst = formatPostObj(dst, p)
	}
	dst = append(dst, `]}`...)
	return dst
}

func formatCreatePostJSON(dst []byte, p Post) []byte {
	dst = append(dst, `{"post":`...)
	dst = formatPostObj(dst, p)
	dst = append(dst, '}')
	return dst
}

func formatLikeJSON(dst []byte, alreadyLiked bool, postID int64) []byte {
	dst = append(dst, `{"liked":true,"already_liked":`...)
	if alreadyLiked {
		dst = append(dst, "true"...)
	} else {
		dst = append(dst, "false"...)
	}
	dst = append(dst, `,"post_id":`...)
	dst = strconv.AppendInt(dst, postID, 10)
	dst = append(dst, '}')
	return dst
}

func appendHTTPResponse(dst []byte, status int, body []byte, isGzip bool) []byte {
	dst = append(dst, "HTTP/1.1 "...)
	dst = strconv.AppendInt(dst, int64(status), 10)
	switch status {
	case 200:
		dst = append(dst, " OK\r\n"...)
	case 201:
		dst = append(dst, " Created\r\n"...)
	case 400:
		dst = append(dst, " Bad Request\r\n"...)
	case 401:
		dst = append(dst, " Unauthorized\r\n"...)
	case 404:
		dst = append(dst, " Not Found\r\n"...)
	case 500:
		dst = append(dst, " Internal Server Error\r\n"...)
	case 503:
		dst = append(dst, " Service Unavailable\r\n"...)
	default:
		dst = append(dst, "\r\n"...)
	}
	dst = append(dst, "Content-Type: application/json\r\nContent-Length: "...)
	dst = strconv.AppendInt(dst, int64(len(body)), 10)
	if isGzip {
		dst = append(dst, "\r\nContent-Encoding: gzip"...)
	}
	dst = append(dst, "\r\n\r\n"...)
	dst = append(dst, body...)
	return dst
}
