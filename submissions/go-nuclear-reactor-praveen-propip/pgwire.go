package main

// Hand-rolled PostgreSQL frontend, connection setup half: startup with
// SCRAM-SHA-256 auth and the prepared statements, over a plain blocking
// socket. Once a connection is ready the event loop takes over its descriptor
// (pipe.go speaks the pipelined half of the protocol).

import (
	"crypto/hmac"
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"strconv"
	"strings"
	"time"
)

const (
	oidInt8        = 20
	oidText        = 25
	oidInt4        = 23
	oidTimestamptz = 1184
)

// The queries from SPEC.md, exactly; one execution per request. The feed's
// LIMIT is the literal 20: a parameter there would hide the limit from the
// planner, so the generic plan would never win and every execution would
// plan from scratch.
const (
	sqlPostSelect = `SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p
  JOIN users u ON u.id = p.user_id`

	sqlPost      = sqlPostSelect + " WHERE p.id = $1"
	sqlFeedQ     = sqlPostSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
	sqlInsPost   = `INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at`
	sqlInsLike   = `INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING`
	sqlSelectOne = `SELECT 1`
)

// Prepared statement names.
const (
	stmtFeed = "f"
	stmtPost = "p"
	stmtIns  = "c"
	stmtLike = "l"
	stmtOne  = "h"
)

var preparedStatements = []struct {
	name, sql string
	params    []uint32
	results   []uint32 // nil: not checked (no result columns)
}{
	{stmtFeed, sqlFeedQ, nil, feedOIDs},
	{stmtPost, sqlPost, []uint32{oidInt8}, feedOIDs},
	{stmtIns, sqlInsPost, []uint32{oidInt8, oidText}, insPostOIDs},
	{stmtLike, sqlInsLike, []uint32{oidInt8, oidInt8}, nil},
	{stmtOne, sqlSelectOne, nil, oneOIDs},
}

var feedOIDs = []uint32{oidInt8, oidText, oidTimestamptz, oidText, oidInt4}
var insPostOIDs = []uint32{oidInt8, oidTimestamptz}
var oneOIDs = []uint32{oidInt4}

type pgDSN struct {
	host, port, user, password, database string
}

// parseDSN parses postgres://user:pass@host:port/db[?params], percent-decoding
// the credentials and database name.
func parseDSN(s string) (*pgDSN, error) {
	u, err := url.Parse(s)
	if err != nil {
		return nil, errors.New("DATABASE_URL does not parse")
	}
	if u.Scheme != "postgres" && u.Scheme != "postgresql" {
		return nil, errors.New("DATABASE_URL must be postgres://user:pass@host:port/db")
	}
	d := &pgDSN{host: u.Hostname(), port: u.Port(), database: strings.TrimPrefix(u.Path, "/")}
	if d.port == "" {
		d.port = "5432"
	}
	if u.User != nil {
		d.user = u.User.Username()
		d.password, _ = u.User.Password()
	}
	if d.host == "" || d.database == "" {
		return nil, errors.New("DATABASE_URL must include host and database")
	}
	return d, nil
}

// pgSetup is a connection between dial and hand-over to the event loop.
type pgSetup struct {
	conn net.Conn
	msg  []byte
}

// dialPostgres connects, authenticates and prepares the statements.
func dialPostgres(dsn *pgDSN) (net.Conn, error) {
	conn, err := net.DialTimeout("tcp", net.JoinHostPort(dsn.host, dsn.port), 3*time.Second)
	if err != nil {
		return nil, err
	}
	_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
	c := &pgSetup{conn: conn}
	err = c.startup(dsn)
	if err == nil {
		err = c.prepareAll()
	}
	if err != nil {
		conn.Close()
		return nil, err
	}
	_ = conn.SetDeadline(time.Time{})
	return conn, nil
}

// receive reads one backend message; the payload is valid until the next call.
func (c *pgSetup) receive() (byte, []byte, error) {
	var hdr [5]byte
	if _, err := io.ReadFull(c.conn, hdr[:]); err != nil {
		return 0, nil, err
	}
	n := int(binary.BigEndian.Uint32(hdr[1:])) - 4
	if n < 0 || n > 1<<24 {
		return 0, nil, errors.New("bad message length")
	}
	if cap(c.msg) < n {
		c.msg = make([]byte, n)
	}
	c.msg = c.msg[:n]
	if _, err := io.ReadFull(c.conn, c.msg); err != nil {
		return 0, nil, err
	}
	return hdr[0], c.msg, nil
}

