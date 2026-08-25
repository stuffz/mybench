package conn

import (
	"context"
	"database/sql"
	"net"
	"os"
	"strconv"
	"strings"
	"testing"

	"github.com/go-sql-driver/mysql"
)

// The pool must pin the connection charset with an explicit SET NAMES (the
// charset DSN param). Relying on the handshake collation byte alone breaks
// behind servers with skip-character-set-client-handshake/init_connect and
// protocol-terminating proxies (Teleport): the session falls back to the
// server charset and non-ASCII text is mangled in both directions.
func TestPoolConfigCharset(t *testing.T) {
	t.Parallel()
	cfg, err := poolConfig(&SavedConn{User: "u", Database: "d"}, "", localhost, 3306)
	if err != nil {
		t.Fatal(err)
	}
	if dsn := cfg.FormatDSN(); !strings.Contains(dsn, "charset=utf8mb4") {
		t.Fatalf("connection charset not pinned to utf8mb4: %s", dsn)
	}
}

// TestLatin1RoundTrip proves åäö survives a latin1 table through the pinned
// utf8mb4 session: the server converts at the charset boundary both ways.
func TestLatin1RoundTrip(t *testing.T) {
	t.Parallel()
	env := os.Getenv("MYBENCH_TEST_DSN")
	if env == "" {
		t.Skip("MYBENCH_TEST_DSN not set")
	}
	base, err := mysql.ParseDSN(env)
	if err != nil {
		t.Fatal(err)
	}
	host, portStr, err := net.SplitHostPort(base.Addr)
	if err != nil {
		t.Fatal(err)
	}
	port, err := strconv.Atoi(portStr)
	if err != nil {
		t.Fatal(err)
	}

	cfg, err := poolConfig(&SavedConn{User: base.User}, base.Passwd, host, port)
	if err != nil {
		t.Fatal(err)
	}
	connector, err := mysql.NewConnector(cfg)
	if err != nil {
		t.Fatal(err)
	}
	db := sql.OpenDB(connector)
	defer func() { _ = db.Close() }()
	ctx := context.Background()

	var client, conn, results string
	err = db.QueryRowContext(ctx,
		"SELECT @@character_set_client, @@character_set_connection, @@character_set_results").
		Scan(&client, &conn, &results)
	if err != nil {
		t.Fatal(err)
	}
	for name, got := range map[string]string{
		"character_set_client": client, "character_set_connection": conn, "character_set_results": results,
	} {
		if got != "utf8mb4" {
			t.Errorf("%s = %q, want utf8mb4", name, got)
		}
	}

	const schema = "mybench_conn_test"
	for _, stmt := range []string{
		"DROP DATABASE IF EXISTS " + schema,
		"CREATE DATABASE " + schema + " CHARACTER SET latin1 COLLATE latin1_swedish_ci",
		"CREATE TABLE " + schema + ".t (id INT PRIMARY KEY, s VARCHAR(80)) CHARACTER SET latin1",
	} {
		if _, err := db.ExecContext(ctx, stmt); err != nil {
			t.Fatal(err)
		}
	}
	defer func() { _, _ = db.ExecContext(ctx, "DROP DATABASE IF EXISTS "+schema) }()

	const sample = "Åsa Öberg (Växjö): åäö ÅÄÖ é"
	if _, err := db.ExecContext(ctx, "INSERT INTO "+schema+".t VALUES (1, ?)", sample); err != nil {
		t.Fatal(err)
	}
	var got string
	if err := db.QueryRowContext(ctx, "SELECT s FROM "+schema+".t WHERE id = 1").Scan(&got); err != nil {
		t.Fatal(err)
	}
	if got != sample {
		t.Errorf("latin1 round-trip: got %q, want %q", got, sample)
	}
}

