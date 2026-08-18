package mcp

// Tool surface: introspection plus a gated read-only run_query. Everything
// resolves connections by saved id or name and requires them to be OPEN in
// the mybench UI — an agent can never open a connection itself.

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/query"
	"github.com/stuffz/mybench/internal/storage"
)

const (
	queryTimeout   = 30 * time.Second
	defaultMaxRows = 200
	hardMaxRows    = 2000
)

func toolDefs() []map[string]any {
	obj := func(props map[string]any, required ...string) map[string]any {
		schema := map[string]any{"type": "object", "properties": props}
		if len(required) > 0 {
			schema["required"] = required
		}
		return schema
	}
	str := func(desc string) map[string]any { return map[string]any{"type": "string", "description": desc} }
	connProp := str("Connection id or name (see list_connections); must be open in mybench")

	return []map[string]any{
		{
			"name":        "list_connections",
			"description": "List the MySQL connections configured in mybench (no credentials) and whether each is currently open. Only open connections can be queried.",
			"inputSchema": obj(map[string]any{}),
		},
		{
			"name":        "list_schemas",
			"description": "List schemas (databases) on an open connection.",
			"inputSchema": obj(map[string]any{"connection": connProp}, "connection"),
		},
		{
			"name":        "list_tables",
			"description": "List tables and views in a schema, with engine and row estimates.",
			"inputSchema": obj(map[string]any{"connection": connProp, "schema": str("Schema name")}, "connection", "schema"),
		},
		{
			"name":        "table_schema",
			"description": "Describe one table: columns, indexes, foreign keys and the CREATE TABLE statement.",
			"inputSchema": obj(map[string]any{
				"connection": connProp,
				"schema":     str("Schema name"),
				"table":      str("Table name"),
			}, "connection", "schema", "table"),
		},
		{
			"name": "run_query",
			"description": "Run a single READ-ONLY statement (SELECT/SHOW/EXPLAIN/DESCRIBE) and return the rows. " +
				"Writes are rejected and the statement runs inside a read-only transaction. Rows are capped.",
			"inputSchema": obj(map[string]any{
				"connection": connProp,
				"sql":        str("The read-only SQL statement"),
				"max_rows":   map[string]any{"type": "integer", "description": fmt.Sprintf("Row cap (default %d, max %d)", defaultMaxRows, hardMaxRows)},
			}, "connection", "sql"),
		},
		{
			"name":        "explain",
			"description": "EXPLAIN FORMAT=TREE for a SELECT — the optimizer plan without executing the statement.",
			"inputSchema": obj(map[string]any{"connection": connProp, "sql": str("The SELECT to explain")}, "connection", "sql"),
		},
	}
}

// toolText wraps text into an MCP tool result.
func toolText(text string, isErr bool) map[string]any {
	res := map[string]any{"content": []map[string]any{{"type": "text", "text": text}}}
	if isErr {
		res["isError"] = true
	}
	return res
}

func toolJSON(v any) map[string]any {
	b, err := json.MarshalIndent(v, "", " ")
	if err != nil {
		return toolText(fmt.Sprintf("marshal: %v", err), true)
	}
	return toolText(string(b), false)
}

type callParams struct {
	Name      string          `json:"name"`
	Arguments json.RawMessage `json:"arguments"`
}

func (s *Service) callTool(raw json.RawMessage) (map[string]any, error) {
	var p callParams
	if err := json.Unmarshal(raw, &p); err != nil {
		return nil, fmt.Errorf("bad params: %w", err)
	}
	var args struct {
		Connection string `json:"connection"`
		Schema     string `json:"schema"`
		Table      string `json:"table"`
		SQL        string `json:"sql"`
		MaxRows    int    `json:"max_rows"`
	}
	if len(p.Arguments) > 0 {
		if err := json.Unmarshal(p.Arguments, &args); err != nil {
			return nil, fmt.Errorf("bad arguments: %w", err)
		}
	}

	switch p.Name {
	case "list_connections":
		return s.listConnections()
	case "list_schemas":
		id, err := s.resolve(args.Connection)
		if err != nil {
			return nil, err
		}
		schemas, err := s.admin.Schemas(id)
		if err != nil {
			return nil, err
		}
		return toolJSON(schemas), nil
	case "list_tables":
		id, err := s.resolve(args.Connection)
		if err != nil {
			return nil, err
		}
		tables, err := s.admin.Tables(id, args.Schema)
		if err != nil {
			return nil, err
		}
		return toolJSON(tables), nil
	case "table_schema":
		return s.tableSchema(args.Connection, args.Schema, args.Table)
	case "run_query":
		return s.runQuery(args.Connection, args.SQL, args.MaxRows)
	case "explain":
		if !query.IsReadOnly(args.SQL) {
			return nil, errors.New("only read-only statements can be explained here")
		}
		return s.runQuery(args.Connection, "EXPLAIN FORMAT=TREE "+args.SQL, 100)
	default:
		return nil, fmt.Errorf("unknown tool %q", p.Name)
	}
}

