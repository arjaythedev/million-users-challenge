package main

import (
	"log"
	"net"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"

	"golang.org/x/sys/unix"
)

// hasPartial is a lock-free fast-path flag indexed by fd. Only touched when a
// request actually fragments across reads — 99.999% of requests skip both the
// atomic load and the mutex entirely.
// 2M entries x 4B = 8 MB in BSS; no GC pointers, no heap.
var hasPartial [2097152]uint32

var (
	partialMu   sync.Mutex
	partialData = make(map[int][]byte)
)

func savePartial(fd int, b []byte) {
	partialMu.Lock()
	cur := partialData[fd]
	cur = append(cur[:0], b...)
	partialData[fd] = cur
	partialMu.Unlock()
	atomic.StoreUint32(&hasPartial[fd], 1)
}

func getPartial(fd int) []byte {
	if atomic.LoadUint32(&hasPartial[fd]) == 0 {
		return nil
	}
	partialMu.Lock()
	b := partialData[fd]
	delete(partialData, fd)
	partialMu.Unlock()
	atomic.StoreUint32(&hasPartial[fd], 0)
	return b
}

// clearConn must be called BEFORE unix.Close(fd) — otherwise fd can be recycled
// by the next accept4() and the call would wipe the new connection's state.
func clearConn(fd int) {
	if atomic.LoadUint32(&hasPartial[fd]) == 0 {
		return
	}
	partialMu.Lock()
	delete(partialData, fd)
	partialMu.Unlock()
	atomic.StoreUint32(&hasPartial[fd], 0)
}

func createListener(host string, port string) (int, error) {
	portNum, err := strconv.Atoi(port)
	if err != nil {
		return -1, err
	}
	fd, err := unix.Socket(unix.AF_INET, unix.SOCK_STREAM|unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		return -1, err
	}

	if err := unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEADDR, 1); err != nil {
		unix.Close(fd)
		return -1, err
	}
	if err := unix.SetsockoptInt(fd, unix.SOL_SOCKET, unix.SO_REUSEPORT, 1); err != nil {
		unix.Close(fd)
		return -1, err
	}
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_DEFER_ACCEPT, 1)
	_ = unix.SetsockoptInt(fd, unix.IPPROTO_TCP, unix.TCP_NODELAY, 1)

	var addr [4]byte
	ip := net.ParseIP(host)
	if ip != nil {
		if ip4 := ip.To4(); ip4 != nil {
			copy(addr[:], ip4)
		}
	}
	sa := &unix.SockaddrInet4{Port: portNum, Addr: addr}
	if err := unix.Bind(fd, sa); err != nil {
		unix.Close(fd)
		return -1, err
	}
	if err := unix.Listen(fd, 65535); err != nil {
		unix.Close(fd)
		return -1, err
	}
	return fd, nil
}

type Worker struct {
	id          int
	epollFd     int
	listener1   int
	listener2   int
	dbPool      *DBPool
	authChecker *AuthChecker
}

func (w *Worker) run() {
	runtime.LockOSThread()
	events := make([]unix.EpollEvent, 1024)
	scratch := make([]byte, 16384)
	errBuf := make([]byte, 0, 1024)

	for {
		n, err := unix.EpollWait(w.epollFd, events, -1)
		if err != nil {
			if err == unix.EINTR {
				continue
			}
			log.Printf("[worker %d] epoll_wait: %v", w.id, err)
			continue
		}

		for i := 0; i < n; i++ {
			ev := &events[i]
			fd := int(ev.Fd)

			if fd == w.listener1 || (w.listener2 > 0 && fd == w.listener2) {
				for {
					nfd, _, err := unix.Accept4(fd, unix.SOCK_NONBLOCK|unix.SOCK_CLOEXEC)
					if err != nil {
						break
					}
					if nfd >= len(hasPartial) {
						log.Printf("[worker %d] fd %d exceeds hasPartial array, closing", w.id, nfd)
						unix.Close(nfd)
						continue
					}

					eev := unix.EpollEvent{
						Events: unix.EPOLLIN | unix.EPOLLET | unix.EPOLLRDHUP,
						Fd:     int32(nfd),
					}
					_ = unix.EpollCtl(w.epollFd, unix.EPOLL_CTL_ADD, nfd, &eev)
				}
				continue
			}

			if ev.Events&(unix.EPOLLRDHUP|unix.EPOLLHUP|unix.EPOLLERR) != 0 {
				_ = unix.EpollCtl(w.epollFd, unix.EPOLL_CTL_DEL, fd, nil)
				clearConn(fd) // BEFORE unix.Close
				unix.Close(fd)
				continue
			}

			if ev.Events&unix.EPOLLIN != 0 {
				w.handleRead(fd, scratch, errBuf)
			}
		}
	}
}

