package query

// Editable grid (SPEC.md phase 2): only single-table results with a usable
// PRIMARY/non-null-unique key are editable. Edits become UPDATE statements
// that the frontend previews before ApplyEdits runs them — the executed SQL
// is byte-for-byte the previewed SQL.

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/sqlesc"
)

// EditInfo says whether a finished result can be edited and how rows are
// identified. Reason is set when Editable is false.
type EditInfo struct {
	Editable     bool     `json:"editable"`
	Reason       string   `json:"reason,omitempty"`
	Schema       string   `json:"schema,omitempty"`
	Table        string   `json:"table,omitempty"`
	KeyCols      []string `json:"keyCols,omitempty"`
	EditableCols []string `json:"editableCols,omitempty"`
}

// CellEdit is one staged change. Key holds the identifying column values
// captured when the edit was staged (nil = SQL NULL); Value nil sets NULL.
type CellEdit struct {
	Key   map[string]*string `json:"key"`
	Col   string             `json:"col"`
	Value *string            `json:"value"`
}

const editMetaTimeout = 5 * time.Second

// scrub blanks comments and string-literal contents (backtick identifiers
// survive) so keyword checks and FROM extraction can't be fooled by values.
func scrub(text string) string {
	out := []byte(text)
	i, n := 0, len(text)
	blank := func(from, to int) {
		for k := from; k < to && k < n; k++ {
			if out[k] != '\n' {
				out[k] = ' '
			}
		}
	}
	for i < n {
		c := text[i]
		switch {
		case c == '\'' || c == '"':
			q := c
			start := i
			i++
			for i < n && text[i] != q {
				if text[i] == '\\' {
					i++
				}
				i++
			}
			i++
			blank(start+1, i-1)
		case c == '`':
			i++
			for i < n && text[i] != '`' {
				i++
			}
			i++
		case c == '-' && i+1 < n && text[i+1] == '-':
			start := i
			for i < n && text[i] != '\n' {
				i++
			}
			blank(start, i)
		case c == '#':
			start := i
			for i < n && text[i] != '\n' {
				i++
			}
			blank(start, i)
		case c == '/' && i+1 < n && text[i+1] == '*':
			start := i
			i += 2
			for i < n && !(text[i] == '*' && i+1 < n && text[i+1] == '/') {
				i++
			}
			i += 2
			blank(start, i)
		default:
			i++
		}
	}
	return string(out)
}

var fromRe = regexp.MustCompile(`(?is)\bfrom\s+(.*?)(\bwhere\b|\border\s+by\b|\blimit\b|\bfor\s+(update|share)\b|\block\s+in\b|;|$)`)

var (
	readOnlyPrefixRe = regexp.MustCompile(`(?s)^\s*(select|with|table|show|explain|desc|describe)\b`)
	selectPrefixRe   = regexp.MustCompile(`(?s)^\s*select\b`)
	joinRe           = regexp.MustCompile(`\b(join|straight_join)\b`)
	subqueryRe       = regexp.MustCompile(`\(\s*select\b`)
)

// depth0Words returns the lowercase word tokens of s that sit at parenthesis
// depth 0 — CTE bodies and subqueries disappear, so top-level verbs and
// clauses can be judged without a full parser.
func depth0Words(s string) []string {
	var words []string
	var cur strings.Builder
	flush := func() {
		if cur.Len() > 0 {
			words = append(words, cur.String())
			cur.Reset()
		}
	}
	depth := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '(':
			flush()
			depth++
		case c == ')':
			flush()
			depth--
		case depth == 0 && (c == '_' || c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z'):
			if c >= 'A' && c <= 'Z' {
				c += 'a' - 'A'
			}
			cur.WriteByte(c)
		default:
			if depth == 0 {
				flush()
			}
		}
	}
	flush()
	return words
}

