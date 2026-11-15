package main

import (
	"sync"
	"sync/atomic"
	"time"
)

// wheelSlots is the wheel's span in seconds. It must exceed the longest
// deadline (idleTimeout) so that every scheduled second maps to its own slot.
const wheelSlots = 512

// deadlineWheel enforces connection deadlines without a runtime timer per
// connection. A million runtime timers make every scheduler pass that adjusts
// the timer heap walk it, so idle connections would cost CPU in proportion to
// their number. Instead, a connection is filed under the second its deadline
// falls in, and one ticker sweeps a second's slot once that second has passed.
//
// Requests only store a later deadline in the connection; they never touch the
// wheel. When a slot is swept, a connection whose deadline moved later is filed
// again under its new deadline, so an active connection is examined about once
// per idle timeout. Only a deadline that moves earlier (a partially received
// request starting the read timeout) files the connection immediately.
type deadlineWheel struct {
	slots [wheelSlots]wheelSlot
	// next is the first second whose slot has not been swept.
	next atomic.Int64
}

// slotClosed marks a connection that is closed and must not be filed again.
const slotClosed = -1

type wheelSlot struct {
	mu    sync.Mutex
	conns []*eventConnection
}

func newDeadlineWheel() *deadlineWheel {
	w := &deadlineWheel{}
	w.next.Store(deadlineSecond(monotonic()))
	return w
}

// deadlineSecond is the first whole second at which a deadline has passed.
func deadlineSecond(deadline int64) int64 { return deadline/int64(time.Second) + 1 }

// schedule makes sure the connection is examined no later than the second
// after deadline. A connection already filed under that second or an earlier
// one is left where it is: the sweep re-files it if the deadline is later.
func (w *deadlineWheel) schedule(state *eventConnection, deadline int64) {
	second := deadlineSecond(deadline)
	for {
		current := state.slot.Load()
		if current == slotClosed || current > 0 && current <= second {
			return
		}
		if next := w.next.Load(); second < next {
			second = next
		}
		slot := &w.slots[second%wheelSlots]
		slot.mu.Lock()
		// The sweep advances next while holding the slot's lock, so a second
		// still at or after next here has not been swept yet.
		if second < w.next.Load() {
			slot.mu.Unlock()
			continue
		}
		slot.conns = append(slot.conns, state)
		// Claim the entry unless the connection closed or was filed under an
		// earlier second meanwhile; then the entry just added is stale and the
		// sweep skips it. A later filing is replaced, so the earlier deadline
		// wins.
		for !state.slot.CompareAndSwap(current, second) {
			current = state.slot.Load()
			if current == slotClosed || current > 0 && current <= second {
				break
			}
		}
		slot.mu.Unlock()
		return
	}
}

// run sweeps every second that has passed, once per tick, until stop closes.
func (w *deadlineWheel) run(stop <-chan struct{}) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	var spare []*eventConnection
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
		}
		now := monotonic()
		for second := w.next.Load(); second <= now/int64(time.Second); second++ {
			spare = w.sweep(second, now, spare)
		}
	}
}

// sweep closes the connections in second's slot whose deadline has passed and
// re-files the rest. It returns the drained slice for reuse.
func (w *deadlineWheel) sweep(second, now int64, spare []*eventConnection) []*eventConnection {
	slot := &w.slots[second%wheelSlots]
	slot.mu.Lock()
	due := slot.conns
	slot.conns = spare[:0]
	w.next.Store(second + 1)
	slot.mu.Unlock()
	for i, state := range due {
		due[i] = nil
		// A connection that was re-filed earlier, or has closed, no longer
		// belongs to this slot; its entry here is stale.
		if !state.slot.CompareAndSwap(second, 0) {
			continue
		}
		// The slot is cleared before the deadline is read. A request that
		// moves the deadline earlier stores it before checking the slot, so
		// either it sees the cleared slot and files the connection itself, or
		// this read sees its deadline.
		deadline := state.deadline.Load()
		if deadline <= now {
			state.slot.Store(slotClosed)
			_ = state.conn.Close()
			continue
		}
		w.schedule(state, deadline)
	}
	return due[:0]
}
