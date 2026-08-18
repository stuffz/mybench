package storage

import (
	"path/filepath"
	"testing"
)

func open(t *testing.T) *Store {
	t.Helper()
	s, err := Open(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	return s
}

func TestWorkspaceRoundtrip(t *testing.T) {
	s := open(t)
	if blob, err := s.LoadWorkspace(); err != nil || blob != "" {
		t.Fatalf("fresh store: blob=%q err=%v", blob, err)
	}
	for _, v := range []string{`{"a":1}`, `{"a":2}`} {
		if err := s.SaveWorkspace(v); err != nil {
			t.Fatal(err)
		}
	}
	blob, err := s.LoadWorkspace()
	if err != nil || blob != `{"a":2}` {
		t.Fatalf("got %q err=%v", blob, err)
	}
}

func TestHistory(t *testing.T) {
	s := open(t)
	add := func(conn, q string) {
		t.Helper()
		if err := s.AddHistory(HistoryEntry{ConnID: conn, Query: q, StartedAt: "2026-08-18T00:00:00Z", DurationMs: 5, RowCount: 1}); err != nil {
			t.Fatal(err)
		}
	}
	add("c1", "SELECT 1")
	add("c1", "SELECT * FROM orders")
	add("c2", "SELECT 2")

	got, err := s.ListHistory("c1", "", 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 2 || got[0].Query != "SELECT * FROM orders" {
		t.Fatalf("newest-first per conn, got %+v", got)
	}

	got, err = s.ListHistory("c1", "ORDERS", 0)
	if err != nil || len(got) != 1 {
		t.Fatalf("case-insensitive search: %+v err=%v", got, err)
	}

	if err := s.ClearHistory("c1"); err != nil {
		t.Fatal(err)
	}
	got, _ = s.ListHistory("c1", "", 0)
	if len(got) != 0 {
		t.Fatalf("clear left %d rows", len(got))
	}
	got, _ = s.ListHistory("c2", "", 0)
	if len(got) != 1 {
		t.Fatalf("clear crossed connections: %+v", got)
	}
}

func TestSnippets(t *testing.T) {
	s := open(t)
	first, err := s.AddSnippet("top customers", "SELECT * FROM customers LIMIT 10")
	if err != nil || first.ID == 0 {
		t.Fatalf("add: %+v err=%v", first, err)
	}
	if _, err := s.AddSnippet("orders by day", "SELECT DATE(created_at), COUNT(*) FROM orders GROUP BY 1"); err != nil {
		t.Fatal(err)
	}
	list, err := s.ListSnippets()
	if err != nil || len(list) != 2 || list[0].Name != "orders by day" {
		t.Fatalf("list (newest first): %+v err=%v", list, err)
	}
	if err := s.DeleteSnippet(first.ID); err != nil {
		t.Fatal(err)
	}
	list, _ = s.ListSnippets()
	if len(list) != 1 || list[0].Name != "orders by day" {
		t.Fatalf("after delete: %+v", list)
	}
}

func TestPruneAndStats(t *testing.T) {
	s := open(t)
	for i := 0; i < 10; i++ {
		if err := s.AddHistory(HistoryEntry{ConnID: "c1", Query: "SELECT 1", StartedAt: "2026-08-18T00:00:00Z"}); err != nil {
			t.Fatal(err)
		}
	}
	if err := s.AddHistory(HistoryEntry{ConnID: "c2", Query: "SELECT 22", StartedAt: "2026-08-18T00:00:00Z"}); err != nil {
		t.Fatal(err)
	}

	if err := s.PruneHistory("c1", 3); err != nil {
		t.Fatal(err)
	}
	got, _ := s.ListHistory("c1", "", 0)
	if len(got) != 3 {
		t.Fatalf("prune kept %d, want 3", len(got))
	}
	got, _ = s.ListHistory("c2", "", 0)
	if len(got) != 1 {
		t.Fatalf("prune crossed connections: %+v", got)
	}

	st, err := s.HistoryStats()
	if err != nil {
		t.Fatal(err)
	}
	// 3×"SELECT 1" (8 bytes) + 1×"SELECT 22" (9 bytes)
	if st.Count != 4 || st.Bytes != 3*8+9 {
		t.Fatalf("stats: %+v", st)
	}
}
