package admin

// The dashboard sampler. One call returns one snapshot of every source the
// dashboard tab draws from — global status and variables, InnoDB's own
// metrics table, the live transactions and any lock waits. Rates are the
// client's job: it keeps the history and does the per-second maths, so this
// stays a stateless read with no accumulators to get wrong.
//
// Deliberately not a SHOW ENGINE INNODB STATUS parser. That text is not a
// stable interface (MySQL bug #45509) and the existing parsers are GPL /
// Artistic, so everything with a structured source is read from
// information_schema.INNODB_METRICS and performance_schema instead. Only the
// sections with no structured equivalent go through InnoDBSections.

import (
	"context"
	"database/sql"
	"fmt"
	"strconv"
	"strings"
	"time"
)

// The counters both the status strip (GlobalStatus) and the dashboard read.
const (
	statusUptime           = "Uptime"
	statusQuestions        = "Questions"
	statusThreadsConnected = "Threads_connected"
	statusThreadsRunning   = "Threads_running"
)

// dashStatus are the SHOW GLOBAL STATUS counters the dashboard graphs. A
// whitelist rather than the whole set: the full list is several hundred rows
// per poll, and the panel that does want all of them is Server Info.
var dashStatus = []string{
	statusUptime, statusQuestions, "Queries",
	statusThreadsConnected, statusThreadsRunning, "Threads_created", "Threads_cached",
	"Connections", "Max_used_connections", "Aborted_clients", "Aborted_connects",
	"Slow_queries",
	"Com_select", "Com_insert", "Com_update", "Com_delete", "Com_replace",
	"Com_commit", "Com_rollback",
	"Bytes_received", "Bytes_sent",
	"Created_tmp_tables", "Created_tmp_disk_tables", "Created_tmp_files",
	"Select_full_join", "Select_scan", "Sort_merge_passes", "Sort_rows",
	"Table_locks_immediate", "Table_locks_waited",
	"Open_tables", "Opened_tables", "Open_files",
	"Table_open_cache_hits", "Table_open_cache_misses", "Table_open_cache_overflows",
	"Handler_read_first", "Handler_read_key", "Handler_read_next",
	"Handler_read_rnd", "Handler_read_rnd_next",
	"Handler_write", "Handler_update", "Handler_delete",
	"Innodb_buffer_pool_read_requests", "Innodb_buffer_pool_reads",
	"Innodb_buffer_pool_write_requests", "Innodb_buffer_pool_wait_free",
	"Innodb_buffer_pool_pages_total", "Innodb_buffer_pool_pages_free",
	"Innodb_buffer_pool_pages_data", "Innodb_buffer_pool_pages_dirty",
	"Innodb_buffer_pool_bytes_data", "Innodb_buffer_pool_bytes_dirty",
	"Innodb_rows_read", "Innodb_rows_inserted", "Innodb_rows_updated", "Innodb_rows_deleted",
	"Innodb_data_reads", "Innodb_data_writes", "Innodb_data_fsyncs",
	"Innodb_data_read", "Innodb_data_written",
	"Innodb_log_waits", "Innodb_log_writes", "Innodb_os_log_written", "Innodb_os_log_fsyncs",
	"Innodb_row_lock_waits", "Innodb_row_lock_time", "Innodb_row_lock_time_avg",
	"Innodb_row_lock_current_waits",
	// MariaDB publishes these two as status counters; MySQL only has them in
	// INNODB_METRICS. Asking for both costs nothing and the client prefers
	// whichever it finds.
	"Innodb_deadlocks", "Innodb_history_list_length",
	"Prepared_stmt_count", "Binlog_cache_use", "Binlog_cache_disk_use",
	"Key_read_requests", "Key_reads",
}

