// Package conn manages saved and open MySQL connections: config on disk
// (passwords in the OS keyring), one pool + optional tsh tunnel per open
// connection, and dedicated per-tab sessions.
package conn

// Service: saved connections (JSON under the user config dir, never
// with passwords — those go to the SecretStore), open connections (one
// sql.DB pool + optional tsh tunnel each), and per-editor-tab sessions
// (dedicated sql.Conn, opened lazily — pools break USE/SET/transactions).
// Everything downstream is keyed by connID (SPEC.md).

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"encoding/json"
	"errors"
	"fmt"
	"hash/fnv"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"sync"
	"time"

	"github.com/go-sql-driver/mysql"
)

const tlsPreferred = "preferred"

// SavedConn is one stored connection profile; never carries a password.
type SavedConn struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Color    string `json:"color"`  // accent override (hex); empty = auto hue
	Method   string `json:"method"` // tcp | ssh | teleport
	Host     string `json:"host"`   // MySQL host (for ssh: as seen from the SSH host)
	Port     int    `json:"port"`
	User     string `json:"user"`
	Database string `json:"database"`
	TLSMode  string `json:"tlsMode"` // disabled | preferred | required
	// ssh method: local port-forward through the system ssh client.
	SSHHost    string `json:"sshHost"`
	SSHPort    int    `json:"sshPort"`
	SSHUser    string `json:"sshUser"`
	SSHKeyFile string `json:"sshKeyFile"`
	// teleport method.
	Teleport   bool   `json:"teleport"`   // legacy flag, migrated to Method on load
	TeleportDB string `json:"teleportDb"` // teleport database resource name
	// Quick-connect profile: lives in quick.json (not connections.json),
	// hidden from the connections dialog; its resource+user-derived id
	// anchors the workspace's saved tabs across reconnects (SaveQuick).
	Ephemeral bool `json:"ephemeral,omitempty"`
}

// Connection methods (SavedConn.Method). methodTCP doubles as the
// go-sql-driver network name — the same literal on purpose.
const (
	methodTCP      = "tcp"
	methodSSH      = "ssh"
	methodTeleport = "teleport"
)

// localhost is the default MySQL host over a tunnel and the only address
// tunnels bind. It doubles as the default host for new profiles.
const localhost = "127.0.0.1"

// defaultUser fills an empty user on save — root is what a fresh local
// server has.
const defaultUser = "root"

// method returns the connection method, migrating pre-Method profiles.
func (c *SavedConn) method() string {
	if c.Method != "" {
		return c.Method
	}
	if c.Teleport {
		return methodTeleport
	}
	return methodTCP
}

type openConn struct {
	saved  SavedConn
	db     *sql.DB
	tunnel *tunnel

	mu       sync.Mutex
	sessions map[string]*sql.Conn // tabID → dedicated session
}

// State reports a connection's open/error state to the frontend.
type State struct {
	ID    string `json:"id"`
	Open  bool   `json:"open"`
	Error string `json:"error"`
}

// Service owns saved profiles, open pools and per-tab sessions.
type Service struct {
	mu        sync.Mutex
	saved     []SavedConn
	quick     map[string]SavedConn // ephemeral quick-connect profiles, by id
	open      map[string]*openConn
	secrets   SecretStore
	configDir string
	tunnels   *TunnelMgr
}

// New loads saved connections and probes the secret store.
func New() *Service {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = "."
	}
	dir = filepath.Join(dir, "mybench")
	s := &Service{
		quick:     map[string]SavedConn{},
		open:      map[string]*openConn{},
		secrets:   newSecretStore(dir),
		configDir: dir,
		tunnels:   &TunnelMgr{},
	}
	_ = s.loadFile()      // missing file on first launch is fine
	_ = s.loadQuickFile() // same
	return s
}

func (s *Service) file() string {
	return filepath.Join(s.configDir, "connections.json")
}

// quick.json keeps the quick-connect profiles apart from the user-curated
// connections.json: it is rebuildable cache with its own lifecycle, and
// connections.json's array order is load-bearing (the dialog's display
// order) — mixing cache entries in would make every writer preserve them.
func (s *Service) quickFile() string {
	return filepath.Join(s.configDir, "quick.json")
}