// frontendMsg frames one frontend message.
func frontendMsg(typ byte, payload []byte) []byte {
	m := make([]byte, 5, 5+len(payload))
	m[0] = typ
	binary.BigEndian.PutUint32(m[1:], uint32(4+len(payload)))
	return append(m, payload...)
}

func (c *pgSetup) startup(dsn *pgDSN) error {
	p := binary.BigEndian.AppendUint32(nil, 196608)
	p = append(p, "user\x00"+dsn.user+"\x00database\x00"+dsn.database+"\x00\x00"...)
	m := binary.BigEndian.AppendUint32(nil, uint32(4+len(p)))
	if _, err := c.conn.Write(append(m, p...)); err != nil {
		return err
	}
	for {
		typ, msg, err := c.receive()
		if err != nil {
			return err
		}
		switch typ {
		case 'R':
			if len(msg) < 4 {
				return errors.New("short auth message")
			}
			switch code := binary.BigEndian.Uint32(msg); code {
			case 0: // AuthenticationOk
			case 10: // AuthenticationSASL
				if err := c.scram(string(msg[4:]), dsn.password); err != nil {
					return err
				}
			default:
				return fmt.Errorf("unsupported auth method %d", code)
			}
		case 'E':
			return pgErrorOf(msg)
		case 'Z':
			return nil
		case 'S', 'K', 'N': // ParameterStatus, BackendKeyData, Notice
		default:
			return fmt.Errorf("unexpected message %q during startup", typ)
		}
	}
}

// saslMsg reads the next SASL step, whose auth code must be want.
func (c *pgSetup) saslMsg(want uint32) ([]byte, error) {
	typ, msg, err := c.receive()
	if err != nil {
		return nil, err
	}
	if typ == 'E' {
		return nil, pgErrorOf(msg)
	}
	if typ != 'R' || len(msg) < 4 || binary.BigEndian.Uint32(msg) != want {
		return nil, fmt.Errorf("unexpected SASL message %q", typ)
	}
	return msg[4:], nil
}

// scram performs SCRAM-SHA-256 (RFC 5802/7677) without channel binding.
func (c *pgSetup) scram(mechanisms, password string) error {
	if !strings.Contains(mechanisms, "SCRAM-SHA-256\x00") {
		return errors.New("server does not offer SCRAM-SHA-256")
	}
	nonce := make([]byte, 18)
	if _, err := rand.Read(nonce); err != nil {
		return err
	}
	clientNonce := base64.StdEncoding.EncodeToString(nonce)
	clientFirstBare := "n=,r=" + clientNonce
	first := "n,," + clientFirstBare
	p := append([]byte("SCRAM-SHA-256\x00"), 0, 0, 0, 0)
	binary.BigEndian.PutUint32(p[len(p)-4:], uint32(len(first)))
	if _, err := c.conn.Write(frontendMsg('p', append(p, first...))); err != nil {
		return err
	}
	msg, err := c.saslMsg(11)
	if err != nil {
		return err
	}
	serverFirst := string(msg)
	serverNonce, salt, iters, err := parseServerFirst(serverFirst, clientNonce)
	if err != nil {
		return err
	}

	clientFinalNoProof := "c=biws,r=" + serverNonce
	proof, salted := scramProof(clientFirstBare, serverFirst, clientFinalNoProof, password, salt, iters)
	clientFinal := clientFinalNoProof + ",p=" + base64.StdEncoding.EncodeToString(proof)
	if _, err := c.conn.Write(frontendMsg('p', []byte(clientFinal))); err != nil {
		return err
	}
	if msg, err = c.saslMsg(12); err != nil {
		return err
	}
	v, err := base64.StdEncoding.DecodeString(strings.TrimPrefix(string(msg), "v="))
	if err != nil {
		return errors.New("bad SASLFinal payload")
	}
	serverKey := hmacSha256(salted, []byte("Server Key"))
	serverSig := hmacSha256(serverKey, []byte(clientFirstBare+","+serverFirst+","+clientFinalNoProof))
	if !hmac.Equal(v, serverSig) {
		return errors.New("SCRAM server signature mismatch")
	}
	return nil
}

