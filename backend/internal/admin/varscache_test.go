package admin

import (
	"testing"
	"time"
)

// TestVarsCacheServesInsideItsWindow pins the reason the cache exists: the
// dashboard polls every two seconds and SHOW GLOBAL VARIABLES returns ~600 rows
// of configuration that only a SET GLOBAL or a restart can change.
func TestVarsCacheServesInsideItsWindow(t *testing.T) {
	t.Parallel()
	var c varsCache
	start := time.Unix(0, 0)
	stored := map[string]string{"max_connections": "200"}

	if _, ok := c.get("c1", start); ok {
		t.Fatal("an empty cache reported a hit")
	}

	c.put("c1", stored, start)
	got, ok := c.get("c1", start.Add(varsTTL-time.Second))
	if !ok {
		t.Fatal("a value inside the window was not served")
	}
	if got["max_connections"] != "200" {
		t.Fatalf("max_connections = %q, want 200", got["max_connections"])
	}
}

func TestVarsCacheExpiresAtTheWindowEdge(t *testing.T) {
	t.Parallel()
	var c varsCache
	start := time.Unix(0, 0)
	c.put("c1", map[string]string{"max_connections": "200"}, start)

	if _, ok := c.get("c1", start.Add(varsTTL)); ok {
		t.Error("the entry outlived its window, so a SET GLOBAL would stay hidden")
	}
}

// Two servers behind one process must not read each other's configuration.
func TestVarsCacheKeepsConnectionsApart(t *testing.T) {
	t.Parallel()
	var c varsCache
	start := time.Unix(0, 0)
	c.put("c1", map[string]string{"max_connections": "200"}, start)

	if _, ok := c.get("c2", start); ok {
		t.Fatal("one connection's variables were served for another")
	}
}
