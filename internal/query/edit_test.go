package query

import (
	"reflect"
	"testing"
)

func TestAnalyzeSingleTable(t *testing.T) {
	cases := []struct {
		sql           string
		schema, table string
		wantNotEditbl bool
	}{
		{"SELECT * FROM orders", "", "orders", false},
		{"select id, total from orders where id > 5 order by id limit 10", "", "orders", false},
		{"SELECT * FROM mybench_test.orders o", "mybench_test", "orders", false},
		{"SELECT * FROM `weird``db`.`my table`", "weird`db", "my table", false},
		{"SELECT * FROM orders JOIN customers ON 1=1", "", "", true},
		{"SELECT * FROM orders STRAIGHT_JOIN customers", "", "", true},
		{"SELECT * FROM (\n\tselect 1) t", "", "", true},
		{"SELECT * FROM orders, customers", "", "", true},
		{"SELECT DISTINCT region FROM customers", "", "", true},
		{"SELECT region, COUNT(*) FROM customers GROUP BY region", "", "", true},
		{"SELECT * FROM (SELECT 1) t", "", "", true},
		{"UPDATE orders SET total = 0", "", "", true},
		{"SELECT 1", "", "", true},
		// 'join' inside a string literal must not trip the check.
		{"SELECT * FROM orders WHERE note = 'join the club'", "", "orders", false},
		// comment containing forbidden words is stripped
		{"SELECT * FROM orders -- union join distinct", "", "orders", false},
	}
	for _, c := range cases {
		schema, table, reason := analyzeSingleTable(c.sql)
		if c.wantNotEditbl {
			if reason == "" {
				t.Errorf("%q: expected not-editable, got schema=%q table=%q", c.sql, schema, table)
			}
			continue
		}
		if reason != "" {
			t.Errorf("%q: unexpected reason %q", c.sql, reason)
			continue
		}
		if schema != c.schema || table != c.table {
			t.Errorf("%q: got %q.%q, want %q.%q", c.sql, schema, table, c.schema, c.table)
		}
	}
}

func TestBuildStatements(t *testing.T) {
	info := &EditInfo{
		Editable:     true,
		Schema:       "db",
		Table:        "orders",
		KeyCols:      []string{"id"},
		EditableCols: []string{"id", "total", "note"},
	}
	v := func(s string) *string { return &s }
	stmts, err := buildStatements(info, []CellEdit{
		{Key: map[string]*string{"id": v("7")}, Col: "total", Value: v("12.50")},
		{Key: map[string]*string{"id": v("8")}, Col: "note", Value: nil},
		{Key: map[string]*string{"id": v("9")}, Col: "note", Value: v("it's 100% \\ done")},
	})
	if err != nil {
		t.Fatal(err)
	}
	want := []string{
		"UPDATE `db`.`orders` SET `total` = '12.50' WHERE `id` = '7' LIMIT 1;",
		"UPDATE `db`.`orders` SET `note` = NULL WHERE `id` = '8' LIMIT 1;",
		"UPDATE `db`.`orders` SET `note` = 'it''s 100% \\\\ done' WHERE `id` = '9' LIMIT 1;",
	}
	if !reflect.DeepEqual(stmts, want) {
		t.Errorf("got:\n%v\nwant:\n%v", stmts, want)
	}

	if _, err := buildStatements(info, []CellEdit{
		{Key: map[string]*string{"id": v("1")}, Col: "nope", Value: v("x")},
	}); err == nil {
		t.Error("editing a non-editable column must fail")
	}
	if _, err := buildStatements(info, []CellEdit{
		{Key: map[string]*string{}, Col: "total", Value: v("1")},
	}); err == nil {
		t.Error("missing key column must fail")
	}
}

func TestIsReadOnly(t *testing.T) {
	yes := []string{
		"SELECT 1",
		"  select * from t;  ",
		"WITH x AS (SELECT 1) SELECT * FROM x",
		"SHOW PROCESSLIST",
		"EXPLAIN SELECT 1",
		"DESCRIBE orders",
		"SELECT 'DROP TABLE t'", // write keyword inside a string
	}
	no := []string{
		"UPDATE t SET a = 1",
		"DELETE FROM t",
		"INSERT INTO t VALUES (1)",
		"DROP TABLE t",
		"SELECT 1; DELETE FROM t", // multi-statement smuggling
		"SET @a = 1",
		"CALL p()",
		"",
		// WITH-wrapped DML — the CTE hides a write.
		"WITH t AS (SELECT id FROM orders) DELETE FROM orders WHERE id IN (SELECT id FROM t)",
		"WITH t AS (SELECT 1) UPDATE orders SET total = 0",
		// File writes and variable assignment pass a READ ONLY transaction.
		"SELECT * FROM orders INTO OUTFILE '/tmp/x'",
		"SELECT 1 INTO DUMPFILE '/tmp/x'",
		"SELECT id INTO @v FROM orders LIMIT 1",
	}
	for _, s := range yes {
		if !IsReadOnly(s) {
			t.Errorf("IsReadOnly(%q) = false, want true", s)
		}
	}
	for _, s := range no {
		if IsReadOnly(s) {
			t.Errorf("IsReadOnly(%q) = true, want false", s)
		}
	}
}

func TestPatch(t *testing.T) {
	v := func(s string) *string { return &s }
	r := &storedResult{
		cols: []Column{{Name: "id"}, {Name: "total"}},
		rows: [][]*string{{v("1"), v("10")}, {v("2"), v("20")}},
	}
	r.patch([]CellEdit{{Key: map[string]*string{"id": v("2")}, Col: "total", Value: v("99")}})
	if *r.rows[1][1] != "99" {
		t.Errorf("patch missed: got %v", *r.rows[1][1])
	}
	if *r.rows[0][1] != "10" {
		t.Errorf("patch hit wrong row")
	}
}