// scramProof computes the SCRAM-SHA-256 client proof and salted password for
// the given exchange parts (RFC 5802): PBKDF2 key, client key XOR client
// signature over the auth message.
func scramProof(clientFirstBare, serverFirst, clientFinalNoProof, password string, salt []byte, iters int) ([]byte, []byte) {
	salted, err := pbkdf2.Key(sha256.New, password, salt, iters, 32)
	if err != nil {
		return nil, nil
	}
	clientKey := hmacSha256(salted, []byte("Client Key"))
	storedKey := sha256.Sum256(clientKey)
	authMessage := []byte(clientFirstBare + "," + serverFirst + "," + clientFinalNoProof)
	clientSig := hmacSha256(storedKey[:], authMessage)
	proof := make([]byte, 32)
	for i := range clientKey {
		proof[i] = clientKey[i] ^ clientSig[i]
	}
	return proof, salted
}

func parseServerFirst(s, clientNonce string) (nonce string, salt []byte, iters int, err error) {
	for _, part := range strings.Split(s, ",") {
		if v, ok := strings.CutPrefix(part, "r="); ok {
			nonce = v
		} else if v, ok := strings.CutPrefix(part, "s="); ok {
			salt, err = base64.StdEncoding.DecodeString(v)
			if err != nil {
				return "", nil, 0, errors.New("bad salt")
			}
		} else if v, ok := strings.CutPrefix(part, "i="); ok {
			iters, err = strconv.Atoi(v)
			if err != nil {
				return "", nil, 0, errors.New("bad iterations")
			}
		}
	}
	if !strings.HasPrefix(nonce, clientNonce) || nonce == "" || salt == nil || iters <= 0 {
		return "", nil, 0, errors.New("bad server-first-message")
	}
	return nonce, salt, iters, nil
}

func hmacSha256(key, msg []byte) []byte {
	m := hmac.New(sha256.New, key)
	m.Write(msg)
	return m.Sum(nil)
}

// prepareAll parses every statement and sanity-checks its result types once
// per connection.
func (c *pgSetup) prepareAll() error {
	for _, st := range preparedStatements {
		p := append([]byte(st.name), 0)
		p = append(p, st.sql...)
		p = append(p, 0)
		p = binary.BigEndian.AppendUint16(p, uint16(len(st.params)))
		for _, oid := range st.params {
			p = binary.BigEndian.AppendUint32(p, oid)
		}
		m := frontendMsg('P', p)
		m = append(m, frontendMsg('D', append([]byte{'S'}, st.name+"\x00"...))...)
		m = append(m, syncMsg...)
		if _, err := c.conn.Write(m); err != nil {
			return err
		}
		var got []uint32
		for done := false; !done; {
			typ, msg, err := c.receive()
			if err != nil {
				return err
			}
			switch typ {
			case 'E':
				return pgErrorOf(msg)
			case 'T':
				got = rowDescriptionOIDs(msg)
			case 'Z':
				done = true
			case '1', 't', 'n', 'N', 'S':
			default:
				return fmt.Errorf("unexpected message %q during prepare", typ)
			}
		}
		if st.results != nil && !equalOIDs(got, st.results) {
			return fmt.Errorf("statement %s: unexpected result types %v", st.name, got)
		}
	}
	return nil
}

func rowDescriptionOIDs(msg []byte) []uint32 {
	if len(msg) < 2 {
		return nil
	}
	n := int(binary.BigEndian.Uint16(msg))
	msg = msg[2:]
	oids := make([]uint32, 0, n)
	for i := 0; i < n; i++ {
		z := strings.IndexByte(string(msg), 0)
		if z < 0 || len(msg) < z+19 {
			return nil
		}
		oids = append(oids, binary.BigEndian.Uint32(msg[z+7:]))
		msg = msg[z+19:]
	}
	return oids
}

func equalOIDs(a, b []uint32) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// pgError is a backend ErrorResponse.
type pgError struct {
	code    string
	message string
}

func (e *pgError) Error() string { return e.message + " (" + e.code + ")" }

const pgForeignKeyViolation = "23503"

// pgErrorOf parses the (type byte, C string) fields of an ErrorResponse.
func pgErrorOf(msg []byte) *pgError {
	e := &pgError{}
	for len(msg) > 1 {
		typ := msg[0]
		z := strings.IndexByte(string(msg[1:]), 0)
		if z < 0 {
			break
		}
		switch typ {
		case 'C':
			e.code = string(msg[1 : 1+z])
		case 'M':
			e.message = string(msg[1 : 1+z])
		}
		msg = msg[z+2:]
	}
	return e
}

// commandTagRows parses the trailing row count from tags like "INSERT 0 1".
func commandTagRows(tag []byte) int64 {
	var v int64
	i := len(tag) - 1
	mult := int64(1)
	for i >= 0 && tag[i] >= '0' && tag[i] <= '9' {
		v += int64(tag[i]-'0') * mult
		mult *= 10
		i--
	}
	return v
}
