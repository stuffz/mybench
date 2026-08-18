// Package admin serves the panel queries (processlist, users/grants,
// status, schema metadata) over connection pools.
package admin

// Service: the panel queries (SPEC.md — these are the easy 5% of
// Workbench). Everything runs on the connection's shared pool, not on tab
// sessions, and every method is a plain query — no state here.

import (
	"context"
	"database/sql"
	"fmt"
	"strings"
	"time"

	"github.com/stuffz/mybench/internal/buildinfo"
	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/sqlesc"
)

const adminTimeout = 10 * time.Second

// AppInfo is the build stamp shown in the About dialog.
type AppInfo struct {
	Commit string `json:"commit"`
	Date   string `json:"date"` // RFC3339 UTC; empty when not stamped
}

// AppInfo returns the -ldflags-injected build stamp (no DB involved).
func (s *Service) AppInfo() AppInfo {
	return AppInfo{Commit: buildinfo.Commit, Date: buildinfo.Date}
}

// Process is one row of the server's processlist.
type Process struct {
	ID      int64  `json:"id"`
	User    string `json:"user"`
	Host    string `json:"host"`
	DB      string `json:"db"`
	Command string `json:"command"`
	Time    int64  `json:"time"`
	State   string `json:"state"`
	Info    string `json:"info"`
}

// UserRow is one account from mysql.user.
type UserRow struct {
	User   string `json:"user"`
	Host   string `json:"host"`
	Plugin string `json:"plugin"`
	Locked bool   `json:"locked"`
}

// StatusSnapshot is the status-strip payload.
type StatusSnapshot struct {
	Version          string `json:"version"`
	UptimeSeconds    int64  `json:"uptimeSeconds"`
	ThreadsConnected int64  `json:"threadsConnected"`
	ThreadsRunning   int64  `json:"threadsRunning"`
	Questions        int64  `json:"questions"`
}

// SchemaTable is a table or view in the sidebar tree.
type SchemaTable struct {
	Name string `json:"name"`
	Type string `json:"type"` // BASE TABLE | VIEW
}

// SchemaColumn is a column node in the sidebar tree and inspector pages.
type SchemaColumn struct {
	Name     string `json:"name"`
	Type     string `json:"type"`
	Key      string `json:"key"` // PRI | UNI | MUL | ""
	Nullable bool   `json:"nullable"`
	Default  string `json:"default"`
	Extra    string `json:"extra"`
	Comment  string `json:"comment"`
}

// Service exposes the admin-panel and schema queries to the frontend.
type Service struct {
	conns *conn.Service
}

// New builds the admin service over the connection registry.
func New(conns *conn.Service) *Service {
	return &Service{conns: conns}
}

func (s *Service) pool(connID string) (*sql.DB, error) {
	return conn.Pool(s.conns, connID)
}

// Processlist lists the server's client connections.
func (s *Service) Processlist(connID string) ([]Process, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()

	const q = `SELECT ID, IFNULL(USER,''), IFNULL(HOST,''), IFNULL(DB,''),
		IFNULL(COMMAND,''), IFNULL(TIME,0), IFNULL(STATE,''), IFNULL(INFO,'')
		FROM performance_schema.processlist ORDER BY ID`
	rows, err := db.QueryContext(ctx, q)
	if err != nil {
		// Older servers or locked-down grants: same shape via information_schema.
		rows, err = db.QueryContext(ctx, strings.Replace(q, "performance_schema", "information_schema", 1))
		if err != nil {
			return nil, fmt.Errorf("processlist: %w", err)
		}
	}
	defer func() { _ = rows.Close() }()

	var out []Process
	for rows.Next() {
		var p Process
		if err := rows.Scan(&p.ID, &p.User, &p.Host, &p.DB, &p.Command, &p.Time, &p.State, &p.Info); err != nil {
			return nil, fmt.Errorf("scan processlist: %w", err)
		}
		out = append(out, p)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read processlist: %w", err)
	}
	return out, nil
}

