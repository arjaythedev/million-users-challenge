package main

import (
	"bufio"
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"sync"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

// These tests run a real event loop against scripted peers: socket pairs
// stand in for the clients and for PostgreSQL, so every byte the loop sends to
// the database and every reply it parses is under the test's control.

type testLoop struct {
	t       *testing.T
	loop    *loop
	clients []*os.File
	// database holds the far end of each session's socket, by session index.
	database []*os.File
	// spares are handed out, in order, to sessions that redial.
	spares chan int
}

// socketPair returns a non-blocking descriptor for the loop and a blocking
// file, with a read timeout, for the test.
func socketPair(t *testing.T) (int, *os.File) {
	t.Helper()
	fds, err := unix.Socketpair(unix.AF_UNIX, unix.SOCK_STREAM|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	if err := unix.SetNonblock(fds[0], true); err != nil {
		t.Fatal(err)
	}
	if err := unix.SetsockoptTimeval(fds[1], unix.SOL_SOCKET, unix.SO_RCVTIMEO, &unix.Timeval{Sec: 5}); err != nil {
		t.Fatal(err)
	}
	far := os.NewFile(uintptr(fds[1]), "peer")
	t.Cleanup(func() { _ = far.Close() })
	return fds[0], far
}

func startLoop(t *testing.T, readers, writers, clients int) *testLoop {
	t.Helper()
	stop := make(chan struct{})
	h := &testLoop{t: t, spares: make(chan int, 8)}
	app := &application{
		secret:  []byte("test-secret"),
		started: time.Now(),
		stop:    stop,
		plan:    sessionPlan{readers: readers, writers: writers, batch: 64},
		dial: func() (int, error) {
			select {
			case fd := <-h.spares:
				return fd, nil
			default:
				return -1, errors.New("test database is down")
			}
		},
	}
	l, err := newLoop(0, app)
	if err != nil {
		t.Fatal(err)
	}
	h.loop = l
	l.sessions = app.plan.sessions(l)
	for i, s := range l.sessions {
		near, far := socketPair(t)
		s.dialed(dialResult{index: i, fd: near})
		h.database = append(h.database, far)
	}
	for i := 0; i < clients; i++ {
		near, far := socketPair(t)
		l.adopt(int32(near))
		h.clients = append(h.clients, far)
	}
	var running sync.WaitGroup
	running.Add(1)
	go l.run(&running)
	t.Cleanup(func() {
		close(stop)
		app.stopping.Store(true)
		l.wake()
		running.Wait()
	})
	return h
}

func (h *testLoop) request(client int, text string) {
	h.t.Helper()
	if _, err := io.WriteString(h.clients[client], text); err != nil {
		h.t.Fatal(err)
	}
}

// response reads one HTTP response and returns its status and body.
func (h *testLoop) response(client int) (int, string) {
	h.t.Helper()
	response, err := http.ReadResponse(bufio.NewReader(h.clients[client]), nil)
	if err != nil {
		h.t.Fatalf("client %d: %v", client, err)
	}
	body, err := io.ReadAll(response.Body)
	if err != nil {
		h.t.Fatalf("client %d: %v", client, err)
	}
	return response.StatusCode, string(body)
}

// expect reads exactly the bytes of want from a session's socket.
func (h *testLoop) expect(peer *os.File, what string, want []byte) {
	h.t.Helper()
	got := make([]byte, len(want))
	if _, err := io.ReadFull(peer, got); err != nil {
		h.t.Fatalf("%s: %v", what, err)
	}
	if !bytes.Equal(got, want) {
		h.t.Fatalf("%s:\n got %q\nwant %q", what, got, want)
	}
}

// reply writes backend messages in pieces of at most step bytes, so message
// boundaries fall anywhere in the loop's reads.
func (h *testLoop) reply(peer *os.File, step int, messages ...[]byte) {
	h.t.Helper()
	stream := bytes.Join(messages, nil)
	for len(stream) > 0 {
		n := min(step, len(stream))
		if _, err := peer.Write(stream[:n]); err != nil {
			h.t.Fatal(err)
		}
		stream = stream[n:]
	}
}

func backend(kind byte, body []byte) []byte {
	message := append([]byte{kind, 0, 0, 0, 0}, body...)
	binary.BigEndian.PutUint32(message[1:], uint32(len(body)+4))
	return message
}

func dataRow(columns ...[]byte) []byte {
	body := binary.BigEndian.AppendUint16(nil, uint16(len(columns)))
	for _, column := range columns {
		body = binary.BigEndian.AppendUint32(body, uint32(len(column)))
		body = append(body, column...)
	}
	return backend('D', body)
}

func timestamp(micros int64) []byte {
	return binary.BigEndian.AppendUint64(nil, uint64(micros-postgresEpochMicros))
}

func failed(code, constraint string) []byte {
	return backend('E', []byte("SERROR\x00C"+code+"\x00Mtest failure\x00n"+constraint+"\x00\x00"))
}

func complete(tag string) []byte { return backend('C', []byte(tag+"\x00")) }

var (
	bound = backend('2', nil)
	ready = backend('Z', []byte{'I'})
)

func batchOf(queries ...[]byte) []byte {
	return append(bytes.Join(queries, nil), syncMessage...)
}

func bearer(secret []byte, user int, name string) string {
	encode := base64.RawURLEncoding.EncodeToString
	signed := encode([]byte(`{"alg":"HS256","typ":"JWT"}`)) + "." +
		encode([]byte(fmt.Sprintf(`{"sub":"%d","username":%q,"exp":%d}`, user, name, time.Now().Unix()+3600)))
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(signed))
	return "Bearer " + signed + "." + encode(mac.Sum(nil))
}

// createdAt is 2026-01-02T03:04:05.123456Z in Unix microseconds.
const createdAt = 1767323045123456

func TestReadsAreAnsweredFromFragmentedReplies(t *testing.T) {
	h := startLoop(t, 1, 1, 3)
	db := h.database[0]
	h.request(0, "GET /feed HTTP/1.1\r\nHost: test\r\n\r\n")
	h.expect(db, "feed batch", batchOf(feedMessage))
	// These arrive while the session is busy and leave together when it is
	// free. The client of the second one goes away before its reply.
	h.request(1, "GET /posts/7 HTTP/1.1\r\nHost: test\r\n\r\n")
	h.request(2, "GET /posts/7 HTTP/1.1\r\nHost: test\r\n\r\n")
	time.Sleep(50 * time.Millisecond)
	_ = h.clients[2].Close()
	h.reply(db, 1, bound,
		dataRow([]byte("12"), []byte("line\none \"quoted\""), timestamp(createdAt), []byte("ada"), []byte("3")),
		dataRow([]byte("11"), []byte("second"), timestamp(createdAt-1000000), []byte("bo"), []byte("0")),
		complete("SELECT 2"), ready)
	status, body := h.response(0)
	want := `{"posts":[{"id":12,"body":"line\none \"quoted\"","created_at":"2026-01-02T03:04:05.123Z","author":"ada","like_count":3},` +
		`{"id":11,"body":"second","created_at":"2026-01-02T03:04:04.123Z","author":"bo","like_count":0}]}`
	if status != 200 || body != want {
		t.Fatalf("feed: %d %s", status, body)
	}
	h.expect(db, "post batch", batchOf(appendPostQuery(nil, 7), appendPostQuery(nil, 7)))
	h.reply(db, 3, bound, complete("SELECT 0"), bound, complete("SELECT 0"), ready)
	if status, body := h.response(1); status != 404 || body != string(bodyPostNotFound) {
		t.Fatalf("missing post: %d %s", status, body)
	}
	// The loop is still serving after answering a connection that closed.
	h.request(0, "GET /health HTTP/1.1\r\nHost: test\r\n\r\n")
	h.expect(db, "health batch", batchOf(healthMessage))
	h.reply(db, 64, bound, dataRow([]byte("1")), complete("SELECT 1"), ready)
	if status, body := h.response(0); status != 200 || !bytes.HasPrefix([]byte(body), []byte(`{"status":"ok","db":"ok","uptime_s":`)) {
		t.Fatalf("health: %d %s", status, body)
	}
}

func TestFailedReadFailsAloneAndTheRestRunAgain(t *testing.T) {
	h := startLoop(t, 1, 1, 3)
	db := h.database[0]
	h.request(0, "GET /feed HTTP/1.1\r\nHost: test\r\n\r\n")
	h.expect(db, "feed batch", batchOf(feedMessage))
	h.request(1, "GET /posts/5 HTTP/1.1\r\nHost: test\r\n\r\n")
	h.request(2, "GET /posts/5 HTTP/1.1\r\nHost: test\r\n\r\n")
	time.Sleep(50 * time.Millisecond)
	h.reply(db, 7, bound, complete("SELECT 0"), ready)
	if status, body := h.response(0); status != 200 || body != `{"posts":[]}` {
		t.Fatalf("empty feed: %d %s", status, body)
	}
	one := appendPostQuery(nil, 5)
	h.expect(db, "batch of two", batchOf(one, one))
	// The first query fails; the server skips the second up to the Sync.
	h.reply(db, 5, bound, failed("XX000", ""), ready)
	h.expect(db, "skipped query sent again", batchOf(one))
	h.reply(db, 9, bound,
		dataRow([]byte("5"), []byte("five"), timestamp(createdAt), []byte("ada"), []byte("1")),
		complete("SELECT 1"), ready)
	statusA, bodyA := h.response(1)
	statusB, bodyB := h.response(2)
	if statusA > statusB {
		statusA, bodyA, statusB, bodyB = statusB, bodyB, statusA, bodyA
	}
	if statusA != 200 || bodyA != `{"post":{"id":5,"body":"five","created_at":"2026-01-02T03:04:05.123Z","author":"ada","like_count":1}}` {
		t.Fatalf("read that ran again: %d %s", statusA, bodyA)
	}
	if statusB != 500 || bodyB != string(bodyInternal) {
		t.Fatalf("failed read: %d %s", statusB, bodyB)
	}
}

func TestFailedGroupRunsEachWriteAlone(t *testing.T) {
	h := startLoop(t, 1, 1, 3)
	db := h.database[1]
	token := bearer(h.loop.app.secret, 42, "ada")
	post := func(path, body string) string {
		return fmt.Sprintf("POST %s HTTP/1.1\r\nHost: test\r\nAuthorization: %s\r\nContent-Length: %d\r\n\r\n%s", path, token, len(body), body)
	}
	h.request(0, post("/posts/1/like", ""))
	h.expect(db, "first group", batchOf(appendLikeQuery(nil, 42, 1)))
	h.request(1, post("/posts/999/like", ""))
	h.request(2, post("/posts", `{"body":" hello "}`))
	time.Sleep(50 * time.Millisecond)
	h.reply(db, 2, bound, complete("INSERT 0 1"), ready)
	if status, body := h.response(0); status != 201 || body != `{"liked":true,"already_liked":false,"post_id":1}` {
		t.Fatalf("like: %d %s", status, body)
	}
	// A group is one implicit transaction: creates first, likes in key order.
	create, like := appendCreateQuery(nil, 42, "hello"), appendLikeQuery(nil, 42, 999)
	h.expect(db, "group of two", batchOf(create, like))
	row := dataRow([]byte("77"), timestamp(createdAt))
	h.reply(db, 4, bound, row, complete("INSERT 0 1"), bound, failed("23503", "likes_post_id_fkey"), ready)
	// Nothing committed, so each write runs again as its own transaction.
	h.expect(db, "writes alone", append(batchOf(create), batchOf(like)...))
	h.reply(db, 6, bound, row, complete("INSERT 0 1"), ready, bound, failed("23503", "likes_post_id_fkey"), ready)
	if status, body := h.response(2); status != 201 || body != `{"post":{"id":77,"body":"hello","created_at":"2026-01-02T03:04:05.123Z","author":"ada","like_count":0}}` {
		t.Fatalf("create: %d %s", status, body)
	}
	if status, body := h.response(1); status != 404 || body != string(bodyPostNotFound) {
		t.Fatalf("like of a missing post: %d %s", status, body)
	}
	// A like that is already there inserts no row.
	h.request(0, post("/posts/1/like", ""))
	h.expect(db, "repeated like", batchOf(appendLikeQuery(nil, 42, 1)))
	h.reply(db, 64, bound, complete("INSERT 0 0"), ready)
	if status, body := h.response(0); status != 200 || body != `{"liked":true,"already_liked":true,"post_id":1}` {
		t.Fatalf("repeated like: %d %s", status, body)
	}
}

func TestLostSessionRunsReadsAgainButNeverWrites(t *testing.T) {
	h := startLoop(t, 1, 0, 1)
	replacement, db := socketPair(t)
	h.spares <- replacement
	h.request(0, "GET /feed HTTP/1.1\r\nHost: test\r\n\r\n")
	h.expect(h.database[0], "feed batch", batchOf(feedMessage))
	_ = h.database[0].Close()
	// The read goes out again on the session that replaces the lost one.
	h.expect(db, "feed batch on the new session", batchOf(feedMessage))
	h.reply(db, 64, bound, complete("SELECT 0"), ready)
	if status, body := h.response(0); status != 200 || body != `{"posts":[]}` {
		t.Fatalf("feed after reconnect: %d %s", status, body)
	}
	// A write whose session is lost may have committed: it fails and is not
	// sent again, although a new session is available.
	second, db2 := socketPair(t)
	h.spares <- second
	token := bearer(h.loop.app.secret, 42, "ada")
	h.request(0, "POST /posts/3/like HTTP/1.1\r\nHost: test\r\nAuthorization: "+token+"\r\nContent-Length: 0\r\n\r\n")
	h.expect(db, "like", batchOf(appendLikeQuery(nil, 42, 3)))
	_ = db.Close()
	if status, body := h.response(0); status != 500 || body != string(bodyInternal) {
		t.Fatalf("write on a lost session: %d %s", status, body)
	}
	h.request(0, "GET /posts/3 HTTP/1.1\r\nHost: test\r\n\r\n")
	h.expect(db2, "next request on the third session", batchOf(appendPostQuery(nil, 3)))
	h.reply(db2, 64, bound, complete("SELECT 0"), ready)
	if status, _ := h.response(0); status != 404 {
		t.Fatalf("read after a failed write: %d", status)
	}
	// With the database gone, requests fail at once instead of waiting.
	_ = db2.Close()
	time.Sleep(50 * time.Millisecond)
	h.request(0, "GET /health HTTP/1.1\r\nHost: test\r\n\r\n")
	if status, body := h.response(0); status != 503 || !bytes.Contains([]byte(body), []byte(`"db":"unreachable"`)) {
		t.Fatalf("health without a database: %d %s", status, body)
	}
}
