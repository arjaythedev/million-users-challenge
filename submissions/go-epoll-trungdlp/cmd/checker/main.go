// checker opens many keep-alive connections and checks that every response belongs to
// its request: GET /posts/:id must return that id, in order, including pipelined
// requests, requests split across writes, and connections closed mid-request.
package main

import (
	"bufio"
	"bytes"
	"flag"
	"fmt"
	"io"
	"math/rand/v2"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

var (
	addr                                     = flag.String("addr", "127.0.0.1:3000", "server")
	conns                                    = flag.Int("conns", 500, "connections")
	duration                                 = flag.Duration("d", 30*time.Second, "duration")
	checked, mismatches, errs, aborted, slow atomic.Int64
)

func id() int { return 1 + rand.IntN(500000) }

func req(i int, gz bool) string {
	h := ""
	if gz {
		h = "Accept-Encoding: gzip\r\n"
	}
	return fmt.Sprintf("GET /posts/%d HTTP/1.1\r\nHost: x\r\n%s\r\n", i, h)
}

func expect(r *bufio.Reader, want int) error {
	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		return err
	}
	body, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil {
		return err
	}
	needle := []byte(`{"post":{"id":` + strconv.Itoa(want) + `,`)
	checked.Add(1)
	if resp.StatusCode != 200 || !bytes.HasPrefix(body, needle) {
		mismatches.Add(1)
		if mismatches.Load() < 5 {
			fmt.Fprintf(os.Stderr, "MISMATCH want id %d: %d %.80s\n", want, resp.StatusCode, body)
		}
	}
	return nil
}

func worker(deadline time.Time) {
	for time.Now().Before(deadline) {
		c, err := net.Dial("tcp", *addr)
		if err != nil {
			errs.Add(1)
			time.Sleep(10 * time.Millisecond)
			continue
		}
		r := bufio.NewReader(c)
		for n := 0; n < 50 && time.Now().Before(deadline); n++ {
			c.SetDeadline(time.Now().Add(5 * time.Second))
			switch k := rand.IntN(10); {
			case k < 5: // one request
				i := id()
				if _, err = io.WriteString(c, req(i, false)); err == nil {
					err = expect(r, i)
				}
			case k < 7: // three pipelined requests in one write
				ids := []int{id(), id(), id()}
				_, err = io.WriteString(c, req(ids[0], false)+req(ids[1], false)+req(ids[2], false))
				for _, i := range ids {
					if err == nil {
						err = expect(r, i)
					}
				}
			case k < 9: // a request split across two writes
				i := id()
				s := req(i, false)
				cut := 1 + rand.IntN(len(s)-1)
				if _, err = io.WriteString(c, s[:cut]); err == nil {
					time.Sleep(time.Millisecond)
					if _, err = io.WriteString(c, s[cut:]); err == nil {
						err = expect(r, i)
					}
				}
			case k == 9 && rand.IntN(10) == 0: // a slow reader: the server's writes back up
				err = slowReader()
				n = 50
			default: // close while the statement is in flight
				_, _ = io.WriteString(c, req(id(), false)+req(id(), false))
				aborted.Add(1)
				n = 50
			}
			if err != nil {
				errs.Add(1)
				if errs.Load() < 5 {
					fmt.Fprintf(os.Stderr, "error: %v\n", err)
				}
				break
			}
		}
		c.Close()
	}
}

// slowReader pipelines 100 requests on a connection with a 4 KB receive buffer and reads
// the answers only after all were sent, so the server must queue unwritten output.
func slowReader() error {
	d := net.Dialer{Control: func(_, _ string, rc syscall.RawConn) error {
		return rc.Control(func(fd uintptr) { _ = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_RCVBUF, 4096) })
	}}
	c, err := d.Dial("tcp", *addr)
	if err != nil {
		return err
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(20 * time.Second))
	ids := make([]int, 100)
	var b bytes.Buffer
	for i := range ids {
		ids[i] = id()
		b.WriteString(req(ids[i], true))
	}
	if _, err := c.Write(b.Bytes()); err != nil {
		return err
	}
	time.Sleep(300 * time.Millisecond)
	r := bufio.NewReader(c)
	for _, i := range ids {
		if err := expect(r, i); err != nil {
			return err
		}
	}
	slow.Add(1)
	return nil
}

func main() {
	flag.Parse()
	deadline := time.Now().Add(*duration)
	var wg sync.WaitGroup
	for range *conns {
		wg.Add(1)
		go func() { defer wg.Done(); worker(deadline) }()
	}
	wg.Wait()
	fmt.Printf("checked=%d mismatches=%d errors=%d aborted=%d slow=%d\n", checked.Load(), mismatches.Load(), errs.Load(), aborted.Load(), slow.Load())
	if mismatches.Load() > 0 || errs.Load() > 0 {
		os.Exit(1)
	}
}