// Save fills in the friendly defaults: host 127.0.0.1, user root, and a name
// derived from the connection details when none is given.
func TestSaveDefaults(t *testing.T) {
	t.Parallel()
	s := &Service{configDir: t.TempDir(), open: map[string]*openConn{}, quick: map[string]SavedConn{}}

	c, err := s.Save(SavedConn{Method: methodTCP}, "")
	if err != nil {
		t.Fatal(err)
	}
	if c.Host != localhost || c.User != defaultUser || c.Port != 3306 {
		t.Fatalf("tcp defaults: %+v", c)
	}
	if c.Name != "root@127.0.0.1:3306" {
		t.Fatalf("tcp auto name: %q", c.Name)
	}

	c, err = s.Save(SavedConn{Method: methodSSH, SSHHost: "jump", SSHUser: "ops", User: "app"}, "")
	if err != nil {
		t.Fatal(err)
	}
	if c.Name != "app@127.0.0.1 via ssh ops@jump" {
		t.Fatalf("ssh auto name: %q", c.Name)
	}

	c, err = s.Save(SavedConn{Method: methodTeleport, TeleportDB: "prod-db"}, "")
	if err != nil {
		t.Fatal(err)
	}
	if c.Name != "root@prod-db" {
		t.Fatalf("teleport auto name: %q", c.Name)
	}

	// A given name always wins over the derived one.
	c, err = s.Save(SavedConn{Method: methodTCP, Name: "mine", Host: "h", User: "u"}, "")
	if err != nil {
		t.Fatal(err)
	}
	if c.Name != "mine" {
		t.Fatalf("explicit name overridden: %q", c.Name)
	}
}

// SaveQuick registers an in-memory profile: listed (flagged ephemeral) so the
// shell can label its tab, never written to disk, gone once closed.
func TestSaveQuick(t *testing.T) {
	t.Parallel()
	s := &Service{configDir: t.TempDir(), open: map[string]*openConn{}, quick: map[string]SavedConn{}}

	c, err := s.SaveQuick("prod-db", "reader", "app")
	if err != nil {
		t.Fatal(err)
	}
	if c.ID == "" || !c.Ephemeral || c.Method != methodTeleport {
		t.Fatalf("quick profile: %+v", c)
	}
	if c.Name != "reader@prod-db" {
		t.Fatalf("quick auto name: %q", c.Name)
	}

	// Same resource+user+database returns the same profile — a failed open
	// retried must not pile up entries.
	c2, err := s.SaveQuick("prod-db", "reader", "app")
	if err != nil {
		t.Fatal(err)
	}
	if c2.ID != c.ID {
		t.Fatalf("duplicate quick profile: %q vs %q", c2.ID, c.ID)
	}

	list := s.List()
	if len(list) != 1 || !list[0].Ephemeral {
		t.Fatalf("quick profile missing from List: %+v", list)
	}
	// Nothing may reach the connections file.
	if _, err := os.Stat(s.file()); !os.IsNotExist(err) {
		t.Fatalf("quick profile touched disk: %v", err)
	}

	s.Close(c.ID)
	if len(s.List()) != 0 {
		t.Fatal("quick profile survived Close")
	}

	if _, err := s.SaveQuick("", "reader", ""); err == nil {
		t.Fatal("empty resource must be refused")
	}
}

func TestReorder(t *testing.T) {
	t.Parallel()
	s := &Service{
		configDir: t.TempDir(),
		saved:     []SavedConn{{ID: "a"}, {ID: "b"}, {ID: "c"}},
	}
	if err := s.Reorder([]string{"c", "a"}); err != nil {
		t.Fatal(err)
	}
	// Mentioned ids lead in the given order; unmentioned ones follow in
	// their old relative order.
	got := s.List()
	want := []string{"c", "a", "b"}
	for i, w := range want {
		if got[i].ID != w {
			t.Fatalf("order: got %v at %d, want %v", got[i].ID, i, w)
		}
	}
	// Unknown and duplicate ids are ignored, nothing is lost.
	if err := s.Reorder([]string{"b", "b", "nope"}); err != nil {
		t.Fatal(err)
	}
	if got := s.List(); len(got) != 3 || got[0].ID != "b" {
		t.Fatalf("after dedupe: %v", got)
	}
}
