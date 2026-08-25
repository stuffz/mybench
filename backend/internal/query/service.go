// Package query executes SQL on per-tab sessions and buffers results in Go;
// the frontend polls state and pulls visible windows.
package query

// Service: keyed by connID, and every editor tab runs on
// its own dedicated session (lazy sql.Conn) so USE / SET / transactions work
// across statements. Run returns at column-metadata time and streams into
// the result store; the frontend polls State and pulls windows.

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/storage"
)

const defaultRowCap = 50_000 // SPEC.md default fetch cap; maxRows -1 overrides

const streamChunk = 500 // rows appended to the store per batch

// Column describes one result column (name + MySQL type).
type Column struct {
	Name string `json:"name"`
	Type string `json:"type"`
}

// RowWindow is a slice of buffered rows for the visible viewport.
type RowWindow struct {
	Offset int         `json:"offset"`
	Rows   [][]*string `json:"rows"`
}

// RunResult carries the initial state plus whether the tab's session had to
// be recreated (session state lost — the UI surfaces it, never hides it).
type RunResult struct {
	State        *ResultState `json:"state"`
	SessionReset bool         `json:"sessionReset"`
}

// Service runs SQL on per-tab sessions and serves buffered results.
type Service struct {
	conns *conn.Service
	store *resultStore
	hist  *storage.Store // query history; nil disables recording
	// history entries kept per connection; the frontend pushes the
	// preference via SetHistoryLimit (prefs live in the workspace blob).
	histKeep atomic.Int64

	mu       sync.Mutex
	timeouts map[string]time.Duration // connID → server max_execution_time
	// Serialises Lint: one shared lint session, one PREPARE at a time.
	lintMu sync.Mutex
	// One statement at a time per tab session, keyed connID+tabID.
	stmtLocks map[string]*sync.Mutex
}

const defaultHistoryKeep = 10_000

const settingHistoryKeep = "history_keep"

// New builds the query service over the connection registry; hist receives
// every executed statement (nil disables history).
func New(conns *conn.Service, hist *storage.Store) *Service {
	s := &Service{
		conns: conns, store: newResultStore(), hist: hist,
		timeouts: map[string]time.Duration{}, stmtLocks: map[string]*sync.Mutex{},
	}
	s.histKeep.Store(defaultHistoryKeep)
	// Retention survives restarts backend-side; the pref push just updates it.
	if hist != nil {
		if v, _ := hist.GetSetting(settingHistoryKeep); v != "" {
			if n, err := strconv.Atoi(v); err == nil && n > 0 {
				s.histKeep.Store(int64(n))
			}
		}
	}
	return s
}

// SetHistoryLimit applies and persists the history retention preference.
func (s *Service) SetHistoryLimit(n int) {
	if n <= 0 {
		return
	}
	s.histKeep.Store(int64(n))
	if s.hist != nil {
		_ = s.hist.SetSetting(settingHistoryKeep, strconv.Itoa(n))
	}
}

// HistoryStats reports how much history is stored (tooltip in preferences).
func (s *Service) HistoryStats() (*storage.HistoryStats, error) {
	if s.hist == nil {
		return nil, errors.New("history storage unavailable")
	}
	return s.hist.HistoryStats()
}

// execTimeout reads (and caches per connection) the server's
// max_execution_time so Run can auto-cancel ahead of it.
func (s *Service) execTimeout(ctx context.Context, sess *sql.Conn, connID string) time.Duration {
	s.mu.Lock()
	if d, ok := s.timeouts[connID]; ok {
		s.mu.Unlock()
		return d
	}
	s.mu.Unlock()
	tctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	var ms uint64
	if err := sess.QueryRowContext(tctx, "SELECT @@max_execution_time").Scan(&ms); err != nil {
		return 0 // unknown — don't cache, don't auto-cancel
	}
	d := time.Duration(ms) * time.Millisecond
	s.mu.Lock()
	s.timeouts[connID] = d
	s.mu.Unlock()
	return d
}

