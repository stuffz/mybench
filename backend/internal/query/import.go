package query

// CSV import and row insertion for the sidebar's Import Rows dialog. The
// dialog's preview and the import share one parser (this file), so what the
// grid shows is exactly what gets inserted. File I/O stays in the Go
// process, like exportCSV.

import (
	"context"
	"encoding/csv"
	"errors"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/sqlesc"
)

const (
	// csvRowCap bounds what one ReadCSV returns — the dialog grid holds
	// every row, so a huge file must not land in the UI wholesale.
	csvRowCap = 10000
	// insertBatchRows rows per generated multi-row INSERT statement.
	insertBatchRows = 500
	// importTimeout bounds the whole import transaction.
	importTimeout = 2 * time.Minute
	// csvSniffLines is how many records the separator/header sniff reads.
	csvSniffLines = 10
)

// csvSeparators are the sniff candidates, first match wins ties.
var csvSeparators = []rune{',', ';', '\t', '|'}

// CSVPreview is a parsed CSV file: the separator and header decision
// actually used (sniffed or caller-given), and the data rows, capped.
type CSVPreview struct {
	Sep       string     `json:"sep"`
	HasHeader bool       `json:"hasHeader"`
	Header    []string   `json:"header"` // empty when hasHeader is false
	Rows      [][]string `json:"rows"`
	TotalRows int        `json:"totalRows"` // data rows in the whole file
	Truncated bool       `json:"truncated"`
}

// ReadCSV parses a CSV file for the import dialog. With sniff true the
// separator and header row are auto-detected and sep/hasHeader inputs are
// ignored; the choices used are returned so the dialog can show them.
func (s *Service) ReadCSV(path, sep string, hasHeader, sniff bool) (*CSVPreview, error) {
	return readCSVFile(path, sep, hasHeader, sniff, csvRowCap)
}

func readCSVFile(path, sep string, hasHeader, sniff bool, maxRows int) (*CSVPreview, error) {
	f, err := os.Open(path) //nolint:gosec // the user picked this path in an open dialog
	if err != nil {
		return nil, fmt.Errorf("open csv: %w", err)
	}
	defer func() { _ = f.Close() }()

	var sepRune rune
	if sniff {
		sepRune, err = sniffSeparator(f)
		if err != nil {
			return nil, err
		}
	} else {
		r := []rune(sep)
		if len(r) != 1 {
			return nil, fmt.Errorf("separator must be one character, got %q", sep)
		}
		sepRune = r[0]
	}

	rd := csv.NewReader(f)
	rd.Comma = sepRune
	rd.FieldsPerRecord = -1 // ragged rows pass through; the dialog maps them
	rd.LazyQuotes = true

	out := &CSVPreview{Sep: string(sepRune)}
	var records [][]string
	for {
		rec, err := rd.Read()
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return nil, fmt.Errorf("parse csv: %w", err)
		}
		if len(records) < maxRows+1 { // +1: a header row does not count against the cap
			records = append(records, rec)
		}
		out.TotalRows++
	}
	if len(records) == 0 {
		return nil, errors.New("the file is empty")
	}

	if sniff {
		hasHeader = sniffHeader(records)
	}
	out.HasHeader = hasHeader
	if hasHeader {
		out.Header = records[0]
		records = records[1:]
		out.TotalRows--
	}
	if len(records) > maxRows {
		records = records[:maxRows]
	}
	out.Rows = records
	out.Truncated = out.TotalRows > len(records)
	return out, nil
}