// IsReadOnly reports whether sql is a single read-only statement (SELECT /
// WITH…SELECT / TABLE / SHOW / EXPLAIN / DESCRIBE), judged on a scrubbed
// copy so comments and string literals can't fool it. Used by the MCP gate.
// The READ ONLY transaction it runs under is the second line of defense —
// this gate additionally blocks what that transaction does NOT stop:
// SELECT … INTO OUTFILE/DUMPFILE (writes files server-side) and WITH-wrapped
// DML (WITH t AS (…) DELETE …).
func IsReadOnly(sqlText string) bool {
	s := strings.ToLower(scrub(sqlText))
	// Single statement only: no interior semicolons (trailing one is fine).
	if i := strings.IndexByte(s, ';'); i >= 0 && strings.TrimSpace(s[i+1:]) != "" {
		return false
	}
	if !readOnlyPrefixRe.MatchString(s) {
		return false
	}
	words := depth0Words(s)
	if len(words) == 0 {
		return false
	}
	// INTO at the top level is OUTFILE/DUMPFILE/@var — none is wanted here.
	for _, w := range words {
		if w == "into" {
			return false
		}
	}
	// A WITH statement's real verb comes after the CTE list; only SELECT and
	// TABLE stay read-only (DELETE/UPDATE/INSERT/REPLACE can hide there).
	if words[0] == "with" {
		for _, w := range words[1:] {
			switch w {
			case "select", "table":
				return true
			case "delete", "update", "insert", "replace":
				return false
			}
		}
		return false
	}
	return true
}

// analyzeSingleTable decides whether the SELECT reads exactly one base table
// and returns it (schema may be empty — the session default applies).
func analyzeSingleTable(query string) (schema, table, reason string) {
	s := scrub(query)
	low := strings.ToLower(s)
	if !selectPrefixRe.MatchString(low) {
		return "", "", "only SELECT results are editable"
	}
	if joinRe.MatchString(low) {
		return "", "", "not a plain single-table SELECT (join)"
	}
	for _, tok := range []string{"union", "distinct", `group\s+by`, "having"} {
		if regexp.MustCompile(`\b` + tok + `\b`).MatchString(low) {
			return "", "", "not a plain single-table SELECT (" + strings.ReplaceAll(tok, `\s+`, " ") + ")"
		}
	}
	if subqueryRe.MatchString(low) {
		return "", "", "not a plain single-table SELECT (subquery)"
	}
	m := fromRe.FindStringSubmatch(s)
	if m == nil {
		return "", "", "no FROM clause"
	}
	from := strings.TrimSpace(m[1])
	if strings.Contains(from, ",") {
		return "", "", "more than one table in FROM"
	}
	target := firstToken(from)
	if target == "" {
		return "", "", "no FROM clause"
	}
	parts := splitQualified(target)
	switch len(parts) {
	case 1:
		return "", parts[0], ""
	case 2:
		return parts[0], parts[1], ""
	default:
		return "", "", "cannot parse FROM target"
	}
}

// firstToken returns the leading whitespace-delimited token, treating
// backtick-quoted identifiers (spaces included) as atomic.
func firstToken(s string) string {
	s = strings.TrimSpace(s)
	inTick := false
	for i := 0; i < len(s); i++ {
		switch {
		case s[i] == '`':
			inTick = !inTick
		case !inTick && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'):
			return s[:i]
		}
	}
	return s
}

// splitQualified splits schema.table respecting backtick quoting and
// unquotes the parts (“ inside backticks is a literal backtick).
func splitQualified(s string) []string {
	var parts []string
	var cur strings.Builder
	inTick := false
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '`' && inTick && i+1 < len(s) && s[i+1] == '`':
			cur.WriteByte('`')
			i++
		case c == '`':
			inTick = !inTick
		case c == '.' && !inTick:
			parts = append(parts, cur.String())
			cur.Reset()
		default:
			cur.WriteByte(c)
		}
	}
	parts = append(parts, cur.String())
	return parts
}

