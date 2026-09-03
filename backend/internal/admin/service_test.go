package admin

import (
	"context"
	"database/sql"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/go-sql-driver/mysql"
)

// TestProcesslistFullInfo proves Info carries the whole statement: the
// performance_schema table clips it at 1024 bytes, which cut off exactly the
// long queries the Client Connections panel exists to show.
func TestProcesslistFullInfo(t *testing.T) {
	t.Parallel()
	env := os.Getenv("MYBENCH_TEST_DSN")
	if env == "" {
		t.Skip("MYBENCH_TEST_DSN not set")
	}
	cfg, err := mysql.ParseDSN(env)
	if err != nil {
		t.Fatal(err)
	}
	connector, err := mysql.NewConnector(cfg)
	if err != nil {
		t.Fatal(err)
	}
	db := sql.OpenDB(connector)
	defer func() { _ = db.Close() }()

	const marker = "SELECT SLEEP(3) /* processlist-full-info "
	long := marker + strings.Repeat("x", 3000) + " */" //nolint:gosec // test statement, no user input
	done := make(chan error, 1)
	go func() {
		_, err := db.ExecContext(context.Background(), long)
		done <- err
	}()
	defer func() {
		if err := <-done; err != nil {
			t.Error(err)
		}
	}()

	deadline := time.Now().Add(2 * time.Second)
	for {
		rows, err := readProcesslist(context.Background(), db)
		if err != nil {
			t.Fatal(err)
		}
		for _, p := range rows {
			if !strings.HasPrefix(p.Info, marker) {
				continue
			}
			if p.Info != long {
				t.Fatalf("Info truncated to %d of %d bytes", len(p.Info), len(long))
			}
			return
		}
		if time.Now().After(deadline) {
			t.Fatal("long statement never showed up in the processlist")
		}
		time.Sleep(50 * time.Millisecond)
	}
}