// sniffSeparator parses the head of the file once per candidate and keeps
// the one that yields the most columns consistently. Rewinds f.
func sniffSeparator(f *os.File) (rune, error) {
	head := make([]byte, 64*1024)
	n, err := f.Read(head)
	if err != nil && !errors.Is(err, io.EOF) {
		return 0, fmt.Errorf("read csv: %w", err)
	}
	if _, err := f.Seek(0, io.SeekStart); err != nil {
		return 0, fmt.Errorf("rewind csv: %w", err)
	}
	sample := string(head[:n])

	best, bestCols := csvSeparators[0], 1
	for _, cand := range csvSeparators {
		rd := csv.NewReader(strings.NewReader(sample))
		rd.Comma = cand
		rd.FieldsPerRecord = -1
		rd.LazyQuotes = true
		cols := 0
		consistent := true
		for i := range csvSniffLines {
			rec, err := rd.Read()
			if err != nil {
				break
			}
			if cols == 0 {
				cols = len(rec)
			} else if len(rec) != cols {
				// The sample may cut the last line mid-record; a short
				// final read is not an inconsistency signal worth losing
				// the candidate over.
				if i < csvSniffLines-1 {
					consistent = false
				}
				break
			}
		}
		if consistent && cols > bestCols {
			best, bestCols = cand, cols
		}
	}
	return best, nil
}

// sniffHeader guesses whether row 0 is a header: it has no numeric cell
// while some later row has one. A file with no numbers anywhere defaults
// to having a header — typical exports do, and the dialog can override.
func sniffHeader(records [][]string) bool {
	if len(records) < 2 {
		return false
	}
	if rowHasNumber(records[0]) {
		return false
	}
	for _, rec := range records[1:] {
		if rowHasNumber(rec) {
			return true
		}
	}
	return true
}

func rowHasNumber(rec []string) bool {
	for _, cell := range rec {
		if cell == "" {
			continue
		}
		if _, err := strconv.ParseFloat(cell, 64); err == nil {
			return true
		}
	}
	return false
}

// buildInsertBatches renders rows as multi-row INSERTs, batch rows per
// statement, everything quoted through sqlesc.
func buildInsertBatches(schema, table string, cols []string, rows [][]*string, batch int) ([]string, error) {
	if len(cols) == 0 {
		return nil, errors.New("no columns to insert")
	}
	if len(rows) == 0 {
		return nil, errors.New("no rows to insert")
	}
	quoted := make([]string, len(cols))
	for i, c := range cols {
		quoted[i] = sqlesc.Ident(c)
	}
	prefix := "INSERT INTO " + sqlesc.Ident(schema) + "." + sqlesc.Ident(table) +
		" (" + strings.Join(quoted, ", ") + ") VALUES\n"

	var stmts []string
	var values []string
	for i, row := range rows {
		if len(row) != len(cols) {
			return nil, fmt.Errorf("row %d has %d cells, want %d", i+1, len(row), len(cols))
		}
		cells := make([]string, len(row))
		for j, v := range row {
			cells[j] = sqlesc.NullableValue(v)
		}
		values = append(values, "("+strings.Join(cells, ", ")+")")
		if len(values) == batch {
			stmts = append(stmts, prefix+strings.Join(values, ",\n")+";")
			values = nil
		}
	}
	if len(values) > 0 {
		stmts = append(stmts, prefix+strings.Join(values, ",\n")+";")
	}
	return stmts, nil
}

// ImportRows inserts rows into one table on the connection's shared pool —
// the dialog is not tied to a tab session. One transaction: the first
// failing statement rolls back everything. Returns rows inserted.
func (s *Service) ImportRows(
	connID, schema, table string, cols []string, rows [][]*string,
) (int, error) {
	stmts, err := buildInsertBatches(schema, table, cols, rows, insertBatchRows)
	if err != nil {
		return 0, err
	}
	db, err := conn.Pool(s.conns, connID)
	if err != nil {
		return 0, err
	}

	ctx, cancel := context.WithTimeout(context.Background(), importTimeout)
	defer cancel()
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("begin import transaction: %w", err)
	}
	total := 0
	for _, stmt := range stmts {
		res, err := tx.ExecContext(ctx, stmt)
		if err != nil {
			_ = tx.Rollback()
			return 0, fmt.Errorf("insert failed, rolled back: %w", err)
		}
		if n, err := res.RowsAffected(); err == nil {
			total += int(n)
		}
	}
	if err := tx.Commit(); err != nil {
		return 0, fmt.Errorf("commit import: %w", err)
	}
	return total, nil
}