// dashVars are the settings the dashboard shows beside the counters they
// bound — a hit rate means little without the pool size it was measured on.
var dashVars = []string{
	"version", "version_comment", "hostname", "port", "server_id", "server_uuid",
	"datadir", "time_zone", "default_storage_engine", "performance_schema",
	"character_set_server", "collation_server", "transaction_isolation", "autocommit",
	"max_connections", "thread_cache_size", "table_open_cache",
	"wait_timeout", "interactive_timeout", "max_allowed_packet",
	"tmp_table_size", "max_heap_table_size",
	"long_query_time", "slow_query_log", "read_only", "super_read_only", "log_bin",
	"gtid_mode",
	"innodb_buffer_pool_size", "innodb_buffer_pool_instances", "innodb_page_size",
	"innodb_log_file_size", "innodb_log_files_in_group", "innodb_redo_log_capacity",
	"innodb_flush_log_at_trx_commit", "innodb_flush_method",
	"innodb_io_capacity", "innodb_io_capacity_max",
	"innodb_read_io_threads", "innodb_write_io_threads",
	"innodb_file_per_table", "innodb_adaptive_hash_index", "innodb_doublewrite",
	"innodb_lock_wait_timeout", "innodb_print_all_deadlocks",
}

// TrxRow is one live InnoDB transaction.
type TrxRow struct {
	ID           string `json:"id"`
	Thread       int64  `json:"thread"`
	State        string `json:"state"`
	Started      string `json:"started"`
	Seconds      int64  `json:"seconds"`
	Isolation    string `json:"isolation"`
	RowsLocked   int64  `json:"rowsLocked"`
	RowsModified int64  `json:"rowsModified"`
	Operation    string `json:"operation"`
	Query        string `json:"query"`
}

// LockWaitRow is one row-lock wait: who is blocked, by whom, and on what.
type LockWaitRow struct {
	WaitingTrx     string `json:"waitingTrx"`
	WaitingThread  int64  `json:"waitingThread"`
	WaitingQuery   string `json:"waitingQuery"`
	BlockingTrx    string `json:"blockingTrx"`
	BlockingThread int64  `json:"blockingThread"`
	BlockingQuery  string `json:"blockingQuery"`
	Schema         string `json:"schema"`
	Table          string `json:"table"`
	Index          string `json:"index"`
	LockType       string `json:"lockType"`
	LockMode       string `json:"lockMode"`
	Seconds        int64  `json:"seconds"`
}

// DashSnapshot is one poll of the dashboard's sources. Notes carry the
// sources that were unavailable (an old server, a missing grant) so the panel
// can say so instead of showing a confident zero.
type DashSnapshot struct {
	Version   string            `json:"version"`
	SampledAt string            `json:"sampledAt"`
	Uptime    int64             `json:"uptime"`
	Status    map[string]int64  `json:"status"`
	Vars      map[string]string `json:"vars"`
	InnoDB    map[string]int64  `json:"innodb"`
	Trx       []TrxRow          `json:"trx"`
	LockWaits []LockWaitRow     `json:"lockWaits"`
	Notes     []string          `json:"notes"`
}

// wanted maps lowercased name → the canonical spelling to report it under,
// so a server that cases its variables differently still lands in the map
// under the key the client looks for.
func wanted(names []string) map[string]string {
	out := make(map[string]string, len(names))
	for _, n := range names {
		out[strings.ToLower(n)] = n
	}
	return out
}

var (
	statusWanted = wanted(dashStatus)
	varsWanted   = wanted(dashVars)
)