// Run executes on the tab's dedicated session. maxRows 0 means the default
// cap, -1 unlimited.
// stmtLock serialises statements on one tab's session.
//
// stream() publishes "done" before its deferred rows.Close() drains the rest of
// the result set off the wire, so a client that fires the next statement the
// moment it sees done lands on a session the driver has not finished with. MySQL
// answers that with "driver: bad connection", and Session() would then replace
// the session — silently dropping its USE/SET/transaction state. Holding this
// for the whole statement keeps a script's statements ordered and its session
// state intact; the cost is that Run waits for the previous drain.
func (s *Service) stmtLock(connID, tabID string) *sync.Mutex {
	key := connID + "\x00" + tabID
	s.mu.Lock()
	defer s.mu.Unlock()
	if l, ok := s.stmtLocks[key]; ok {
		return l
	}
	l := &sync.Mutex{}
	s.stmtLocks[key] = l
	return l
}

// Run starts one statement on the tab's session and registers a cancellable
// result; rows stream into the result buffer in the background.
func (s *Service) Run(connID, tabID, query string, maxRows int) (*RunResult, error) {
	lock := s.stmtLock(connID, tabID)
	lock.Lock()
	released := false
	release := func() {
		if !released {
			released = true
			lock.Unlock()
		}
	}

	baseCtx, baseCancel := context.WithCancel(context.Background())
	cancel := baseCancel
	// Session acquisition happens before the result (and its cancel) is
	// registered, so bound it: a dead network must fail the Run, not hang it
	// forever with no Cancel path.
	sessCtx, sessCancel := context.WithTimeout(baseCtx, 10*time.Second)
	sess, reset, err := conn.Session(sessCtx, s.conns, connID, tabID)
	sessCancel()
	if err != nil {
		cancel()
		release()
		return nil, err
	}

	// Auto-cancel ahead of the server's own timeout: with max_execution_time
	// set, cancel 5s earlier (at least 1s in) so the tab gets a clean cancel
	// with a clear message instead of a server-side kill.
	runCtx := baseCtx
	autoMsg := ""
	if d := s.execTimeout(baseCtx, sess, connID); d > 0 {
		margin := d - 5*time.Second
		if margin < time.Second {
			margin = time.Second
		}
		dctx, dcancel := context.WithDeadline(baseCtx, time.Now().Add(margin))
		runCtx = dctx
		cancel = func() {
			dcancel()
			baseCancel()
		}
		autoMsg = fmt.Sprintf("auto-cancelled after %s — server max_execution_time is %s", margin, d)
	}

	limit := defaultRowCap
	switch {
	case maxRows < 0:
		limit = 0 // unlimited
	case maxRows > 0:
		limit = maxRows
	}

	// Register the result (and its cancel) BEFORE executing: a slow query is
	// then cancellable from the moment Run returns, not only once the first
	// row arrives. Columns land via setColumns when execution starts
	// producing; the frontend picks them up on its state polls.
	id := s.store.add(nil, cancel, connID, tabID, query)
	go func() {
		// Released only once the rows are fully drained and closed, which is
		// when exec returns — not when the result reports done.
		defer release()
		s.exec(runCtx, id, sess, query, limit, autoMsg, time.Now())
	}()

	r, err := s.store.get(id)
	if err != nil {
		return nil, err
	}
	return &RunResult{State: r.state(id), SessionReset: reset}, nil
}

// exec runs the query and hands the rows to stream; errors (including the
// auto-cancel deadline) surface through the result state, never a panic.
func (s *Service) exec(ctx context.Context, id string, sess *sql.Conn, query string, limit int, autoMsg string, started time.Time) {
	r, err := s.store.get(id)
	if err != nil {
		return // result closed before execution started
	}
	if isDML(query) {
		s.execDML(ctx, r, sess, query, autoMsg, started)
		return
	}
	rows, qerr := sess.QueryContext(ctx, query)
	if qerr != nil {
		r.finish(false, runErrMsg(qerr, autoMsg))
		s.record(r, started)
		return
	}
	types, terr := rows.ColumnTypes()
	if terr != nil {
		_ = rows.Close() //nolint:sqlclosecheck // error path before stream() takes ownership
		r.finish(false, fmt.Sprintf("column types: %v", terr))
		s.record(r, started)
		return
	}
	cols := make([]Column, len(types))
	for i, t := range types {
		cols[i] = Column{Name: t.Name(), Type: t.DatabaseTypeName()}
	}
	r.setColumns(cols)
	s.stream(id, rows, cols, limit, autoMsg)
	s.record(r, started)
}

// runErrMsg shapes a statement error for the result state: the auto-cancel
// deadline and a user cancel get their friendly messages.
func runErrMsg(err error, autoMsg string) string {
	switch {
	case errors.Is(err, context.DeadlineExceeded) && autoMsg != "":
		return autoMsg
	case errors.Is(err, context.Canceled):
		return "cancelled"
	}
	return fmt.Sprintf("query: %v", err)
}