func (w *Worker) handleRead(fd int, scratch []byte, errBuf []byte) {
	var buf []byte
	if existing := getPartial(fd); len(existing) > 0 {
		buf = existing
	}

	for {
		n, err := unix.Read(fd, scratch)
		if err != nil {
			if err == unix.EAGAIN || err == unix.EWOULDBLOCK {
				break
			}
			_ = unix.EpollCtl(w.epollFd, unix.EPOLL_CTL_DEL, fd, nil)
			clearConn(fd)
			unix.Close(fd)
			return
		}
		if n == 0 {
			_ = unix.EpollCtl(w.epollFd, unix.EPOLL_CTL_DEL, fd, nil)
			clearConn(fd)
			unix.Close(fd)
			return
		}
		buf = append(buf, scratch[:n]...)
	}

	if len(buf) == 0 {
		return
	}

	offset := 0
	for offset < len(buf) {
		req, err := parseHTTPRequest(buf[offset:])
		if err != nil {
			sendSyncError(fd, 400, "bad request", errBuf)
			_ = unix.EpollCtl(w.epollFd, unix.EPOLL_CTL_DEL, fd, nil)
			clearConn(fd)
			unix.Close(fd)
			return
		}
		if !req.Complete {
			break
		}

		w.dispatch(fd, req, errBuf)

		offset += req.HeaderLen + req.ContentLength
		if !req.KeepAlive {
			_ = unix.EpollCtl(w.epollFd, unix.EPOLL_CTL_DEL, fd, nil)
			clearConn(fd)
			unix.Close(fd)
			return
		}
	}

	if offset < len(buf) {
		savePartial(fd, buf[offset:])
	}
}

func (w *Worker) dispatch(fd int, req ParsedRequest, errBuf []byte) {
	if req.Path == "/health" {
		if req.Method != "GET" {
			sendSyncError(fd, 404, "not found", errBuf)
			return
		}
		w.dbPool.Submit(dbJob{
			fd:         int32(fd),
			jobType:    JobHealth,
			closeAfter: !req.KeepAlive,
		})
		return
	}

	if req.Path == "/feed" {
		if req.Method != "GET" {
			sendSyncError(fd, 404, "not found", errBuf)
			return
		}
		w.dbPool.Submit(dbJob{
			fd:         int32(fd),
			jobType:    JobFeed,
			gzip:       req.GzipAccepted,
			closeAfter: !req.KeepAlive,
		})
		return
	}

	if req.Path == "/posts" {
		if req.Method != "POST" {
			sendSyncError(fd, 404, "not found", errBuf)
			return
		}
		authRes := w.authChecker.Check(req.AuthHeader)
		if authRes.Status != 0 {
			sendSyncError(fd, authRes.Status, authRes.ErrorMsg, errBuf)
			return
		}
		body, code, msg := parsePostBody(req.Body)
		if code != 0 {
			sendSyncError(fd, code, msg, errBuf)
			return
		}
		w.dbPool.Submit(dbJob{
			fd:         int32(fd),
			jobType:    JobCreatePost,
			userID:     authRes.UserID,
			username:   authRes.Username,
			body:       body,
			closeAfter: !req.KeepAlive,
		})
		return
	}

	if strings.HasPrefix(req.Path, "/posts/") {
		sub := req.Path[len("/posts/"):]
		if strings.HasSuffix(sub, "/like") {
			if req.Method != "POST" {
				sendSyncError(fd, 404, "not found", errBuf)
				return
			}
			authRes := w.authChecker.Check(req.AuthHeader)
			if authRes.Status != 0 {
				sendSyncError(fd, authRes.Status, authRes.ErrorMsg, errBuf)
				return
			}
			idStr := sub[:len(sub)-len("/like")]
			postID, ok := parseID(idStr)
			if !ok {
				sendSyncError(fd, 400, "invalid post id", errBuf)
				return
			}
			w.dbPool.Submit(dbJob{
				fd:         int32(fd),
				jobType:    JobLikePost,
				postID:     postID,
				userID:     authRes.UserID,
				closeAfter: !req.KeepAlive,
			})
			return
		}
		if req.Method != "GET" {
			sendSyncError(fd, 404, "not found", errBuf)
			return
		}
		postID, ok := parseID(sub)
		if !ok {
			sendSyncError(fd, 400, "invalid post id", errBuf)
			return
		}
		w.dbPool.Submit(dbJob{
			fd:         int32(fd),
			jobType:    JobGetPost,
			postID:     postID,
			gzip:       req.GzipAccepted,
			closeAfter: !req.KeepAlive,
		})
		return
	}

	sendSyncError(fd, 404, "not found", errBuf)
}