func (s *Service) loadQuickFile() error {
	data, err := os.ReadFile(s.quickFile())
	if err != nil {
		return fmt.Errorf("read quick profiles: %w", err)
	}
	var list []SavedConn
	if err := json.Unmarshal(data, &list); err != nil {
		return fmt.Errorf("parse quick profiles: %w", err)
	}
	for _, c := range list {
		s.quick[c.ID] = c
	}
	return nil
}

// saveQuickFile is called with s.mu held.
func (s *Service) saveQuickFile() error {
	list := make([]SavedConn, 0, len(s.quick))
	for _, c := range s.quick {
		list = append(list, c)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].ID < list[j].ID })
	data, err := json.MarshalIndent(list, "", "  ")
	if err != nil {
		return fmt.Errorf("encode quick profiles: %w", err)
	}
	if err := os.MkdirAll(s.configDir, 0o700); err != nil {
		return fmt.Errorf("create config dir: %w", err)
	}
	if err := os.WriteFile(s.quickFile(), data, 0o600); err != nil {
		return fmt.Errorf("write quick profiles: %w", err)
	}
	return nil
}

func (s *Service) loadFile() error {
	data, err := os.ReadFile(s.file())
	if err != nil {
		return fmt.Errorf("read connections: %w", err)
	}
	if err := json.Unmarshal(data, &s.saved); err != nil {
		return fmt.Errorf("parse connections: %w", err)
	}
	for i := range s.saved {
		s.saved[i].Method = s.saved[i].method()
	}
	return nil
}

func (s *Service) saveFile() error {
	data, err := json.MarshalIndent(s.saved, "", "  ")
	if err != nil {
		return fmt.Errorf("encode connections: %w", err)
	}
	if err := os.MkdirAll(s.configDir, 0o700); err != nil {
		return fmt.Errorf("create config dir: %w", err)
	}
	if err := os.WriteFile(s.file(), data, 0o600); err != nil {
		return fmt.Errorf("write connections: %w", err)
	}
	return nil
}

// List returns the saved profiles, then any live quick-connect ones (flagged
// ephemeral) — the shell needs those to label tabs; the dialog hides them.
func (s *Service) List() []SavedConn {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]SavedConn, len(s.saved), len(s.saved)+len(s.quick))
	copy(out, s.saved)
	for _, c := range s.quick {
		out = append(out, c)
	}
	sort.SliceStable(out[len(s.saved):], func(i, j int) bool {
		q := out[len(s.saved):]
		return q[i].ID < q[j].ID
	})
	return out
}

// Save upserts a connection; a non-empty password goes to the secret store,
// never to disk. An empty password keeps whatever is already stored. Empty
// host, user and name get defaults (localhost, root, a derived name).
func (s *Service) Save(c SavedConn, password string) (*SavedConn, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c.Method = c.method()
	c.Teleport = c.Method == methodTeleport
	c.Ephemeral = false // saved is the opposite of ephemeral
	switch c.Method {
	case methodTeleport:
		if c.TeleportDB == "" {
			return nil, errors.New("teleport connections need a database resource name")
		}
	case methodSSH:
		if c.SSHHost == "" {
			return nil, errors.New("ssh connections need an SSH host")
		}
		if c.SSHPort == 0 {
			c.SSHPort = 22
		}
		if c.Host == "" {
			c.Host = localhost // MySQL on the SSH host itself is the common case
		}
	default:
		if c.Host == "" {
			c.Host = localhost
		}
	}
	if c.User == "" {
		c.User = defaultUser
	}
	if c.Port == 0 {
		c.Port = 3306
	}
	if c.TLSMode == "" {
		c.TLSMode = tlsPreferred
	}
	if c.Name == "" {
		c.Name = autoName(&c)
	}
	if c.ID == "" {
		c.ID = "c" + strconv.FormatInt(time.Now().UnixNano(), 36)
	}
	found := false
	for i := range s.saved {
		if s.saved[i].ID == c.ID {
			s.saved[i] = c
			found = true
			break
		}
	}
	if !found {
		s.saved = append(s.saved, c)
	}
	if password != "" {
		if err := s.secrets.SetSecret(c.ID, password); err != nil {
			return nil, fmt.Errorf("store password: %w", err)
		}
	}
	if err := s.saveFile(); err != nil {
		return nil, err
	}
	return &c, nil
}

