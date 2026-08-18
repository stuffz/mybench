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
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
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
}

// method returns the connection method, migrating pre-Method profiles.
func (c *SavedConn) method() string {
	if c.Method != "" {
		return c.Method
	}
	if c.Teleport {
		return "teleport"
	}
	return "tcp"
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
		open:      map[string]*openConn{},
		secrets:   newSecretStore(dir),
		configDir: dir,
		tunnels:   &TunnelMgr{},
	}
	_ = s.loadFile() // missing file on first launch is fine
	return s
}

func (s *Service) file() string {
	return filepath.Join(s.configDir, "connections.json")
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

// List returns the saved connection profiles.
func (s *Service) List() []SavedConn {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]SavedConn, len(s.saved))
	copy(out, s.saved)
	return out
}

// Save upserts a connection; a non-empty password goes to the secret store,
// never to disk. An empty password keeps whatever is already stored.
func (s *Service) Save(c SavedConn, password string) (*SavedConn, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if c.Name == "" || c.User == "" {
		return nil, errors.New("name and user are required")
	}
	c.Method = c.method()
	c.Teleport = c.Method == "teleport"
	switch c.Method {
	case "teleport":
		if c.TeleportDB == "" {
			return nil, errors.New("teleport connections need a database resource name")
		}
	case "ssh":
		if c.SSHHost == "" {
			return nil, errors.New("ssh connections need an SSH host")
		}
		if c.SSHPort == 0 {
			c.SSHPort = 22
		}
		if c.Host == "" {
			c.Host = "127.0.0.1" // MySQL on the SSH host itself is the common case
		}
	default:
		if c.Host == "" {
			return nil, errors.New("host is required")
		}
	}
	if c.Port == 0 {
		c.Port = 3306
	}
	if c.TLSMode == "" {
		c.TLSMode = tlsPreferred
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
	s.mu.Unlock()
	if saved == nil {
		return nil, fmt.Errorf("unknown connection %q", id)
	}

	host, port := saved.Host, saved.Port
	method := saved.method()
	var tun *tunnel
	switch method {
	case "teleport":
		t, err := s.tunnels.start(saved.TeleportDB, saved.User, saved.Database)
		if err != nil {
			return nil, err
		}
		tun = t
		host, port = "127.0.0.1", t.port
	case "ssh":
		t, err := s.tunnels.startSSH(*saved)
		if err != nil {
			return nil, err
		}
		tun = t
		host, port = "127.0.0.1", t.port
	}

	password, err := s.secrets.GetSecret(id)
	if err != nil {
		// Teleport tunnels authenticate with tsh certs; MySQL behind them
		// usually needs no password at all.
		if method == "teleport" {
			password = ""
		} else {
			if tun != nil {
				tun.stop()
			}
			return nil, fmt.Errorf("no stored password for %q — edit the connection and set one", saved.Name)
		}
	}

	cfg := mysql.NewConfig()
	cfg.User = saved.User
	cfg.Passwd = password
	cfg.Net = "tcp"
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

	connector, err := mysql.NewConnector(cfg)
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

// Close tears down a connection's sessions, pool and tunnel.
func (s *Service) Close(id string) {
	s.mu.Lock()
	oc := s.open[id]
	delete(s.open, id)
	s.mu.Unlock()
	if oc == nil {
		return
	}
	oc.mu.Lock()
	for _, sess := range oc.sessions {
		_ = sess.Close()
	}
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