// execDML runs row-changing DML through Exec — QueryContext would discard
// the OK packet — and publishes its counts as a one-row result set, so the
// grid confirms an INSERT or UPDATE the same way it shows a SELECT.
func (s *Service) execDML(ctx context.Context, r *storedResult, sess *sql.Conn, query, autoMsg string, started time.Time) {
	res, err := sess.ExecContext(ctx, query)
	if err != nil {
		r.finish(false, runErrMsg(err, autoMsg))
		s.record(r, started)
		return
	}
	cols, row := dmlSummary(res)
	r.setColumns(cols)
	r.append([][]*string{row})
	r.finish(false, "")
	s.record(r, started)
}

const typeBigint = "BIGINT"

// dmlSummary shapes the OK packet as one buffered row. Last Insert ID only
// appears when the statement generated one — an UPDATE never carries it.
func dmlSummary(res sql.Result) (cols []Column, row []*string) {
	cols = []Column{{Name: "Affected Rows", Type: typeBigint}}
	row = []*string{intCell(res.RowsAffected())}
	if id, err := res.LastInsertId(); err == nil && id > 0 {
		cols = append(cols, Column{Name: "Last Insert ID", Type: typeBigint})
		row = append(row, intCell(id, nil))
	}
	return cols, row
}

// intCell renders a driver count as a cell; an unreported count becomes NULL.
func intCell(n int64, err error) *string {
	if err != nil {
		return nil
	}
	v := strconv.FormatInt(n, 10)
	return &v
}

// stream owns rows and the surrounding lifecycle; it must never panic the app.
func (s *Service) stream(id string, rows *sql.Rows, cols []Column, rowCap int, autoMsg string) {
	defer func() { _ = rows.Close() }()

	r, err := s.store.get(id)
	if err != nil {
		return // result closed before streaming started
	}

	ncols := len(cols)
	binary := make([]bool, ncols)
	for i, c := range cols {
		binary[i] = isBinaryType(c.Type)
	}

	raw := make([]sql.RawBytes, ncols)
	scan := make([]any, ncols)
	for i := range raw {
		scan[i] = &raw[i]
	}

	total := 0
	capped := false
	batch := make([][]*string, 0, streamChunk)
	flush := func() {
		if len(batch) > 0 {
			r.append(batch)
			batch = make([][]*string, 0, streamChunk)
		}
	}

	for rows.Next() {
		if rowCap > 0 && total >= rowCap {
			capped = true
			// NB: the deferred rows.Close() drains the REMAINDER of the result
			// set from the wire — on a huge result the session stays busy after
			// "done" shows. Deliberate tradeoff: cancelling the context instead
			// would kill the session and cost its USE/SET/transaction state on
			// every capped fetch. "Fetch All" exists for the honest path.
			break
		}
		if err := rows.Scan(scan...); err != nil {
			flush()
			r.finish(false, fmt.Sprintf("scan row: %v", err))
			return
		}
		row := make([]*string, ncols)
		for i, cell := range raw {
			if cell != nil {
				v := renderCell(cell, binary[i])
				row[i] = &v
			}
		}
		batch = append(batch, row)
		total++
		if len(batch) >= streamChunk {
			flush()
		}
	}
	flush()

	errMsg := ""
	if err := rows.Err(); err != nil && !errors.Is(err, context.Canceled) {
		if errors.Is(err, context.DeadlineExceeded) && autoMsg != "" {
			errMsg = autoMsg
		} else {
			errMsg = err.Error()
		}
	}
	r.finish(capped, errMsg)
}

// record writes one finished run to history (best effort — a history write
// must never fail a query).
func (s *Service) record(r *storedResult, started time.Time) {
	if s.hist == nil {
		return
	}
	r.mu.RLock()
	e := storage.HistoryEntry{
		ConnID:     r.connID,
		Query:      r.query,
		StartedAt:  started.UTC().Format(time.RFC3339),
		DurationMs: time.Since(started).Milliseconds(),
		RowCount:   len(r.rows),
		Error:      r.errMsg,
	}
	r.mu.RUnlock()
	_ = s.hist.AddHistory(e)
	_ = s.hist.PruneHistory(e.ConnID, int(s.histKeep.Load()))
}

