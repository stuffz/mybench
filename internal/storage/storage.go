// Package storage is the app's local SQLite store: the workspace blob
// (prefs, tabs, layout — written through on every change) and query history.
// modernc.org/sqlite keeps the build pure Go, so CGO_ENABLED=0 static
// binaries and the distroless Docker image keep working.
package storage

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	_ "modernc.org/sqlite" // database/sql driver
)

type Store struct {
	db *sql.DB
}

// DefaultPath is the SQLite file next to connections.json / workspace.json.
func DefaultPath() string {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = "."
	}
	return filepath.Join(dir, "mybench", "mybench.db")
}

// Open creates/migrates the database. WAL keeps the frequent workspace
// write-through cheap and non-blocking for history reads.
func Open(path string) (*Store, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, fmt.Errorf("create config dir: %w", err)
	}
	db, err := sql.Open("sqlite", path+"?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=synchronous(NORMAL)")
	if err != nil {
		return nil, fmt.Errorf("open sqlite: %w", err)
	}
	// One writer connection: modernc/sqlite serializes anyway and this rules
	// out SQLITE_BUSY between the workspace write-through and history inserts.
	db.SetMaxOpenConns(1)
	for _, stmt := range []string{
		`CREATE TABLE IF NOT EXISTS workspace (
			id INTEGER PRIMARY KEY CHECK (id = 1),
			json TEXT NOT NULL,
			updated_at TEXT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS history (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			conn_id TEXT NOT NULL,
			query TEXT NOT NULL,
			started_at TEXT NOT NULL,
			duration_ms INTEGER NOT NULL,
			row_count INTEGER NOT NULL,
			error TEXT NOT NULL DEFAULT ''
		)`,
		`CREATE INDEX IF NOT EXISTS history_conn_time ON history(conn_id, id DESC)`,
		`CREATE TABLE IF NOT EXISTS settings (
			k TEXT PRIMARY KEY,
			v TEXT NOT NULL
		)`,
		`CREATE TABLE IF NOT EXISTS snippets (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			name TEXT NOT NULL,
			sql_text TEXT NOT NULL,
			created_at TEXT NOT NULL
		)`,
	} {
		if _, err := db.Exec(stmt); err != nil {
			_ = db.Close()
			return nil, fmt.Errorf("migrate: %w", err)
		}
	}
	// Additive migration: history.source ("" = editor, "mcp" = agent).
	if _, err := db.Exec(`ALTER TABLE history ADD COLUMN source TEXT NOT NULL DEFAULT ''`); err != nil {
		if !strings.Contains(err.Error(), "duplicate column") {
			_ = db.Close()
			return nil, fmt.Errorf("migrate source column: %w", err)
		}
	}
	return &Store{db: db}, nil
}

// GetSetting returns a settings value, "" when unset.
func (s *Store) GetSetting(key string) (string, error) {
	var v string
	err := s.db.QueryRow(`SELECT v FROM settings WHERE k = ?`, key).Scan(&v)
	if err == sql.ErrNoRows {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("get setting %s: %w", key, err)
	}
	return v, nil
}

// SetSetting upserts a settings value.
func (s *Store) SetSetting(key, value string) error {
	_, err := s.db.Exec(
		`INSERT INTO settings (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v`,
		key, value,
	)
	if err != nil {
		return fmt.Errorf("set setting %s: %w", key, err)
	}
	return nil
}

func (s *Store) Close() error { return s.db.Close() }

// LoadWorkspace returns the saved blob, or "" when none exists yet.
func (s *Store) LoadWorkspace() (string, error) {
	var blob string
	err := s.db.QueryRow(`SELECT json FROM workspace WHERE id = 1`).Scan(&blob)
	if err == sql.ErrNoRows {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("load workspace: %w", err)
	}
	return blob, nil
}

// SaveWorkspace writes the blob through immediately.
func (s *Store) SaveWorkspace(blob string) error {
	_, err := s.db.Exec(
		`INSERT INTO workspace (id, json, updated_at) VALUES (1, ?, ?)
		 ON CONFLICT(id) DO UPDATE SET json = excluded.json, updated_at = excluded.updated_at`,
		blob, time.Now().UTC().Format(time.RFC3339),
	)
	if err != nil {
		return fmt.Errorf("save workspace: %w", err)
	}
	return nil
}

// HistoryEntry is one executed statement.
type HistoryEntry struct {
	ID         int64  `json:"id"`
	ConnID     string `json:"connId"`
	Query      string `json:"query"`
	StartedAt  string `json:"startedAt"` // RFC3339
	DurationMs int64  `json:"durationMs"`
	RowCount   int    `json:"rowCount"`
	Error      string `json:"error"`
	Source     string `json:"source"` // "" = editor, "mcp" = agent via MCP
}

