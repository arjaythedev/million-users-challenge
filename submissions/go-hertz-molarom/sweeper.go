package main

import (
	"context"
	"sync"
	"sync/atomic"
	"time"

	"github.com/cloudwego/netpoll"
)

type connState struct {
	conn     netpoll.Connection
	lastSeen atomic.Int64
	inflight atomic.Int32
}

func (st *connState) begin() { st.inflight.Add(1) }

func (st *connState) end(now int64) {
	st.lastSeen.Store(now)
	st.inflight.Add(-1)
}

const sweepShards = 64

type sweepShard struct {
	mu    sync.Mutex
	conns []*connState
}

// idleSweeper closes keep-alive connections that have been idle longer than timeout.
type idleSweeper struct {
	timeout time.Duration
	epoch   time.Time
	next    atomic.Uint32
	shards  [sweepShards]sweepShard

	expired []*connState
}

func newIdleSweeper(timeout time.Duration) *idleSweeper {
	return &idleSweeper{timeout: timeout, epoch: time.Now()}
}

func (s *idleSweeper) now() int64 {
	return int64(time.Since(s.epoch) / time.Second)
}

func (s *idleSweeper) track(nc netpoll.Connection) *connState {
	st := &connState{conn: nc}
	st.lastSeen.Store(s.now())

	sh := &s.shards[s.next.Add(1)%sweepShards]
	sh.mu.Lock()
	sh.conns = append(sh.conns, st)
	sh.mu.Unlock()
	return st
}

func (s *idleSweeper) Run(ctx context.Context) {
	interval := min(max(s.timeout/8, time.Second), 10*time.Second)
	t := time.NewTicker(interval)
	defer t.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
			s.sweep()
		}
	}
}

func (s *idleSweeper) sweep() {
	cutoff := s.now() - int64(s.timeout/time.Second)

	for i := range s.shards {
		sh := &s.shards[i]
		sh.mu.Lock()
		conns := sh.conns
		for j := 0; j < len(conns); {
			st := conns[j]
			switch {
			case !st.conn.IsActive():
			case st.inflight.Load() == 0 && st.lastSeen.Load() <= cutoff:
				s.expired = append(s.expired, st)
			default:
				j++
				continue
			}
			last := len(conns) - 1
			conns[j], conns[last] = conns[last], nil
			conns = conns[:last]
		}
		sh.conns = conns
		sh.mu.Unlock()

		for k, st := range s.expired {
			_ = st.conn.Close()
			s.expired[k] = nil
		}
		s.expired = s.expired[:0]
	}
}
