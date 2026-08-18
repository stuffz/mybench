package admin

// Inspector and graph queries: table/schema detail pages, the server-info
// panel and the whole-server FK graph. All read-only information_schema /
// SHOW queries on the shared pool, same rules as service.go.

import (
	"context"
	"fmt"
	"strings"
)

// systemSchemas are excluded from the FK graph (and anywhere else "user
// tables only" is meant).
const systemSchemas = "'mysql','sys','information_schema','performance_schema'"

// IndexRow is one index (columns collapsed) of a table.
type IndexRow struct {
	Name        string `json:"name"`
	Columns     string `json:"columns"`
	Unique      bool   `json:"unique"`
	Type        string `json:"type"`
	Cardinality int64  `json:"cardinality"`
}

// ForeignKey is one FK constraint (columns collapsed) of a table.
type ForeignKey struct {
	Name       string `json:"name"`
	Columns    string `json:"columns"`
	RefSchema  string `json:"refSchema"`
	RefTable   string `json:"refTable"`
	RefColumns string `json:"refColumns"`
	OnUpdate   string `json:"onUpdate"`
	OnDelete   string `json:"onDelete"`
}

// TableInfo is one information_schema.tables row for the inspector pages.
type TableInfo struct {
	Name          string `json:"name"`
	Type          string `json:"type"` // BASE TABLE | VIEW
	Engine        string `json:"engine"`
	RowFormat     string `json:"rowFormat"`
	Rows          int64  `json:"rows"`
	AvgRowLength  int64  `json:"avgRowLength"`
	DataLength    int64  `json:"dataLength"`
	IndexLength   int64  `json:"indexLength"`
	AutoIncrement int64  `json:"autoIncrement"`
	Collation     string `json:"collation"`
	Created       string `json:"created"`
	Updated       string `json:"updated"`
	Comment       string `json:"comment"`
}

// SchemaMeta is the schema-inspector header line.
type SchemaMeta struct {
	Charset   string `json:"charset"`
	Collation string `json:"collation"`
}

// KV is one SHOW VARIABLES / SHOW STATUS row.
type KV struct {
	Name  string `json:"name"`
	Value string `json:"value"`
}

// ServerInfo is the server-info tab payload.
type ServerInfo struct {
	Variables []KV `json:"variables"`
	Status    []KV `json:"status"`
}

// GraphNode is one user table in the FK graph.
type GraphNode struct {
	Schema string `json:"schema"`
	Table  string `json:"table"`
	Rows   int64  `json:"rows"`
}

// GraphEdge is one FK constraint between two tables.
type GraphEdge struct {
	FromSchema string `json:"fromSchema"`
	FromTable  string `json:"fromTable"`
	ToSchema   string `json:"toSchema"`
	ToTable    string `json:"toTable"`
	Name       string `json:"name"`
}

// Graph is the whole-server FK graph (system schemas excluded).
type Graph struct {
	Nodes []GraphNode `json:"nodes"`
	Edges []GraphEdge `json:"edges"`
}