// AddHistory appends an entry.
func (s *Store) AddHistory(e HistoryEntry) error {
	_, err := s.db.Exec(
		`INSERT INTO history (conn_id, query, started_at, duration_ms, row_count, error, source)
		 VALUES (?, ?, ?, ?, ?, ?, ?)`,
		e.ConnID, e.Query, e.StartedAt, e.DurationMs, e.RowCount, e.Error, e.Source,
	)
	if err != nil {
		return fmt.Errorf("add history: %w", err)
	}
	return nil
}

// PruneHistory keeps the newest `keep` entries of one connection.
func (s *Store) PruneHistory(connID string, keep int) error {
	if keep <= 0 {
		return nil
	}
	_, err := s.db.Exec(
		`DELETE FROM history WHERE conn_id = ? AND id < (
			SELECT COALESCE(MIN(id), 0) FROM (
				SELECT id FROM history WHERE conn_id = ? ORDER BY id DESC LIMIT ?
			)
		)`,
		connID, connID, keep,
	)
	if err != nil {
		return fmt.Errorf("prune history: %w", err)
	}
	return nil
}

// HistoryStats sizes the stored history across all connections; Bytes is the
// text payload (statements + errors), not file-level overhead.
type HistoryStats struct {
	Count int64 `json:"count"`
	Bytes int64 `json:"bytes"`
}

func (s *Store) HistoryStats() (*HistoryStats, error) {
	var st HistoryStats
	err := s.db.QueryRow(
		`SELECT COUNT(*), COALESCE(SUM(LENGTH(query) + LENGTH(error)), 0) FROM history`,
	).Scan(&st.Count, &st.Bytes)
	if err != nil {
		return nil, fmt.Errorf("history stats: %w", err)
	}
	return &st, nil
}

// ListHistory returns newest-first entries for one connection, optionally
// filtered by a case-insensitive substring of the statement.
func (s *Store) ListHistory(connID, search string, limit int) ([]HistoryEntry, error) {
	if limit <= 0 || limit > 1000 {
		limit = 500
	}
	rows, err := s.db.Query(
		`SELECT id, conn_id, query, started_at, duration_ms, row_count, error, source
		 FROM history
		 WHERE conn_id = ? AND (? = '' OR instr(lower(query), lower(?)) > 0)
		 ORDER BY id DESC LIMIT ?`,
		connID, search, search, limit,
	)
	if err != nil {
		return nil, fmt.Errorf("list history: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []HistoryEntry
	for rows.Next() {
		var e HistoryEntry
		if err := rows.Scan(&e.ID, &e.ConnID, &e.Query, &e.StartedAt, &e.DurationMs, &e.RowCount, &e.Error, &e.Source); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

// Snippet is a named, reusable SQL fragment (global — not per connection).
type Snippet struct {
	ID        int64  `json:"id"`
	Name      string `json:"name"`
	SQL       string `json:"sql"`
	CreatedAt string `json:"createdAt"`
}

// AddSnippet stores a snippet and returns it with its id.
func (s *Store) AddSnippet(name, sqlText string) (*Snippet, error) {
	created := time.Now().UTC().Format(time.RFC3339)
	res, err := s.db.Exec(
		`INSERT INTO snippets (name, sql_text, created_at) VALUES (?, ?, ?)`,
		name, sqlText, created,
	)
	if err != nil {
		return nil, fmt.Errorf("add snippet: %w", err)
	}
	id, _ := res.LastInsertId()
	return &Snippet{ID: id, Name: name, SQL: sqlText, CreatedAt: created}, nil
}

// ListSnippets returns all snippets, newest first.
func (s *Store) ListSnippets() ([]Snippet, error) {
	rows, err := s.db.Query(`SELECT id, name, sql_text, created_at FROM snippets ORDER BY id DESC`)
	if err != nil {
		return nil, fmt.Errorf("list snippets: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []Snippet
	for rows.Next() {
		var sn Snippet
		if err := rows.Scan(&sn.ID, &sn.Name, &sn.SQL, &sn.CreatedAt); err != nil {
			return nil, err
		}
		out = append(out, sn)
	}
	return out, rows.Err()
}

// DeleteSnippet removes one snippet.
func (s *Store) DeleteSnippet(id int64) error {
	_, err := s.db.Exec(`DELETE FROM snippets WHERE id = ?`, id)
	if err != nil {
		return fmt.Errorf("delete snippet: %w", err)
	}
	return nil
}

// ClearHistory drops one connection's history.
func (s *Store) ClearHistory(connID string) error {
	_, err := s.db.Exec(`DELETE FROM history WHERE conn_id = ?`, connID)
	if err != nil {
		return fmt.Errorf("clear history: %w", err)
	}
	return nil
}