// Kill terminates a connection, or only its running statement with queryOnly.
func (s *Service) Kill(connID string, threadID int64, queryOnly bool) error {
	db, err := s.pool(connID)
	if err != nil {
		return err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	stmt := "KILL "
	if queryOnly {
		stmt = "KILL QUERY "
	}
	if _, err := db.ExecContext(ctx, fmt.Sprintf("%s%d", stmt, threadID)); err != nil {
		return fmt.Errorf("kill: %w", err)
	}
	return nil
}

// Users lists the server's accounts.
func (s *Service) Users(connID string) ([]UserRow, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	rows, err := db.QueryContext(ctx,
		`SELECT User, Host, plugin, account_locked = 'Y' FROM mysql.user ORDER BY User, Host`)
	if err != nil {
		return nil, fmt.Errorf("users: %w", err)
	}
	defer func() { _ = rows.Close() }()

	var out []UserRow
	for rows.Next() {
		var u UserRow
		if err := rows.Scan(&u.User, &u.Host, &u.Plugin, &u.Locked); err != nil {
			return nil, fmt.Errorf("scan users: %w", err)
		}
		out = append(out, u)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read users: %w", err)
	}
	return out, nil
}

// Grants returns SHOW GRANTS for one account.
func (s *Service) Grants(connID, user, host string) ([]string, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	// SHOW GRANTS cannot be parameterized; quote through the shared escaper
	// (single quotes AND backslashes — account names are attacker-choosable).
	q := fmt.Sprintf("SHOW GRANTS FOR %s@%s", sqlesc.Value(user), sqlesc.Value(host))
	rows, err := db.QueryContext(ctx, q)
	if err != nil {
		return nil, fmt.Errorf("grants: %w", err)
	}
	defer func() { _ = rows.Close() }()

	var out []string
	for rows.Next() {
		var g string
		if err := rows.Scan(&g); err != nil {
			return nil, fmt.Errorf("scan grants: %w", err)
		}
		out = append(out, g)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read grants: %w", err)
	}
	return out, nil
}

// GlobalStatus samples the counters behind the status strip.
func (s *Service) GlobalStatus(connID string) (*StatusSnapshot, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()

	snap := &StatusSnapshot{}
	if verr := db.QueryRowContext(ctx, "SELECT VERSION()").Scan(&snap.Version); verr != nil {
		return nil, fmt.Errorf("version: %w", verr)
	}
	rows, err := db.QueryContext(ctx,
		`SHOW GLOBAL STATUS WHERE Variable_name IN
		('Uptime','Threads_connected','Threads_running','Questions')`)
	if err != nil {
		return nil, fmt.Errorf("global status: %w", err)
	}
	defer func() { _ = rows.Close() }()
	for rows.Next() {
		var name, val string
		if err := rows.Scan(&name, &val); err != nil {
			return nil, fmt.Errorf("scan status: %w", err)
		}
		var n int64
		_, _ = fmt.Sscan(val, &n)
		switch name {
		case "Uptime":
			snap.UptimeSeconds = n
		case "Threads_connected":
			snap.ThreadsConnected = n
		case "Threads_running":
			snap.ThreadsRunning = n
		case "Questions":
			snap.Questions = n
		}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read status: %w", err)
	}
	return snap, nil
}

// Schemas lists schema names for the sidebar tree.
func (s *Service) Schemas(connID string) ([]string, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	rows, err := db.QueryContext(ctx,
		`SELECT schema_name FROM information_schema.schemata ORDER BY schema_name`)
	if err != nil {
		return nil, fmt.Errorf("schemas: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []string
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			return nil, fmt.Errorf("scan schemas: %w", err)
		}
		out = append(out, name)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read schemas: %w", err)
	}
	return out, nil
}

// Tables lists a schema's tables and views.
func (s *Service) Tables(connID, schema string) ([]SchemaTable, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	rows, err := db.QueryContext(ctx,
		`SELECT table_name, table_type FROM information_schema.tables
		 WHERE table_schema = ? ORDER BY table_name`, schema)
	if err != nil {
		return nil, fmt.Errorf("tables: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []SchemaTable
	for rows.Next() {
		var t SchemaTable
		if err := rows.Scan(&t.Name, &t.Type); err != nil {
			return nil, fmt.Errorf("scan tables: %w", err)
		}
		out = append(out, t)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read tables: %w", err)
	}
	return out, nil
}

// SchemaMap returns table→columns for every non-system schema in one query —
// it feeds the editor's completion (SPEC.md: the tree feeds the completer).
// Keys are both "table" and "schema.table".
func (s *Service) SchemaMap(connID string) (map[string][]string, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	rows, err := db.QueryContext(ctx,
		`SELECT table_schema, table_name, column_name FROM information_schema.columns
		 WHERE table_schema NOT IN ('mysql','sys','information_schema','performance_schema')
		 ORDER BY table_schema, table_name, ordinal_position`)
	if err != nil {
		return nil, fmt.Errorf("schema map: %w", err)
	}
	defer func() { _ = rows.Close() }()
	out := map[string][]string{}
	for rows.Next() {
		var schema, table, col string
		if err := rows.Scan(&schema, &table, &col); err != nil {
			return nil, fmt.Errorf("scan schema map: %w", err)
		}
		out[table] = append(out[table], col)
		out[schema+"."+table] = append(out[schema+"."+table], col)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read schema map: %w", err)
	}
	return out, nil
}

// Columns lists a table's columns for the sidebar tree.
func (s *Service) Columns(connID, schema, table string) ([]SchemaColumn, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	rows, err := db.QueryContext(ctx,
		`SELECT column_name, column_type, IFNULL(column_key,''), is_nullable = 'YES',
		        IFNULL(column_default,''), IFNULL(extra,''), IFNULL(column_comment,'')
		 FROM information_schema.columns
		 WHERE table_schema = ? AND table_name = ? ORDER BY ordinal_position`, schema, table)
	if err != nil {
		return nil, fmt.Errorf("columns: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []SchemaColumn
	for rows.Next() {
		var c SchemaColumn
		if err := rows.Scan(&c.Name, &c.Type, &c.Key, &c.Nullable, &c.Default, &c.Extra, &c.Comment); err != nil {
			return nil, fmt.Errorf("scan columns: %w", err)
		}
		out = append(out, c)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read columns: %w", err)
	}
	return out, nil
}
