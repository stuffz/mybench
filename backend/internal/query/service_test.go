package query

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRenderCell(t *testing.T) {
	t.Parallel()
	if got := renderCell([]byte("plain text"), false); got != "plain text" {
		t.Fatalf("text passthrough broken: %q", got)
	}
	if got := renderCell([]byte{0xDE, 0xAD}, true); got != "0xDEAD" {
		t.Fatalf("short binary: %q", got)
	}
	long := bytes.Repeat([]byte{0xAB}, binaryCap+10)
	got := renderCell(long, true)
	if !strings.HasPrefix(got, "0xABAB") || !strings.HasSuffix(got, "(+10 bytes)") {
		t.Fatalf("long binary not truncated as expected: %q…%q", got[:12], got[len(got)-14:])
	}
}

type fakeResult struct {
	id, affected int64
}

func (r fakeResult) LastInsertId() (int64, error) { return r.id, nil }

func (r fakeResult) RowsAffected() (int64, error) { return r.affected, nil }

func TestDMLSummary(t *testing.T) {
	t.Parallel()

	// INSERT: an id was generated, so both columns appear.
	cols, row := dmlSummary(fakeResult{id: 42, affected: 3})
	if len(cols) != 2 || cols[0].Name != "Affected Rows" || cols[1].Name != "Last Insert ID" {
		t.Fatalf("insert columns: %+v", cols)
	}
	if *row[0] != "3" || *row[1] != "42" {
		t.Fatalf("insert row: %q, %q", *row[0], *row[1])
	}

	// UPDATE/DELETE: no generated id, the column stays out.
	cols, row = dmlSummary(fakeResult{id: 0, affected: 5})
	if len(cols) != 1 || cols[0].Name != "Affected Rows" {
		t.Fatalf("update columns: %+v", cols)
	}
	if *row[0] != "5" {
		t.Fatalf("update row: %q", *row[0])
	}
}

func TestExportCSV(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "a", Type: "int"}, {Name: "b", Type: "VARCHAR"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	b1 := "x"
	r.append([][]*string{{strp("1"), &b1}, {strp("2"), nil}})

	if _, err := r.exportCSV(id, ""); err == nil {
		t.Fatal("export while streaming must be refused")
	}
	r.finish(false, "")

	// An explicit path is honoured verbatim.
	explicit := filepath.Join(t.TempDir(), "picked.csv")
	if got, err := r.exportCSV(id, explicit); err != nil || got != explicit {
		t.Fatalf("explicit path: got %q, %v", got, err)
	}

	path, err := r.exportCSV(id, "")
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = os.Remove(path) }()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	want := "a,b\n1,x\n2,\n"
	if string(data) != want {
		t.Fatalf("csv content:\n%q\nwant:\n%q", data, want)
	}
}