// Dashboard samples every dashboard source in one round trip set. A failure
// in an optional source is a note, not an error: the panel is still useful
// without lock waits, and a MariaDB or a locked-down account should not blank
// the whole tab.
func (s *Service) Dashboard(connID string) (*DashSnapshot, error) {
	db, err := s.pool(connID)
	if err != nil {
		return nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), adminTimeout)
	defer cancel()

	snap := &DashSnapshot{
		Status: map[string]int64{},
		Vars:   map[string]string{},
		InnoDB: map[string]int64{},
	}
	if serr := db.QueryRowContext(ctx, "SELECT VERSION(), NOW()").
		Scan(&snap.Version, &snap.SampledAt); serr != nil {
		return nil, fmt.Errorf("dashboard: server clock: %w", serr)
	}

	// Status and variables are the panel's spine — without them there is
	// nothing to draw, so these two are hard errors.
	status, err := selectKV(ctx, db, "SHOW GLOBAL STATUS", statusWanted)
	if err != nil {
		return nil, fmt.Errorf("dashboard: %w", err)
	}
	for name, value := range status {
		if n, perr := strconv.ParseInt(strings.TrimSpace(value), 10, 64); perr == nil {
			snap.Status[name] = n
		}
	}
	snap.Uptime = snap.Status[statusUptime]

	if snap.Vars, err = s.serverVars(ctx, db, connID); err != nil {
		return nil, fmt.Errorf("dashboard: %w", err)
	}

	if snap.InnoDB, err = innodbMetrics(ctx, db); err != nil {
		snap.Notes = append(snap.Notes, "InnoDB metrics unavailable: "+err.Error())
	}
	if snap.Trx, err = liveTrx(ctx, db); err != nil {
		snap.Notes = append(snap.Notes, "Live transactions unavailable: "+err.Error())
	}
	if snap.LockWaits, err = lockWaits(ctx, db); err != nil {
		snap.Notes = append(snap.Notes, "Lock waits unavailable: "+err.Error())
	}
	return snap, nil
}

// serverVars answers from the cache while the entry is fresh. The dashboard
// polls every two seconds; this configuration changes on SET GLOBAL or a
// restart, so re-reading ~600 rows at that rate buys nothing.
func (s *Service) serverVars(ctx context.Context, db *sql.DB, connID string) (map[string]string, error) {
	if vars, ok := s.vars.get(connID, time.Now()); ok {
		return vars, nil
	}
	vars, err := selectKV(ctx, db, "SHOW GLOBAL VARIABLES", varsWanted)
	if err != nil {
		return nil, err
	}
	s.vars.put(connID, vars, time.Now())
	return vars, nil
}

// selectKV runs a two-column name/value query and keeps only the wanted rows,
// reported under their canonical names.
func selectKV(ctx context.Context, db *sql.DB, query string, want map[string]string) (map[string]string, error) {
	rows, err := db.QueryContext(ctx, query)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", query, err)
	}
	defer func() { _ = rows.Close() }()

	out := make(map[string]string, len(want))
	for rows.Next() {
		var name, value string
		if err := rows.Scan(&name, &value); err != nil {
			return nil, fmt.Errorf("scan %s: %w", query, err)
		}
		if canon, ok := want[strings.ToLower(name)]; ok {
			out[canon] = value
		}
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read %s: %w", query, err)
	}
	return out, nil
}

// innodbMetrics reads the enabled counters from InnoDB's own metrics table.
// The default-enabled set is what SHOW ENGINE INNODB STATUS prints, in typed
// rows — which is why the dashboard reads this instead of that text.
func innodbMetrics(ctx context.Context, db *sql.DB) (map[string]int64, error) {
	rows, err := db.QueryContext(ctx,
		`SELECT NAME, COUNT FROM information_schema.INNODB_METRICS
		 WHERE STATUS = 'enabled'`)
	if err != nil {
		return nil, fmt.Errorf("innodb metrics: %w", err)
	}
	defer func() { _ = rows.Close() }()

	out := map[string]int64{}
	for rows.Next() {
		var name string
		var count int64
		if err := rows.Scan(&name, &count); err != nil {
			return nil, fmt.Errorf("scan innodb metrics: %w", err)
		}
		out[name] = count
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read innodb metrics: %w", err)
	}
	return out, nil
}

