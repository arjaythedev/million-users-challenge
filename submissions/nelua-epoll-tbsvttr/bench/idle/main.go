// idle N HOST:PORT: opens N keep-alive connections, sends GET /health on each and reads the answer, then holds
// them open without reading (no goroutine or buffer per connection) until killed.
package main

import (
	"bytes"
	"fmt"
	"net"
	"os"
	"strconv"
	"time"
)

// held keeps the connections reachable: a net.Conn the program no longer refers to is closed by its finalizer.
var held []net.Conn

func main() {
	n, _ := strconv.Atoi(os.Args[1])
	addr := os.Args[2]
	host, _, _ := net.SplitHostPort(addr)
	req := []byte("GET /health HTTP/1.1\r\nHost: " + host + "\r\nAccept-Encoding: gzip\r\n\r\n")
	buf := make([]byte, 4096)
	held = make([]net.Conn, 0, n)
	failed := 0
	for i := 0; i < n; i++ {
		c, err := net.Dial("tcp", addr)
		if err != nil {
			failed++
			continue
		}
		c.Write(req)
		// one read is enough for the small /health answer; wait until its body is in
		got := 0
		for got < len(buf) {
			k, err := c.Read(buf[got:])
			got += k
			if err != nil || bytes.Contains(buf[:got], []byte("}")) {
				break
			}
		}
		held = append(held, c)
		if (i+1)%5000 == 0 {
			fmt.Println(time.Now().Format("15:04:05"), addr, "connected", len(held))
		}
	}
	fmt.Println(time.Now().Format("15:04:05"), addr, "holding", len(held), "failed", failed)
	select {}
}
