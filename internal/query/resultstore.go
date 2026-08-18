package query

// resultStore holds query results in Go — the frontend only ever pulls
// windows (SPEC.md: rows stay in the Go process). Buffers grow while their
// query streams; Sort works on finished buffers only.

import (
	"crypto/rand"
	"encoding/csv"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
)

type storedResult struct {
	mu     sync.RWMutex
	cols   []Column
	rows   [][]*string
	done   bool
	capped bool
	errMsg string
	cancel func()
	// Origin of the result — edit.go needs the SQL (single-table analysis)
	// and the session key (UPDATEs run on the same tab session).
	connID string
	tabID  string
	query  string
	// Cached EditInfo — results are immutable once done, and computing it
	// costs three information_schema round-trips on the tab session.
	edit *EditInfo
}

type resultStore struct {
	mu      sync.Mutex
	results map[string]*storedResult
	nextID  int
}

// newResultID is random, not sequential: result ids cross the bridge, and in
// server mode a guessable id would let one client read another's buffers.
func newResultID() string {
	buf := make([]byte, 8)
	if _, err := rand.Read(buf); err != nil {
		return "r-fallback" // never expected; collides loudly rather than silently
	}
	return "r" + hex.EncodeToString(buf)
}

// ResultState is the poll target while a query streams: the frontend asks
// for counts, never rows, until it needs a visible window.
type ResultState struct {
	ResultID string   `json:"resultId"`
	Columns  []Column `json:"columns"`
	RowCount int      `json:"rowCount"`
	Done     bool     `json:"done"`
	Capped   bool     `json:"capped"`
	Error    string   `json:"error"`
}

func newResultStore() *resultStore {
	return &resultStore{results: map[string]*storedResult{}}
}

func (rs *resultStore) add(cols []Column, cancel func(), connID, tabID, query string) string {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	id := newResultID()
	rs.results[id] = &storedResult{cols: cols, cancel: cancel, connID: connID, tabID: tabID, query: query}
	return id
}

// closeByTab drops every buffer belonging to one tab — called when the tab's
// session closes, and on tab mount to reap buffers orphaned by a frontend
// reload (unmount cleanup never ran, so the old buffers would leak until
// process exit).
func (rs *resultStore) closeByTab(connID, tabID string) {
	rs.mu.Lock()
	var victims []*storedResult
	for id, r := range rs.results {
		if r.connID == connID && r.tabID == tabID {
			victims = append(victims, r)
			delete(rs.results, id)
		}
	}
	rs.mu.Unlock()
	for _, r := range victims {
		if r.cancel != nil {
			r.cancel()
		}
	}
}

func (rs *resultStore) get(id string) (*storedResult, error) {
	rs.mu.Lock()
	defer rs.mu.Unlock()
	r, ok := rs.results[id]
	if !ok {
		return nil, fmt.Errorf("unknown result %q", id)
	}
	return r, nil
}

func (rs *resultStore) close(id string) {
	rs.mu.Lock()
	r := rs.results[id]
	delete(rs.results, id)
	rs.mu.Unlock()
	if r != nil && r.cancel != nil {
		r.cancel()
	}
}

// setColumns lands column metadata once the query starts producing —
// results are registered (cancellable) before execution begins.
func (r *storedResult) setColumns(cols []Column) {
	r.mu.Lock()
	r.cols = cols
	r.mu.Unlock()
}

func (r *storedResult) append(rows [][]*string) {
	r.mu.Lock()
	r.rows = append(r.rows, rows...)
	r.mu.Unlock()
}

func (r *storedResult) finish(capped bool, errMsg string) {
	r.mu.Lock()
	r.done = true
	r.capped = capped
	r.errMsg = errMsg
	r.mu.Unlock()
}

func (r *storedResult) state(id string) *ResultState {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return &ResultState{
		ResultID: id,
		Columns:  r.cols,
		RowCount: len(r.rows),
		Done:     r.done,
		Capped:   r.capped,
		Error:    r.errMsg,
	}
}