// liveTrx lists the running transactions, oldest first — the ones worth
// killing are always at the top. Capped: a server in trouble can have
// thousands open and the panel only ever shows the oldest.
func liveTrx(ctx context.Context, db *sql.DB) ([]TrxRow, error) {
	rows, err := db.QueryContext(ctx,
		`SELECT CAST(trx_id AS CHAR), IFNULL(trx_mysql_thread_id, 0),
		        IFNULL(trx_state, ''),
		        IFNULL(DATE_FORMAT(trx_started, '%Y-%m-%d %H:%i:%s'), ''),
		        IFNULL(TIMESTAMPDIFF(SECOND, trx_started, NOW()), 0),
		        IFNULL(trx_isolation_level, ''),
		        IFNULL(trx_rows_locked, 0), IFNULL(trx_rows_modified, 0),
		        IFNULL(trx_operation_state, ''), IFNULL(trx_query, '')
		 FROM information_schema.innodb_trx
		 ORDER BY trx_started LIMIT 100`)
	if err != nil {
		return nil, fmt.Errorf("innodb trx: %w", err)
	}
	defer func() { _ = rows.Close() }()

	var out []TrxRow
	for rows.Next() {
		var t TrxRow
		if err := rows.Scan(&t.ID, &t.Thread, &t.State, &t.Started, &t.Seconds,
			&t.Isolation, &t.RowsLocked, &t.RowsModified, &t.Operation, &t.Query); err != nil {
			return nil, fmt.Errorf("scan innodb trx: %w", err)
		}
		out = append(out, t)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read innodb trx: %w", err)
	}
	return out, nil
}

// lockWaits pairs each waiting transaction with the one blocking it.
// performance_schema.data_locks / data_lock_waits are the 8.0+ source; the
// removed INNODB_LOCKS pair has no replacement on servers without them, so
// this simply reports unavailable there rather than guessing.
func lockWaits(ctx context.Context, db *sql.DB) ([]LockWaitRow, error) {
	rows, err := db.QueryContext(ctx,
		`SELECT CAST(w.REQUESTING_ENGINE_TRANSACTION_ID AS CHAR),
		        IFNULL(rt.trx_mysql_thread_id, 0), IFNULL(rt.trx_query, ''),
		        CAST(w.BLOCKING_ENGINE_TRANSACTION_ID AS CHAR),
		        IFNULL(bt.trx_mysql_thread_id, 0), IFNULL(bt.trx_query, ''),
		        IFNULL(rl.OBJECT_SCHEMA, ''), IFNULL(rl.OBJECT_NAME, ''),
		        IFNULL(rl.INDEX_NAME, ''), IFNULL(rl.LOCK_TYPE, ''),
		        IFNULL(rl.LOCK_MODE, ''),
		        IFNULL(TIMESTAMPDIFF(SECOND, rt.trx_wait_started, NOW()), 0)
		 FROM performance_schema.data_lock_waits w
		 LEFT JOIN information_schema.innodb_trx rt
		        ON rt.trx_id = w.REQUESTING_ENGINE_TRANSACTION_ID
		 LEFT JOIN information_schema.innodb_trx bt
		        ON bt.trx_id = w.BLOCKING_ENGINE_TRANSACTION_ID
		 LEFT JOIN performance_schema.data_locks rl
		        ON rl.ENGINE_LOCK_ID = w.REQUESTING_ENGINE_LOCK_ID`)
	if err != nil {
		return nil, fmt.Errorf("lock waits: %w", err)
	}
	defer func() { _ = rows.Close() }()

	var out []LockWaitRow
	for rows.Next() {
		var w LockWaitRow
		if err := rows.Scan(&w.WaitingTrx, &w.WaitingThread, &w.WaitingQuery,
			&w.BlockingTrx, &w.BlockingThread, &w.BlockingQuery,
			&w.Schema, &w.Table, &w.Index, &w.LockType, &w.LockMode,
			&w.Seconds); err != nil {
			return nil, fmt.Errorf("scan lock waits: %w", err)
		}
		out = append(out, w)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("read lock waits: %w", err)
	}
	return out, nil
}
