package admin

// SHOW GLOBAL VARIABLES answers with the server's whole configuration — around
// 600 rows on 8.4 — and the dashboard asks for it on every two second poll.
// Only SET GLOBAL or a restart can change any of it, so serving a recent answer
// costs a minute of staleness and saves ~30 round trips a minute per open tab.

import (
	"sync"
	"time"
)

const varsTTL = time.Minute

type varsEntry struct {
	vars    map[string]string
	fetched time.Time
}

// varsCache holds one entry per connection. Entries are replaced whole and
// never written in place, so a map handed to a caller stays safe to read.
type varsCache struct {
	mu      sync.Mutex
	entries map[string]varsEntry
}

func (c *varsCache) get(connID string, now time.Time) (map[string]string, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	entry, ok := c.entries[connID]
	if !ok || now.Sub(entry.fetched) >= varsTTL {
		return nil, false
	}
	return entry.vars, true
}

func (c *varsCache) put(connID string, vars map[string]string, now time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.entries == nil {
		c.entries = map[string]varsEntry{}
	}
	c.entries[connID] = varsEntry{vars: vars, fetched: now}
}