// History lists this connection's executed statements, newest first,
// optionally filtered by a substring.
func (s *Service) History(connID, search string, limit int) ([]storage.HistoryEntry, error) {
	if s.hist == nil {
		return nil, errors.New("history storage unavailable")
	}
	return s.hist.ListHistory(connID, search, limit)
}

// ClearHistory drops the connection's history.
func (s *Service) ClearHistory(connID string) error {
	if s.hist == nil {
		return errors.New("history storage unavailable")
	}
	return s.hist.ClearHistory(connID)
}

// Snippets lists the saved snippet library (global, newest first).
func (s *Service) Snippets() ([]storage.Snippet, error) {
	if s.hist == nil {
		return nil, errors.New("storage unavailable")
	}
	return s.hist.ListSnippets()
}

// SaveSnippet stores a named SQL fragment.
func (s *Service) SaveSnippet(name, sqlText string) (*storage.Snippet, error) {
	if s.hist == nil {
		return nil, errors.New("storage unavailable")
	}
	name = strings.TrimSpace(name)
	if name == "" || strings.TrimSpace(sqlText) == "" {
		return nil, errors.New("snippet name and SQL are required")
	}
	return s.hist.AddSnippet(name, sqlText)
}

// DeleteSnippet removes one snippet.
func (s *Service) DeleteSnippet(id int64) error {
	if s.hist == nil {
		return errors.New("storage unavailable")
	}
	return s.hist.DeleteSnippet(id)
}

// State is the poll target while a query runs: counts and status, never rows.
func (s *Service) State(resultID string) (*ResultState, error) {
	r, err := s.store.get(resultID)
	if err != nil {
		return nil, err
	}
	return r.state(resultID), nil
}

// Rows serves one visible window from a buffered result.
func (s *Service) Rows(resultID string, offset, limit int) (*RowWindow, error) {
	r, err := s.store.get(resultID)
	if err != nil {
		return nil, err
	}
	return r.window(offset, limit)
}

// Cancel aborts the query's context; the driver read stops on next iteration.
// The session usually dies with it — the next Run reports the reset.
func (s *Service) Cancel(resultID string) error {
	r, err := s.store.get(resultID)
	if err != nil {
		return err
	}
	if r.cancel != nil {
		r.cancel()
	}
	return nil
}

// Sort reorders a finished result in Go; the frontend refetches windows.
func (s *Service) Sort(resultID string, col int, desc bool) error {
	r, err := s.store.get(resultID)
	if err != nil {
		return err
	}
	return r.sortBy(col, desc)
}

// CloseResult drops a buffered result and cancels its query if running.
func (s *Service) CloseResult(resultID string) {
	s.store.close(resultID)
}

// CloseTab releases the tab's dedicated session and every buffer the tab
// produced. Also called on tab mount: after a frontend reload the unmount
// cleanup never ran, so this reaps the previous page's orphaned buffers.
func (s *Service) CloseTab(connID, tabID string) {
	s.store.closeByTab(connID, tabID)
	conn.CloseSession(s.conns, connID, tabID)
}

// ExportCSV writes the full buffered result to a file in Go (SPEC.md: export
// never crosses the bridge) and returns the path. An empty path falls back to
// the old default (~/Downloads, else the temp dir).
func (s *Service) ExportCSV(resultID, path string) (string, error) {
	r, err := s.store.get(resultID)
	if err != nil {
		return "", err
	}
	return r.exportCSV(resultID, path)
}

// isBinaryType marks columns whose bytes are not text — rendered as hex,
// truncated (SPEC.md wire format).
func isBinaryType(mysqlType string) bool {
	switch strings.ToUpper(mysqlType) {
	case "BLOB", "TINYBLOB", "MEDIUMBLOB", "LONGBLOB",
		"BINARY", "VARBINARY", "BIT", "GEOMETRY":
		return true
	}
	return false
}

const binaryCap = 256 // bytes of a binary cell kept, as hex

// renderCell converts raw column bytes to the string that crosses the bridge.
// Binary columns become 0x-hex capped at binaryCap bytes; the full value is
// not retained (re-query with HEX()/length if the tail matters — phase 2 adds
// a proper full-value fetch).
func renderCell(cell []byte, binary bool) string {
	if !binary {
		return string(cell)
	}
	n := len(cell)
	if n > binaryCap {
		return fmt.Sprintf("0x%X… (+%d bytes)", cell[:binaryCap], n-binaryCap)
	}
	return fmt.Sprintf("0x%X", cell)
}