// autoName labels an unnamed profile with the same connection summary the
// dialog shows under a name. Called after Save's defaults have been applied.
func autoName(c *SavedConn) string {
	switch c.Method {
	case methodTeleport:
		return fmt.Sprintf("%s@%s", c.User, c.TeleportDB)
	case methodSSH:
		return fmt.Sprintf("%s@%s via ssh %s@%s", c.User, c.Host, c.SSHUser, c.SSHHost)
	default:
		return fmt.Sprintf("%s@%s:%d", c.User, c.Host, c.Port)
	}
}

// quickID derives the profile id from resource+user, so reconnecting lands on
// the same connection id and the workspace restores that identity's tabs —
// while reader and admin on one resource stay two openable connections. The
// database is deliberately not part of the id: a changed default schema
// updates the profile in place (SaveQuick) instead of orphaning the tabs.
func quickID(teleportDB, user string) string {
	h := fnv.New64a()
	_, _ = h.Write([]byte(teleportDB + "\x00" + user))
	return "q" + strconv.FormatUint(h.Sum64(), 36)
}

// SaveQuick registers (or refreshes) a quick-connect Teleport profile in
// quick.json — never the dialog's connections.json; the Ephemeral flag keeps
// it out of the dialog. The profile outlives its connection (Close keeps it)
// so the workspace's saved query tabs come back on the next quick connect.
func (s *Service) SaveQuick(teleportDB, user, database string) (*SavedConn, error) {
	if teleportDB == "" {
		return nil, errors.New("quick connect needs a teleport database resource")
	}
	if user == "" {
		user = defaultUser
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	id := quickID(teleportDB, user)
	c, ok := s.quick[id]
	if !ok {
		c = SavedConn{
			ID:         id,
			Method:     methodTeleport,
			Teleport:   true,
			TeleportDB: teleportDB,
			User:       user,
			Port:       3306,
			TLSMode:    tlsPreferred,
			Ephemeral:  true,
		}
		c.Name = autoName(&c)
	}
	c.Database = database
	s.quick[id] = c
	if err := s.saveQuickFile(); err != nil {
		return nil, err
	}
	return &c, nil
}

// Reorder rearranges the saved profiles to match ids. Profiles the list does
// not mention keep their relative order after the mentioned ones, so a stale
// client can never drop one. The file's array order IS the display order —
// this is what the dialog's move arrows persist.
func (s *Service) Reorder(ids []string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	byID := make(map[string]int, len(s.saved))
	for i := range s.saved {
		byID[s.saved[i].ID] = i
	}
	next := make([]SavedConn, 0, len(s.saved))
	taken := make(map[string]bool, len(ids))
	for _, id := range ids {
		if i, ok := byID[id]; ok && !taken[id] {
			next = append(next, s.saved[i])
			taken[id] = true
		}
	}
	for i := range s.saved {
		if !taken[s.saved[i].ID] {
			next = append(next, s.saved[i])
		}
	}
	s.saved = next
	return s.saveFile()
}

// HasPassword reports whether a secret is stored for this profile, so the UI
// can say "none stored" instead of the misleading "unchanged".
func (s *Service) HasPassword(id string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	pw, err := s.secrets.GetSecret(id)
	return err == nil && pw != ""
}

// SetPassword stores a password, or removes the stored one when given an empty
// string. Save deliberately treats an empty password as "leave it alone" — so a
// dialog can round-trip a profile without knowing the secret — which leaves no
// way to take a password back off. This is that way.
func (s *Service) SetPassword(id, password string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	found := false
	for i := range s.saved {
		if s.saved[i].ID == id {
			found = true
			break
		}
	}
	if !found {
		return fmt.Errorf("no such connection %q", id)
	}
	if password == "" {
		// A missing secret is the desired end state, so a delete that finds
		// nothing is success, not an error.
		_ = s.secrets.DeleteSecret(id)
		return nil
	}
	if err := s.secrets.SetSecret(id, password); err != nil {
		return fmt.Errorf("store password: %w", err)
	}
	return nil
}

// Delete removes a saved (and closed) connection and its secret.
func (s *Service) Delete(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.open[id] != nil {
		return errors.New("close the connection before deleting it")
	}
	for i := range s.saved {
		if s.saved[i].ID == id {
			s.saved = append(s.saved[:i], s.saved[i+1:]...)
			_ = s.secrets.DeleteSecret(id)
			return s.saveFile()
		}
	}
	return fmt.Errorf("unknown connection %q", id)
}

// connCharset is pinned with an explicit SET NAMES on every new connection.
// Without it the driver only sends a collation byte in the handshake, which
// servers running skip-character-set-client-handshake/init_connect and
// protocol-terminating proxies (Teleport) ignore — the session then falls
// back to the server charset and non-ASCII text is mangled in both
// directions (reads show U+FFFD, writes store mojibake).
const connCharset = "utf8mb4"

// poolConfig builds the driver config for a saved connection's pool.
func poolConfig(saved *SavedConn, password, host string, port int) (*mysql.Config, error) {
	cfg := mysql.NewConfig()
	if err := cfg.Apply(mysql.Charset(connCharset, "")); err != nil {
		return nil, fmt.Errorf("set charset: %w", err)
	}
	cfg.User = saved.User
	cfg.Passwd = password
	cfg.Net = methodTCP
	cfg.Addr = net.JoinHostPort(host, strconv.Itoa(port))
	cfg.DBName = saved.Database
	switch saved.TLSMode {
	case "required":
		cfg.TLSConfig = "true"
	case "disabled":
		cfg.TLSConfig = "false"
	default:
		cfg.TLSConfig = tlsPreferred
	}
	return cfg, nil
}

// Open builds the pool (through a tsh tunnel for Teleport connections) and
// verifies it with a ping.
func (s *Service) Open(id string) (*State, error) {
	s.mu.Lock()
	if s.open[id] != nil {
		s.mu.Unlock()
		return &State{ID: id, Open: true}, nil
	}
	var saved *SavedConn
	for i := range s.saved {
		if s.saved[i].ID == id {
			saved = &s.saved[i]
			break
		}
	}
	if saved == nil {
		if q, ok := s.quick[id]; ok {
			saved = &q
		}
	}
	s.mu.Unlock()
	if saved == nil {
		return nil, fmt.Errorf("unknown connection %q", id)
	}

	host, port := saved.Host, saved.Port
	method := saved.method()
	var tun *tunnel
	switch method {
	case methodTeleport:
		t, err := s.tunnels.start(saved.TeleportDB, saved.User, saved.Database)
		if err != nil {
			return nil, err
		}
		tun = t
		host, port = localhost, t.port
	case methodSSH:
		t, err := s.tunnels.startSSH(*saved)
		if err != nil {
			return nil, err
		}
		tun = t
		host, port = localhost, t.port
	}

	// No stored secret is not a reason to refuse: Teleport tunnels
	// authenticate with tsh certs, plenty of local servers have a
	// passwordless account, and where a password really is required the
	// server's own "Access denied ... (using password: NO)" says so far more
	// precisely than we can guess beforehand.
	password, err := s.secrets.GetSecret(id)
	if err != nil {
		password = ""
	}

	var connector driver.Connector
	cfg, err := poolConfig(saved, password, host, port)
	if err == nil {
		connector, err = mysql.NewConnector(cfg)
	}
	if err != nil {
		if tun != nil {
			tun.stop()
		}
		return nil, fmt.Errorf("build connector: %w", err)
	}
	db := sql.OpenDB(connector)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := db.PingContext(ctx); err != nil {
		_ = db.Close()
		if tun != nil {
			tun.stop()
		}
		return nil, fmt.Errorf("cannot reach %s: %w", saved.Name, err)
	}

	s.mu.Lock()
	s.open[id] = &openConn{saved: *saved, db: db, tunnel: tun, sessions: map[string]*sql.Conn{}}
	s.mu.Unlock()
	return &State{ID: id, Open: true}, nil
}

// Close tears down a connection's sessions, pool and tunnel. A quick-connect
// profile survives on purpose: its id anchors the workspace's saved tabs.
func (s *Service) Close(id string) {
	s.mu.Lock()
	oc := s.open[id]
	delete(s.open, id)
	s.mu.Unlock()
	if oc != nil {
		oc.teardown()
	}
}

// ServiceShutdown runs when the backend exits (the GUI closing its stdin):
// close every open pool and kill its tsh/ssh tunnel so no child process
// outlives the app.
func (s *Service) ServiceShutdown() error {
	s.mu.Lock()
	open := s.open
	s.open = map[string]*openConn{}
	s.mu.Unlock()
	// Kill tunnels first: the session/pool closes in teardown block until
	// in-flight queries finish, and shutdown should not wait on them. A dead
	// tunnel errors those queries out immediately, and the children are gone
	// even if a close still hangs.
	for _, oc := range open {
		if oc.tunnel != nil {
			oc.tunnel.stop()
		}
	}
	for _, oc := range open {
		oc.teardown()
	}
	return nil
}

func (oc *openConn) teardown() {
	oc.mu.Lock()
	for _, sess := range oc.sessions {
		_ = sess.Close()
	}
	oc.sessions = map[string]*sql.Conn{}
	oc.mu.Unlock()
	_ = oc.db.Close()
	if oc.tunnel != nil {
		oc.tunnel.stop()
	}
}

func (s *Service) get(id string) (*openConn, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	oc := s.open[id]
	if oc == nil {
		return nil, fmt.Errorf("connection %q is not open", id)
	}
	return oc, nil
}

// Pool exposes an open connection's shared pool (panels, schema queries).
// Package-level on purpose: exported *methods* get bound to the frontend.
func Pool(s *Service, id string) (*sql.DB, error) {
	oc, err := s.get(id)
	if err != nil {
		return nil, err
	}
	return oc.db, nil
}

// Session returns the tab's dedicated connection on connID, opening it
// lazily; the bool reports a recreated (reset) session.
func Session(ctx context.Context, s *Service, connID, tabID string) (*sql.Conn, bool, error) {
	oc, err := s.get(connID)
	if err != nil {
		return nil, false, err
	}
	return oc.session(ctx, tabID)
}

// CloseSession releases a tab's dedicated session.
func CloseSession(s *Service, connID, tabID string) {
	if oc, err := s.get(connID); err == nil {
		oc.closeSession(tabID)
	}
}

// session returns the tab's dedicated connection, opening it lazily. The
// second return reports whether a previously-open session had to be replaced
// (session state was lost — the UI must surface that).
func (oc *openConn) session(ctx context.Context, tabID string) (*sql.Conn, bool, error) {
	oc.mu.Lock()
	defer oc.mu.Unlock()
	if c := oc.sessions[tabID]; c != nil {
		if err := c.PingContext(ctx); err == nil {
			return c, false, nil
		}
		_ = c.Close()
		delete(oc.sessions, tabID)
		c2, err := oc.db.Conn(ctx)
		if err != nil {
			return nil, true, fmt.Errorf("reopen session: %w", err)
		}
		oc.sessions[tabID] = c2
		return c2, true, nil
	}
	c, err := oc.db.Conn(ctx)
	if err != nil {
		return nil, false, fmt.Errorf("open session: %w", err)
	}
	oc.sessions[tabID] = c
	return c, false, nil
}

func (oc *openConn) closeSession(tabID string) {
	oc.mu.Lock()
	if c := oc.sessions[tabID]; c != nil {
		_ = c.Close()
		delete(oc.sessions, tabID)
	}
	oc.mu.Unlock()
}