func (r *storedResult) window(offset, limit int) (*RowWindow, error) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if offset < 0 || offset > len(r.rows) {
		return nil, fmt.Errorf("offset %d out of range", offset)
	}
	end := min(offset+limit, len(r.rows))
	// Copy the slice header region so a concurrent append cannot race the
	// serializer reading it.
	out := make([][]*string, end-offset)
	copy(out, r.rows[offset:end])
	return &RowWindow{Offset: offset, Rows: out}, nil
}

// patch applies committed edits to the buffered rows in place: each edit
// finds its row by matching the key values captured at stage time (buffer
// still holds the old values) and sets the new cell. Applied in order, so
// key-column edits stay consistent.
func (r *storedResult) patch(edits []CellEdit) {
	r.mu.Lock()
	defer r.mu.Unlock()
	colIdx := map[string]int{}
	for i, c := range r.cols {
		colIdx[c.Name] = i
	}
	cellEq := func(a, b *string) bool {
		if a == nil || b == nil {
			return a == nil && b == nil
		}
		return *a == *b
	}
	for _, e := range edits {
		ci, ok := colIdx[e.Col]
		if !ok {
			continue
		}
	rowScan:
		for _, row := range r.rows {
			for k, v := range e.Key {
				ki, ok := colIdx[k]
				if !ok || !cellEq(row[ki], v) {
					continue rowScan
				}
			}
			row[ci] = e.Value
			break
		}
	}
}

// exportCSV writes the whole buffer (header + rows, NULL as empty cell) to
// the user's Downloads dir, falling back to the temp dir.
func (r *storedResult) exportCSV(id string) (string, error) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if !r.done {
		return "", errors.New("cannot export while the query is still streaming")
	}

	dir := os.TempDir()
	if home, err := os.UserHomeDir(); err == nil {
		if dl := filepath.Join(home, "Downloads"); dirExists(dl) {
			dir = dl
		}
	}
	path := filepath.Join(dir, "mybench-"+id+".csv")
	f, err := os.Create(path) //nolint:gosec // path is built from our own id, not user input
	if err != nil {
		return "", fmt.Errorf("create csv: %w", err)
	}
	defer func() { _ = f.Close() }()

	w := csv.NewWriter(f)
	header := make([]string, len(r.cols))
	for i, c := range r.cols {
		header[i] = c.Name
	}
	if err := w.Write(header); err != nil {
		return "", fmt.Errorf("write csv header: %w", err)
	}
	rec := make([]string, len(r.cols))
	for _, row := range r.rows {
		for i, cell := range row {
			if cell == nil {
				rec[i] = ""
			} else {
				rec[i] = *cell
			}
		}
		if err := w.Write(rec); err != nil {
			return "", fmt.Errorf("write csv row: %w", err)
		}
	}
	w.Flush()
	if err := w.Error(); err != nil {
		return "", fmt.Errorf("flush csv: %w", err)
	}
	return path, nil
}

func dirExists(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

// isNumericType decides sort semantics per column (values cross the bridge
// as strings regardless — SPEC.md wire format).
func isNumericType(mysqlType string) bool {
	switch strings.TrimPrefix(strings.ToUpper(mysqlType), "UNSIGNED ") {
	case "TINYINT", "SMALLINT", "MEDIUMINT", "INT", "BIGINT",
		"DECIMAL", "FLOAT", "DOUBLE", "YEAR":
		return true
	}
	return false
}

func (r *storedResult) sortBy(col int, desc bool) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	if !r.done {
		return errors.New("cannot sort while the query is still streaming")
	}
	if col < 0 || col >= len(r.cols) {
		return fmt.Errorf("column %d out of range", col)
	}
	numeric := isNumericType(r.cols[col].Type)
	sort.SliceStable(r.rows, func(i, j int) bool {
		a, b := r.rows[i][col], r.rows[j][col]
		// NULLs sort last in both directions, so handle them before the
		// direction swap.
		if a == nil {
			return false
		}
		if b == nil {
			return true
		}
		if desc {
			a, b = b, a
		}
		return cellLess(a, b, numeric)
	})
	return nil
}

func cellLess(a, b *string, numeric bool) bool {
	if numeric {
		af, aerr := strconv.ParseFloat(*a, 64)
		bf, berr := strconv.ParseFloat(*b, 64)
		if aerr == nil && berr == nil {
			return af < bf
		}
	}
	return *a < *b
}
