package main

// jwtSecret is set once at startup, before the first request.
var jwtSecret []byte

const (
	idleTimeoutSecs = 120       // SPEC: keep idle connections open for at least 75 s
	readBufSize     = 64 << 10  // the loop's one socket read buffer
	stashLimit      = 32 << 10  // pipelined backlog that pauses reading a connection
	pgBufSize       = 256 << 10 // the loop's one Postgres read buffer
	epollBatch      = 512
	maxBatchReads   = 64 // reads sharing one Sync
)

// Shared vocabulary: request kinds and the parsed-request struct handed from
// the HTTP parser to the loop.

type reqKind uint8

const (
	rImmediate reqKind = iota // answer without the database (error paths)
	rHealth
	rFeed
	rGetPost
	rCreate
	rLike
)

// parsed is one parsed HTTP request. username and body point into loop-owned
// memory and are valid until the next parse.
type parsed struct {
	kind       reqKind
	status     int16  // rImmediate: the status to answer with
	reason     string // rImmediate: error body text
	id         int64  // post id for rGetPost / rLike
	uid        int64  // authenticated user id
	username   []byte // authenticated username
	body       []byte // create: trimmed body
	wantsGzip  bool
	closeAfter bool
}

// Immediate error bodies from the SPEC errors table.
var (
	errNotFound      = []byte(`{"error":"not found"}`)
	errInvalidID     = []byte(`{"error":"invalid post id"}`)
	errMissingBearer = []byte(`{"error":"missing bearer token"}`)
	errBadToken      = []byte(`{"error":"invalid or expired token"}`)
	errBadPayload    = []byte(`{"error":"invalid token payload"}`)
	errBadJSON       = []byte(`{"error":"malformed JSON body"}`)
	errBodyRequired  = []byte(`{"error":"body is required"}`)
	errBodyTooLong   = []byte(`{"error":"body must be at most 500 characters"}`)
	errInternal      = []byte(`{"error":"internal server error"}`)
	errPostNotFoundB = []byte(`{"error":"post not found"}`)
)
