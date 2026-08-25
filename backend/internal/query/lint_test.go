package query

import (
	"errors"
	"testing"

	"github.com/go-sql-driver/mysql"
)

func TestParseLintError(t *testing.T) {
	t.Parallel()

	le, ok := parseLintError(&mysql.MySQLError{
		Number: 1064,
		Message: "You have an error in your SQL syntax; check the manual that corresponds " +
			"to your MySQL server version for the right syntax to use near '== 'paid'' at line 4",
	}, 2)
	if !ok {
		t.Fatal("1064 must be reported")
	}
	if le.Statement != 2 || le.Line != 4 || le.Near != "== 'paid'" {
		t.Errorf("position: %+v", le)
	}
	if le.Message != "syntax error near '== 'paid''" {
		t.Errorf("message: %q", le.Message)
	}

	le, ok = parseLintError(&mysql.MySQLError{
		Number:  1064,
		Message: "…for the right syntax to use near '' at line 1",
	}, 0)
	if !ok || le.Line != 1 || le.Message != msgSyntaxAtEnd {
		t.Errorf("end-of-input: %+v ok=%v", le, ok)
	}

	// Semantic and unsupported-statement errors are not lint findings.
	for _, num := range []uint16{1054, 1146, 1295} {
		if _, ok := parseLintError(&mysql.MySQLError{Number: num, Message: "x"}, 0); ok {
			t.Errorf("error %d must be skipped", num)
		}
	}
	if _, ok := parseLintError(errors.New("plain"), 0); ok {
		t.Error("non-mysql errors must be skipped")
	}
}
