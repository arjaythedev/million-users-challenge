//go:build !linux

package main

import "log"

func main() { log.Fatal("this server needs Linux (epoll)") }
