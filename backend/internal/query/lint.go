package query

// Live syntax checking. Statements are PREPAREd on a dedicated per-connection
// session — the server's own parser, no third-party grammar — and PREPARE
// never executes. Only true parse errors (1064) are reported: semantic
// errors (unknown column, missing table) depend on the tab's USE state,
// which the lint session does not share, and a linter that cries wolf is
// worse than none.

import (
	"context"
	"errors"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/go-sql-driver/mysql"

	"github.com/stuffz/mybench/internal/conn"
)

// LintError is one parse error, positioned within the statement it came
// from: Line is 1-based within that statement, Near is the server's input
// fragment from the error point onwards (the server truncates long tails).
type LintError struct {
	Statement int    `json:"statement"`
	Line      int    `json:"line"`
	Near      string `json:"near"`
	Message   string `json:"message"`
}

const errParse = 1064 // ER_PARSE_ERROR

// msgSyntaxAtEnd replaces the server's near-” phrasing when the error sits
// at the end of the input.
const msgSyntaxAtEnd = "syntax error at the end of the statement"

var lintNearRe = regexp.MustCompile(`(?s)near '(.*)' at line (\d+)`)

// parseLintError extracts position and a short message from a PREPARE error;
// ok is false for anything that is not a parse error.
func parseLintError(err error, statement int) (LintError, bool) {
	var me *mysql.MySQLError
	if !errors.As(err, &me) || me.Number != errParse {
		return LintError{}, false
	}
	le := LintError{Statement: statement, Message: me.Message}
	if m := lintNearRe.FindStringSubmatch(me.Message); m != nil {
		le.Near = m[1]
		le.Line, _ = strconv.Atoi(m[2])
		if le.Near == "" {
			le.Message = msgSyntaxAtEnd
		} else {
			// The stock message spends a sentence pointing at the manual;
			// the tooltip only needs the finding.
			le.Message = "syntax error near '" + le.Near + "'"
		}
	}
	return le, true
}

// Lint checks every statement and returns the parse errors, indexed by the
// statement's position in the input. Not connected (or any transport
// trouble) means no findings — the editor must not squiggle over a network
// problem.
func (s *Service) Lint(connID string, statements []string) []LintError {
	out := []LintError{}
	if len(statements) == 0 {
		return out
	}
	// One session, one PREPARE at a time.
	s.lintMu.Lock()
	defer s.lintMu.Unlock()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	c, _, err := conn.Session(ctx, s.conns, connID, "@lint")
	if err != nil {
		return out
	}
	for i, stmt := range statements {
		if strings.TrimSpace(stmt) == "" {
			continue
		}
		// PREPARE FROM takes a literal or a user variable, not a
		// placeholder — so the text goes in via one.
		if _, err := c.ExecContext(ctx, "SET @mybench_lint = ?", stmt); err != nil {
			return out
		}
		if _, err := c.ExecContext(ctx, "PREPARE mybench_lint FROM @mybench_lint"); err != nil {
			if le, ok := parseLintError(err, i); ok {
				out = append(out, le)
			}
			continue
		}
		_, _ = c.ExecContext(ctx, "DEALLOCATE PREPARE mybench_lint")
	}
	return out
}