// resolve maps a connection id or display name to an OPEN connection's id.
func (s *Service) resolve(ref string) (string, error) {
	if ref == "" {
		return "", errors.New("connection is required")
	}
	for _, c := range s.conns.List() {
		if c.ID == ref || c.Name == ref {
			if _, err := conn.Pool(s.conns, c.ID); err != nil {
				return "", fmt.Errorf("connection %q is not open — open it in mybench first", c.Name)
			}
			return c.ID, nil
		}
	}
	return "", fmt.Errorf("no connection named %q (use list_connections)", ref)
}

func (s *Service) listConnections() (map[string]any, error) {
	type item struct {
		ID       string `json:"id"`
		Name     string `json:"name"`
		Method   string `json:"method"`
		Host     string `json:"host,omitempty"`
		Database string `json:"database,omitempty"`
		Open     bool   `json:"open"`
	}
	var out []item
	for _, c := range s.conns.List() {
		_, err := conn.Pool(s.conns, c.ID)
		out = append(out, item{
			ID: c.ID, Name: c.Name, Method: c.Method,
			Host: c.Host, Database: c.Database, Open: err == nil,
		})
	}
	return toolJSON(out), nil
}

func (s *Service) tableSchema(ref, schema, table string) (map[string]any, error) {
	id, err := s.resolve(ref)
	if err != nil {
		return nil, err
	}
	cols, err := s.admin.Columns(id, schema, table)
	if err != nil {
		return nil, err
	}
	idx, err := s.admin.Indexes(id, schema, table)
	if err != nil {
		return nil, err
	}
	fks, err := s.admin.ForeignKeys(id, schema, table)
	if err != nil {
		return nil, err
	}
	ddl, err := s.admin.ShowCreate(id, schema, table)
	if err != nil {
		return nil, err
	}
	return toolJSON(map[string]any{
		"columns": cols, "indexes": idx, "foreignKeys": fks, "createTable": ddl,
	}), nil
}

// runQuery executes one gated read-only statement inside a READ ONLY
// transaction and records it in history (source=mcp).
func (s *Service) runQuery(ref, sqlText string, maxRows int) (map[string]any, error) {
	id, err := s.resolve(ref)
	if err != nil {
		return nil, err
	}
	if !query.IsReadOnly(sqlText) {
		rejErr := errors.New("rejected: only single read-only statements (SELECT/SHOW/EXPLAIN/DESCRIBE) are allowed")
		// Audit trail: a blocked write attempt is worth seeing in history.
		if s.store != nil {
			_ = s.store.AddHistory(storage.HistoryEntry{
				ConnID:    id,
				Query:     sqlText,
				StartedAt: time.Now().UTC().Format(time.RFC3339),
				Error:     rejErr.Error(),
				Source:    "mcp",
			})
		}
		return nil, rejErr
	}
	if maxRows <= 0 {
		maxRows = defaultMaxRows
	}
	if maxRows > hardMaxRows {
		maxRows = hardMaxRows
	}

	pool, err := conn.Pool(s.conns, id)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), queryTimeout)
	defer cancel()

	started := time.Now()
	res, qErr := runReadOnly(ctx, pool, sqlText, maxRows)

	if s.store != nil {
		errMsg := ""
		rowCount := 0
		if qErr != nil {
			errMsg = qErr.Error()
		} else {
			rowCount = len(res.Rows)
		}
		_ = s.store.AddHistory(storage.HistoryEntry{
			ConnID:     id,
			Query:      sqlText,
			StartedAt:  started.UTC().Format(time.RFC3339),
			DurationMs: time.Since(started).Milliseconds(),
			RowCount:   rowCount,
			Error:      errMsg,
			Source:     "mcp",
		})
	}
	if qErr != nil {
		return nil, qErr
	}
	return toolJSON(res), nil
}

type queryResult struct {
	Columns    []string    `json:"columns"`
	Rows       [][]*string `json:"rows"`
	Capped     bool        `json:"capped"`
	DurationMs int64       `json:"durationMs"`
}

func runReadOnly(ctx context.Context, pool *sql.DB, sqlText string, maxRows int) (*queryResult, error) {
	started := time.Now()
	tx, err := pool.BeginTx(ctx, &sql.TxOptions{ReadOnly: true})
	if err != nil {
		return nil, fmt.Errorf("begin read-only tx: %w", err)
	}
	defer func() { _ = tx.Rollback() }()

	rows, err := tx.QueryContext(ctx, sqlText)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()

	cols, err := rows.Columns()
	if err != nil {
		return nil, err
	}
	out := &queryResult{Columns: cols}
	raw := make([]sql.RawBytes, len(cols))
	scan := make([]any, len(cols))
	for i := range raw {
		scan[i] = &raw[i]
	}
	for rows.Next() {
		if len(out.Rows) >= maxRows {
			out.Capped = true
			break
		}
		if err := rows.Scan(scan...); err != nil {
			return nil, err
		}
		row := make([]*string, len(cols))
		for i, cell := range raw {
			if cell != nil {
				v := string(cell)
				row[i] = &v
			}
		}
		out.Rows = append(out.Rows, row)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	out.DurationMs = time.Since(started).Milliseconds()
	return out, nil
}
