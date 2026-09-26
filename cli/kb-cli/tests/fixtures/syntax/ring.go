// Package ring is a fixed-size ring buffer of byte slices, with a reader
// that blocks until data arrives or the ring is closed.
package ring

import (
	"errors"
	"sync"
)

// ErrClosed is returned by Read after Close.
var ErrClosed = errors.New("ring: closed")

// Ring holds up to cap(slots) entries.
type Ring struct {
	mu     sync.Mutex
	cond   *sync.Cond
	slots  [][]byte
	head   int
	count  int
	closed bool
}

// New makes a ring with room for n entries.
func New(n int) *Ring {
	r := &Ring{slots: make([][]byte, n)}
	r.cond = sync.NewCond(&r.mu)
	return r
}

// Write appends p, overwriting the oldest entry when the ring is full.
func (r *Ring) Write(p []byte) {
	r.mu.Lock()
	defer r.mu.Unlock()
	i := (r.head + r.count) % len(r.slots)
	r.slots[i] = append([]byte(nil), p...)
	if r.count < len(r.slots) {
		r.count++
	} else {
		r.head = (r.head + 1) % len(r.slots)
	}
	r.cond.Signal()
}

// Read removes and returns the oldest entry, waiting for one.
func (r *Ring) Read() ([]byte, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for r.count == 0 && !r.closed {
		r.cond.Wait()
	}
	if r.count == 0 {
		return nil, ErrClosed
	}
	p := r.slots[r.head]
	r.slots[r.head] = nil
	r.head = (r.head + 1) % len(r.slots)
	r.count--
	return p, nil
}

// Close wakes every reader; reads drain what is left, then fail.
func (r *Ring) Close() {
	r.mu.Lock()
	r.closed = true
	r.mu.Unlock()
	r.cond.Broadcast()
}
