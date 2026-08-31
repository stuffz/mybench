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

func TestFilterFuzzy(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add(
		[]Column{{Name: "name", Type: "VARCHAR"}, {Name: "city", Type: "VARCHAR"}},
		nil, "c1", "t1", "SELECT 1",
	)
	r, _ := rs.get(id)
	r.rows = [][]*string{
		{strp("John Smith"), strp("Berlin")},
		{strp("Jane Doe"), strp("Oslo")},
		{strp("Bob Jones"), nil},
	}
	r.finish(false, "")

	// Fuzzy: subsequence, case-insensitive — "jsm" hits only "John Smith".
	if err := r.filterBy("JSM"); err != nil {
		t.Fatal(err)
	}
	st := r.state(id)
	if st.RowCount != 1 || st.TotalRows != 3 {
		t.Fatalf("got rowCount=%d totalRows=%d, want 1/3", st.RowCount, st.TotalRows)
	}
	w, err := r.window(0, 10)
	if err != nil {
		t.Fatal(err)
	}
	if len(w.Rows) != 1 || *w.Rows[0][0] != "John Smith" {
		t.Fatalf("window should hold the one match, got %d rows", len(w.Rows))
	}

	// Every term must match some cell: name from one column, city from another.
	if err := r.filterBy("jane oslo"); err != nil {
		t.Fatal(err)
	}
	if got := r.state(id).RowCount; got != 1 {
		t.Fatalf("multi-term: got %d rows, want 1", got)
	}
	if err := r.filterBy("jane berlin"); err != nil {
		t.Fatal(err)
	}
	if got := r.state(id).RowCount; got != 0 {
		t.Fatalf("non-matching term must exclude the row, got %d", got)
	}

	// Empty needle clears the filter.
	if err := r.filterBy(""); err != nil {
		t.Fatal(err)
	}
	st = r.state(id)
	if st.RowCount != 3 || st.TotalRows != 3 {
		t.Fatalf("cleared: got rowCount=%d totalRows=%d, want 3/3", st.RowCount, st.TotalRows)
	}
}

func TestFilterWhileStreamingRefused(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "n", Type: "int"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	r.append(cells("1"))
	if err := r.filterBy("1"); err == nil {
		t.Fatal("filter on a streaming result should be refused")
	}
}

func TestSortRebuildsFilteredView(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "n", Type: "int"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	r.append(cells("12", "3", "21", "11"))
	r.finish(false, "")

	if err := r.filterBy("1"); err != nil {
		t.Fatal(err)
	}
	if err := r.sortBy(0, false); err != nil {
		t.Fatal(err)
	}
	w, err := r.window(0, 10)
	if err != nil {
		t.Fatal(err)
	}
	got := column(w.Rows)
	want := []string{"11", "12", "21"}
	if len(got) != len(want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("view must follow the sort: got %v, want %v", got, want)
		}
	}
}

func TestFilteredWindowBounds(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "n", Type: "int"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	r.append(cells("1", "2", "10"))
	r.finish(false, "")

	if err := r.filterBy("1"); err != nil {
		t.Fatal(err)
	}
	// Two matches; offsets validate against the view, not the buffer.
	if _, err := r.window(3, 5); err == nil {
		t.Fatal("offset past the filtered end should error")
	}
	w, err := r.window(1, 5)
	if err != nil {
		t.Fatal(err)
	}
	if len(w.Rows) != 1 || *w.Rows[0][0] != "10" {
		t.Fatalf("got %v", column(w.Rows))
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
