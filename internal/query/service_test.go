package query

import (
	"bytes"
	"os"
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

func TestExportCSV(t *testing.T) {
	t.Parallel()
	rs := newResultStore()
	id := rs.add([]Column{{Name: "a", Type: "int"}, {Name: "b", Type: "VARCHAR"}}, nil, "c1", "t1", "SELECT 1")
	r, _ := rs.get(id)
	b1 := "x"
	r.append([][]*string{{strp("1"), &b1}, {strp("2"), nil}})

	if _, err := r.exportCSV(id); err == nil {
		t.Fatal("export while streaming must be refused")
	}
	r.finish(false, "")

	path, err := r.exportCSV(id)
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