// Indexes lists a table's indexes, one row per index.
func (s *Service) Indexes(connID, schema, table string) ([]IndexRow, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	rows, err := db.QueryContext(ctx,
		`SELECT index_name,
		        GROUP_CONCAT(column_name ORDER BY seq_in_index SEPARATOR ', '),
		        MAX(non_unique) = 0, index_type, IFNULL(MAX(cardinality), 0)
		 FROM information_schema.statistics
		 WHERE table_schema = ? AND table_name = ?
		 GROUP BY index_name, index_type
		 ORDER BY index_name = 'PRIMARY' DESC, index_name`, schema, table)
	if err != nil {
		return nil, fmt.Errorf("indexes: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []IndexRow
	for rows.Next() {
		var r IndexRow
		if err := rows.Scan(&r.Name, &r.Columns, &r.Unique, &r.Type, &r.Cardinality); err != nil {
			return nil, fmt.Errorf("scan indexes: %w", err)
		}
		out = append(out, r)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read indexes: %w", err)
	}
	return out, nil
}

// ForeignKeys lists a table's outgoing FK constraints.
func (s *Service) ForeignKeys(connID, schema, table string) ([]ForeignKey, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	rows, err := db.QueryContext(ctx,
		`SELECT k.constraint_name,
		        GROUP_CONCAT(k.column_name ORDER BY k.ordinal_position SEPARATOR ', '),
		        k.referenced_table_schema, k.referenced_table_name,
		        GROUP_CONCAT(k.referenced_column_name ORDER BY k.ordinal_position SEPARATOR ', '),
		        r.update_rule, r.delete_rule
		 FROM information_schema.key_column_usage k
		 JOIN information_schema.referential_constraints r
		   ON r.constraint_schema = k.constraint_schema
		  AND r.constraint_name = k.constraint_name
		  AND r.table_name = k.table_name
		 WHERE k.table_schema = ? AND k.table_name = ? AND k.referenced_table_name IS NOT NULL
		 GROUP BY k.constraint_name, k.referenced_table_schema, k.referenced_table_name,
		          r.update_rule, r.delete_rule
		 ORDER BY k.constraint_name`, schema, table)
	if err != nil {
		return nil, fmt.Errorf("foreign keys: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []ForeignKey
	for rows.Next() {
		var f ForeignKey
		if err := rows.Scan(&f.Name, &f.Columns, &f.RefSchema, &f.RefTable, &f.RefColumns, &f.OnUpdate, &f.OnDelete); err != nil {
			return nil, fmt.Errorf("scan foreign keys: %w", err)
		}
		out = append(out, f)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read foreign keys: %w", err)
	}
	return out, nil
}

// TablesInfo returns table detail rows for a schema; table narrows to one.
func (s *Service) TablesInfo(connID, schema, table string) ([]TableInfo, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	q := `SELECT table_name, table_type, IFNULL(engine,''), IFNULL(row_format,''),
	             IFNULL(table_rows,0), IFNULL(avg_row_length,0), IFNULL(data_length,0),
	             IFNULL(index_length,0), IFNULL(auto_increment,0), IFNULL(table_collation,''),
	             IFNULL(DATE_FORMAT(create_time,'%Y-%m-%d %H:%i'),''),
	             IFNULL(DATE_FORMAT(update_time,'%Y-%m-%d %H:%i'),''),
	             IFNULL(table_comment,'')
	      FROM information_schema.tables WHERE table_schema = ?`
	args := []any{schema}
	if table != "" {
		q += " AND table_name = ?"
		args = append(args, table)
	}
	q += " ORDER BY table_name"
	rows, err := db.QueryContext(ctx, q, args...)
	if err != nil {
		return nil, fmt.Errorf("tables info: %w", err)
	}
	defer func() { _ = rows.Close() }()
	var out []TableInfo
	for rows.Next() {
		var t TableInfo
		if err := rows.Scan(&t.Name, &t.Type, &t.Engine, &t.RowFormat, &t.Rows, &t.AvgRowLength,
			&t.DataLength, &t.IndexLength, &t.AutoIncrement, &t.Collation, &t.Created, &t.Updated, &t.Comment); err != nil {
			return nil, fmt.Errorf("scan tables info: %w", err)
		}
		out = append(out, t)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read tables info: %w", err)
	}
	return out, nil
}

// SchemaInfo returns a schema's charset/collation for the inspector header.
func (s *Service) SchemaInfo(connID, schema string) (*SchemaMeta, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	m := &SchemaMeta{}
	err = db.QueryRowContext(ctx,
		`SELECT default_character_set_name, default_collation_name
		 FROM information_schema.schemata WHERE schema_name = ?`, schema).
		Scan(&m.Charset, &m.Collation)
	if err != nil {
		return nil, fmt.Errorf("schema info: %w", err)
	}
	return m, nil
}

// ShowCreate returns the DDL of a table or view.
func (s *Service) ShowCreate(connID, schema, table string) (string, error) {
	db, err := s.pool(connID)
	if err != nil {
		return "", err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	// Identifiers cannot be parameterized; backtick-quote and double embedded backticks.
	quote := func(id string) string { return "`" + strings.ReplaceAll(id, "`", "``") + "`" }
	rows, err := db.QueryContext(ctx, fmt.Sprintf("SHOW CREATE TABLE %s.%s", quote(schema), quote(table)))
	if err != nil {
		return "", fmt.Errorf("show create: %w", err)
	}
	defer func() { _ = rows.Close() }()
	cols, err := rows.Columns()
	if err != nil {
		return "", fmt.Errorf("show create columns: %w", err)
	}
	if !rows.Next() {
		if err := rows.Err(); err != nil {
			return "", fmt.Errorf("read show create: %w", err)
		}
		return "", fmt.Errorf("show create: no result for %s.%s", schema, table)
	}
	// Tables return 2 columns, views 4; the DDL is always column 2.
	vals := make([]any, len(cols))
	for i := range vals {
		var s string
		vals[i] = &s
	}
	if err := rows.Scan(vals...); err != nil {
		return "", fmt.Errorf("scan show create: %w", err)
	}
	return *(vals[1].(*string)), nil
}

// ServerInfoSnapshot returns all global variables and status counters.
func (s *Service) ServerInfoSnapshot(connID string) (*ServerInfo, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()

	read := func(q string) ([]KV, error) {
		rows, err := db.QueryContext(ctx, q)
		if err != nil {
			return nil, err
		}
		defer func() { _ = rows.Close() }()
		var out []KV
		for rows.Next() {
			var kv KV
			if err := rows.Scan(&kv.Name, &kv.Value); err != nil {
				return nil, err
			}
			out = append(out, kv)
		}
		return out, rows.Err()
	}

	info := &ServerInfo{}
	if info.Variables, err = read("SHOW GLOBAL VARIABLES"); err != nil {
		return nil, fmt.Errorf("global variables: %w", err)
	}
	if info.Status, err = read("SHOW GLOBAL STATUS"); err != nil {
		return nil, fmt.Errorf("global status: %w", err)
	}
	return info, nil
}

// InnoDBStatus returns the raw SHOW ENGINE INNODB STATUS monitor text.
func (s *Service) InnoDBStatus(connID string) (string, error) {
	db, err := s.pool(connID)
	if err != nil {
		return "", err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()
	var typ, name, status string
	if err := db.QueryRowContext(ctx, "SHOW ENGINE INNODB STATUS").Scan(&typ, &name, &status); err != nil {
		return "", fmt.Errorf("innodb status: %w", err)
	}
	return status, nil
}

// FKGraph returns every user table and FK edge on the server (system
// schemas excluded) for the graph map.
func (s *Service) FKGraph(connID string) (*Graph, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()

	g := &Graph{}
	rows, err := db.QueryContext(ctx,
		`SELECT table_schema, table_name, IFNULL(table_rows, 0)
		 FROM information_schema.tables
		 WHERE table_type = 'BASE TABLE' AND table_schema NOT IN (`+systemSchemas+`)
		 ORDER BY table_schema, table_name`)
	if err != nil {
		return nil, fmt.Errorf("graph nodes: %w", err)
	}
	defer func() { _ = rows.Close() }()
	for rows.Next() {
		var n GraphNode
		if err := rows.Scan(&n.Schema, &n.Table, &n.Rows); err != nil {
			return nil, fmt.Errorf("scan graph nodes: %w", err)
		}
		g.Nodes = append(g.Nodes, n)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read graph nodes: %w", err)
	}

	erows, err := db.QueryContext(ctx,
		`SELECT DISTINCT table_schema, table_name,
		        referenced_table_schema, referenced_table_name, constraint_name
		 FROM information_schema.key_column_usage
		 WHERE referenced_table_name IS NOT NULL
		   AND table_schema NOT IN (`+systemSchemas+`)`)
	if err != nil {
		return nil, fmt.Errorf("graph edges: %w", err)
	}
	defer func() { _ = erows.Close() }()
	for erows.Next() {
		var e GraphEdge
		if err := erows.Scan(&e.FromSchema, &e.FromTable, &e.ToSchema, &e.ToTable, &e.Name); err != nil {
			return nil, fmt.Errorf("scan graph edges: %w", err)
		}
		g.Edges = append(g.Edges, e)
	}
	if err := erows.Err(); err != nil {
		return nil, fmt.Errorf("read graph edges: %w", err)
	}
	return g, nil
}
