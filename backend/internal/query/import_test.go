package query

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeCSV(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "in.csv")
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestReadCSVSniff(t *testing.T) {
	t.Parallel()
	path := writeCSV(t, "name;city;age\nJohn;Berlin;42\nJane;Oslo;37\n")
	p, err := readCSVFile(path, "", false, true, 100)
	if err != nil {
		t.Fatal(err)
	}
	if p.Sep != ";" {
		t.Fatalf("sniffed sep %q, want ;", p.Sep)
	}
	if !p.HasHeader || len(p.Header) != 3 || p.Header[0] != "name" {
		t.Fatalf("header not detected: hasHeader=%v header=%v", p.HasHeader, p.Header)
	}
	if p.TotalRows != 2 || len(p.Rows) != 2 || p.Rows[0][0] != "John" {
		t.Fatalf("rows wrong: total=%d rows=%v", p.TotalRows, p.Rows)
	}
	if p.Truncated {
		t.Fatal("small file must not report truncation")
	}
}

func TestReadCSVNoHeader(t *testing.T) {
	t.Parallel()
	// Numbers in row 0 → no header.
	path := writeCSV(t, "1,John\n2,Jane\n")
	p, err := readCSVFile(path, "", false, true, 100)
	if err != nil {
		t.Fatal(err)
	}
	if p.HasHeader {
		t.Fatal("numeric first row must not read as a header")
	}
	if p.TotalRows != 2 || len(p.Rows) != 2 {
		t.Fatalf("got total=%d rows=%d, want 2/2", p.TotalRows, len(p.Rows))
	}
}

func TestReadCSVExplicitOverridesSniff(t *testing.T) {
	t.Parallel()
	path := writeCSV(t, "a,b\n1,2\n")
	// Caller says: no header, comma.
	p, err := readCSVFile(path, ",", false, false, 100)
	if err != nil {
		t.Fatal(err)
	}
	if p.HasHeader || len(p.Rows) != 2 {
		t.Fatalf("explicit hasHeader=false ignored: %v rows=%d", p.HasHeader, len(p.Rows))
	}
}

func TestReadCSVTruncates(t *testing.T) {
	t.Parallel()
	path := writeCSV(t, "h1,h2\n"+strings.Repeat("x,1\n", 5))
	p, err := readCSVFile(path, ",", true, false, 3)
	if err != nil {
		t.Fatal(err)
	}
	if len(p.Rows) != 3 || p.TotalRows != 5 || !p.Truncated {
		t.Fatalf("got rows=%d total=%d truncated=%v, want 3/5/true", len(p.Rows), p.TotalRows, p.Truncated)
	}
}

func TestReadCSVEmptyFile(t *testing.T) {
	t.Parallel()
	path := writeCSV(t, "")
	if _, err := readCSVFile(path, ",", false, true, 100); err == nil {
		t.Fatal("empty file should error")
	}
}

func TestBuildInsertBatches(t *testing.T) {
	t.Parallel()
	rows := [][]*string{
		{strp("O'Brien"), strp("1")},
		{nil, strp("2")},
		{strp("c"), strp("3")},
	}
	stmts, err := buildInsertBatches("shop", "cust`om", []string{"name", "n"}, rows, 2)
	if err != nil {
		t.Fatal(err)
	}
	if len(stmts) != 2 {
		t.Fatalf("got %d statements, want 2 (batch=2 over 3 rows)", len(stmts))
	}
	want := "INSERT INTO `shop`.`cust``om` (`name`, `n`) VALUES\n('O''Brien', '1'),\n(NULL, '2');"
	if stmts[0] != want {
		t.Fatalf("got:\n%s\nwant:\n%s", stmts[0], want)
	}
	if !strings.Contains(stmts[1], "('c', '3');") {
		t.Fatalf("tail batch wrong: %s", stmts[1])
	}
}

func TestBuildInsertBatchesValidates(t *testing.T) {
	t.Parallel()
	if _, err := buildInsertBatches("s", "t", nil, [][]*string{{strp("x")}}, 10); err == nil {
		t.Fatal("no columns should error")
	}
	if _, err := buildInsertBatches("s", "t", []string{"a"}, nil, 10); err == nil {
		t.Fatal("no rows should error")
	}
	if _, err := buildInsertBatches("s", "t", []string{"a", "b"}, [][]*string{{strp("x")}}, 10); err == nil {
		t.Fatal("ragged row should error")
	}
}