// EditInfo resolves editability for a finished result, querying the tab
// session for the default schema and information_schema for keys and
// columns. The answer is cached on the result (immutable once done) —
// EditorTab, PreviewEdits and ApplyEdits all ask, and it costs three
// round-trips on the tab session.
func (s *Service) EditInfo(resultID string) (*EditInfo, error) {
	r, err := s.store.get(resultID)
	if err != nil {
		return nil, err
	}
	r.mu.RLock()
	done := r.done
	query := r.query
	connID, tabID := r.connID, r.tabID
	cached := r.edit
	cols := make([]Column, len(r.cols))
	copy(cols, r.cols)
	r.mu.RUnlock()
	if !done {
		return &EditInfo{Reason: "query still running"}, nil
	}
	if cached != nil {
		return cached, nil
	}
	info, err := s.editInfoUncached(query, connID, tabID, cols)
	if err != nil {
		return nil, err
	}
	r.mu.Lock()
	r.edit = info
	r.mu.Unlock()
	return info, nil
}

func (s *Service) editInfoUncached(query, connID, tabID string, cols []Column) (*EditInfo, error) {

	schema, table, reason := analyzeSingleTable(query)
	if reason != "" {
		return &EditInfo{Reason: reason}, nil
	}

	ctx, cancel := context.WithTimeout(context.Background(), editMetaTimeout)
	defer cancel()
	sess, _, err := conn.Session(ctx, s.conns, connID, tabID)
	if err != nil {
		return nil, err
	}
	if schema == "" {
		var db *string
		if err := sess.QueryRowContext(ctx, "SELECT DATABASE()").Scan(&db); err != nil {
			return nil, fmt.Errorf("resolve default schema: %w", err)
		}
		if db == nil {
			return &EditInfo{Reason: "no default schema selected (USE a database or qualify the table)"}, nil
		}
		schema = *db
	}

	keyCols, err := s.keysFor(ctx, sess, schema, table)
	if err != nil {
		return nil, err
	}
	if len(keyCols) == 0 {
		return &EditInfo{Reason: "table has no primary key or non-null unique index"}, nil
	}

	byName := map[string]Column{}
	for _, c := range cols {
		byName[c.Name] = c
	}
	for _, k := range keyCols {
		c, ok := byName[k]
		if !ok {
			return &EditInfo{Reason: fmt.Sprintf("key column %s is not in the result", k)}, nil
		}
		if isBinaryType(c.Type) {
			return &EditInfo{Reason: fmt.Sprintf("key column %s is binary", k)}, nil
		}
	}

	// Editable columns: result columns that are real columns of the table
	// (drops expressions/aliases) and not binary (cells are truncated hex).
	tblCols, err := s.columnsFor(ctx, sess, schema, table)
	if err != nil {
		return nil, err
	}
	var editable []string
	for _, c := range cols {
		if tblCols[c.Name] && !isBinaryType(c.Type) {
			editable = append(editable, c.Name)
		}
	}
	if len(editable) == 0 {
		return &EditInfo{Reason: "no editable columns in the result"}, nil
	}

	return &EditInfo{
		Editable:     true,
		Schema:       schema,
		Table:        table,
		KeyCols:      keyCols,
		EditableCols: editable,
	}, nil
}

