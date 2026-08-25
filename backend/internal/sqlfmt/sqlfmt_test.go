package sqlfmt

import "testing"

func TestFormat(t *testing.T) {
	t.Parallel()
	s := New()
	cases := []struct {
		name string
		in   string
		want string
	}{
		{
			name: "keywords upcased, clauses broken",
			in:   "select a from t where b = 1",
			want: "SELECT\n    a\nFROM t\nWHERE b = 1",
		},
		{
			name: "two-word clauses stay together",
			in:   "select a from t group by a order by a desc",
			want: "SELECT\n    a\nFROM t\nGROUP BY a\nORDER BY a DESC",
		},
		{
			name: "select list, one item per line",
			in:   "select a, b from t",
			want: "SELECT\n    a,\n    b\nFROM t",
		},
		{
			name: "string literals are never touched",
			in:   "select 'from where select' from t",
			want: "SELECT\n    'from where select'\nFROM t",
		},
		{
			name: "backquoted identifiers keep their case",
			in:   "select `From` from `Table`",
			want: "SELECT\n    `From`\nFROM `Table`",
		},
		{
			name: "column lists and tuples stay inline",
			in:   "insert into t (a,b) values (1,2)",
			want: "INSERT INTO t (a, b)\nVALUES (1, 2)",
		},
		{
			name: "calls hug their paren, arguments inline",
			in:   "select if(a is null, 'x', 'y') as v, COALESCE(NULLIF(b, 0), c) from t",
			want: "SELECT\n    IF(a IS NULL, 'x', 'y') AS v,\n    COALESCE(NULLIF(b, 0), c)\nFROM t",
		},
		{
			name: "distinct stays on the select line",
			in:   "select distinct a from t",
			want: "SELECT DISTINCT\n    a\nFROM t",
		},
		{
			name: "conditions hang under their clause",
			in: "select a from t u left join c on c.uid = u.id and CURDATE() " +
				"between c.s and c.e where u.id = ? and u.x in ('y')",
			want: "SELECT\n    a\nFROM t u\nLEFT JOIN c\n    ON c.uid = u.id\n" +
				"    AND CURDATE() BETWEEN c.s AND c.e\nWHERE u.id = ?\n    AND u.x IN ('y')",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			got, err := s.Format(c.in, 4)
			if err != nil {
				t.Fatalf("Format: %v", err)
			}
			if got != c.want {
				t.Errorf("Format(%q)\n got: %q\nwant: %q", c.in, got, c.want)
			}
		})
	}
}

// The formatter must never lose or reorder content: a second pass over its own
// output has to be a no-op, and comments have to survive.
func TestFormatStable(t *testing.T) {
	t.Parallel()
	s := New()
	for _, in := range []string{
		"select a, b from t where c = 'x' -- note\n",
		"/* leading */ select 1",
		"select * from a join b on a.id = b.id where a.x is not null;",
	} {
		once, err := s.Format(in, 4)
		if err != nil {
			t.Fatalf("Format: %v", err)
		}
		twice, err := s.Format(once, 4)
		if err != nil {
			t.Fatalf("Format twice: %v", err)
		}
		if once != twice {
			t.Errorf("not stable for %q:\n first: %q\nsecond: %q", in, once, twice)
		}
	}
}

func TestFormatTabWidth(t *testing.T) {
	t.Parallel()
	s := New()
	got, err := s.Format("select a, b from t", 2)
	if err != nil {
		t.Fatalf("Format: %v", err)
	}
	if want := "SELECT\n  a,\n  b\nFROM t"; got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}
