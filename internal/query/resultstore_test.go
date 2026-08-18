package query

import (
	"testing"
)

func strp(s string) *string { return &s }

func cells(vals ...any) [][]*string {
	rows := make([][]*string, len(vals))
	for i, v := range vals {
		switch x := v.(type) {
		case string:
			rows[i] = []*string{strp(x)}
		case nil:
			rows[i] = []*string{nil}
		default:
			panic("unsupported test cell")
		}
	}
	return rows
}

func column(vals [][]*string) []string {
	out := make([]string, len(vals))
	for i, r := range vals {
		if r[0] == nil {
			out[i] = "<NULL>"
		} else {
			out[i] = *r[0]
		}
	}
	return out
}

func TestWindowBounds(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "n", Type: "int"}}, nil, "c1", "t1", "SELECT 1")
	r, err := rs.get(id)
	if err != nil {
		t.Fatal(err)
	}
	r.append(cells("1", "2", "3"))

	w, err := r.window(1, 10)
	if err != nil {
		t.Fatalf("window: %v", err)
	}
	if len(w.Rows) != 2 || w.Offset != 1 {
		t.Fatalf("got offset=%d len=%d, want offset=1 len=2", w.Offset, len(w.Rows))
	}

	if _, err := r.window(-1, 5); err == nil {
		t.Fatal("negative offset should error")
	}
	if _, err := r.window(4, 5); err == nil {
		t.Fatal("offset past end should error")
	}
	if w, err := r.window(3, 5); err != nil || len(w.Rows) != 0 {
		t.Fatalf("offset==len should give empty window, got err=%v len=%d", err, len(w.Rows))
	}
}

func TestSortNumericWithNulls(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "n", Type: "bigint"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	r.append(cells("10", nil, "2", "9223372036854775807", nil, "1"))
	r.finish(false, "")

	if err := r.sortBy(0, false); err != nil {
		t.Fatal(err)
	}
	got := column(r.rows)
	want := []string{"1", "2", "10", "9223372036854775807", "<NULL>", "<NULL>"}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("asc: got %v, want %v", got, want)
		}
	}

	if err := r.sortBy(0, true); err != nil {
		t.Fatal(err)
	}
	got = column(r.rows)
	want = []string{"9223372036854775807", "10", "2", "1", "<NULL>", "<NULL>"}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("desc: got %v, want %v (NULLs must stay last)", got, want)
		}
	}
}

func TestSortString(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "s", Type: "VARCHAR"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	// String sort: "10" < "2" lexically — that's the point of the numeric split.
	r.append(cells("b", "10", "2", "a"))
	r.finish(false, "")

	if err := r.sortBy(0, false); err != nil {
		t.Fatal(err)
	}
	got := column(r.rows)
	want := []string{"10", "2", "a", "b"}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("got %v, want %v", got, want)
		}
	}
}

func TestSortWhileStreamingRefused(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "n", Type: "int"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	r.append(cells("1"))
	if err := r.sortBy(0, false); err == nil {
		t.Fatal("sort on a streaming result should be refused")
	}
}

func TestCloseCancels(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	cancelled := false
	id := rs.add(nil, func() { cancelled = true }, "c1", "t1", "SELECT 1")
	rs.close(id)
	if !cancelled {
		t.Fatal("close must invoke the cancel func")
	}
	if _, err := rs.get(id); err == nil {
		t.Fatal("closed result should be gone")
	}
}