// keysFor picks PRIMARY, else the first unique index with all NOT NULL cols.
func (s *Service) keysFor(ctx context.Context, sess *sql.Conn, schema, table string) ([]string, error) {
	rows, err := sess.QueryContext(ctx, `
		SELECT s.index_name, s.column_name, c.is_nullable = 'YES'
		FROM information_schema.statistics s
		JOIN information_schema.columns c
		  ON c.table_schema = s.table_schema
		 AND c.table_name   = s.table_name
		 AND c.column_name  = s.column_name
		WHERE s.table_schema = ? AND s.table_name = ? AND s.non_unique = 0
		ORDER BY (s.index_name = 'PRIMARY') DESC, s.index_name, s.seq_in_index`,
		schema, table)
	if err != nil {
		return nil, fmt.Errorf("read keys: %w", err)
	}
	defer func() { _ = rows.Close() }()

	type idx struct {
		cols     []string
		nullable bool
	}
	var order []string
	byName := map[string]*idx{}
	for rows.Next() {
		var name, col string
		var nullable bool
		if err := rows.Scan(&name, &col, &nullable); err != nil {
			return nil, err
		}
		ix, ok := byName[name]
		if !ok {
			ix = &idx{}
			byName[name] = ix
			order = append(order, name)
		}
		ix.cols = append(ix.cols, col)
		ix.nullable = ix.nullable || nullable
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	for _, name := range order {
		if ix := byName[name]; !ix.nullable {
			return ix.cols, nil
		}
	}
	return nil, nil
}

func (s *Service) columnsFor(ctx context.Context, sess *sql.Conn, schema, table string) (map[string]bool, error) {
	rows, err := sess.QueryContext(ctx,
		`SELECT column_name FROM information_schema.columns WHERE table_schema = ? AND table_name = ?`,
		schema, table)
	if err != nil {
		return nil, fmt.Errorf("read columns: %w", err)
	}
	defer func() { _ = rows.Close() }()
	out := map[string]bool{}
	for rows.Next() {
		var c string
		if err := rows.Scan(&c); err != nil {
			return nil, err
		}
		out[c] = true
	}
	return out, rows.Err()
}

// buildStatements renders one UPDATE per edit — exactly what Preview shows
// and Apply executes.
func buildStatements(info *EditInfo, edits []CellEdit) ([]string, error) {
	editable := map[string]bool{}
	for _, c := range info.EditableCols {
		editable[c] = true
	}
	target := sqlesc.Ident(info.Schema) + "." + sqlesc.Ident(info.Table)
	stmts := make([]string, 0, len(edits))
	for _, e := range edits {
		if !editable[e.Col] {
			return nil, fmt.Errorf("column %q is not editable", e.Col)
		}
		var where []string
		for _, k := range info.KeyCols {
			v, ok := e.Key[k]
			if !ok {
				return nil, fmt.Errorf("edit is missing key column %q", k)
			}
			if v == nil {
				where = append(where, sqlesc.Ident(k)+" IS NULL")
			} else {
				where = append(where, sqlesc.Ident(k)+" = "+sqlesc.NullableValue(v))
			}
		}
		stmts = append(stmts, fmt.Sprintf("UPDATE %s SET %s = %s WHERE %s LIMIT 1;",
			target, sqlesc.Ident(e.Col), sqlesc.NullableValue(e.Value), strings.Join(where, " AND ")))
	}
	return stmts, nil
}

// PreviewEdits returns the exact statements ApplyEdits would run.
func (s *Service) PreviewEdits(resultID string, edits []CellEdit) ([]string, error) {
	info, err := s.EditInfo(resultID)
	if err != nil {
		return nil, err
	}
	if !info.Editable {
		return nil, errors.New("result is not editable: " + info.Reason)
	}
	return buildStatements(info, edits)
}

// ApplyEdits runs the previewed statements in one transaction on the tab's
// session and patches the buffered rows on success. Returns rows affected.
func (s *Service) ApplyEdits(resultID string, edits []CellEdit) (int, error) {
	if len(edits) == 0 {
		return 0, nil
	}
	info, err := s.EditInfo(resultID)
	if err != nil {
		return 0, err
	}
	if !info.Editable {
		return 0, errors.New("result is not editable: " + info.Reason)
	}
	stmts, err := buildStatements(info, edits)
	if err != nil {
		return 0, err
	}

	r, err := s.store.get(resultID)
	if err != nil {
		return 0, err
	}
	r.mu.RLock()
	connID, tabID := r.connID, r.tabID
	r.mu.RUnlock()

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	sess, _, err := conn.Session(ctx, s.conns, connID, tabID)
	if err != nil {
		return 0, err
	}
	tx, err := sess.BeginTx(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("begin: %w", err)
	}
	total := 0
	for _, stmt := range stmts {
		res, err := tx.ExecContext(ctx, stmt)
		if err != nil {
			_ = tx.Rollback()
			return 0, fmt.Errorf("%s — %w", stmt, err)
		}
		if n, err := res.RowsAffected(); err == nil {
			total += int(n)
		}
	}
	if err := tx.Commit(); err != nil {
		return 0, fmt.Errorf("commit: %w", err)
	}

	r.patch(edits)
	return total, nil
}
