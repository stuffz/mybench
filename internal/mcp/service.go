// Package mcp exposes a read-only Model Context Protocol server over the
// app's configured connections: agents introspect schemas and run gated
// read-only queries through mybench (Teleport/SSH tunnels included) without
// ever seeing credentials. Off by default; loopback-only with bearer token.
//
// The protocol surface is small and static (initialize / tools/list /
// tools/call over streamable HTTP with plain JSON responses), so it is
// implemented on the stdlib rather than pulling in an SDK.
package mcp

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"net/http"
	"strconv"
	"sync"
	"time"

	"github.com/stuffz/mybench/internal/admin"
	"github.com/stuffz/mybench/internal/conn"
	"github.com/stuffz/mybench/internal/storage"
)

const (
	defaultPort     = 8091
	protocolVersion = "2025-06-18"

	settingEnabled = "mcp_enabled"
	settingPort    = "mcp_port"
	settingToken   = "mcp_token"
)

// Service manages the MCP HTTP listener; bound to the frontend for the
// Preferences panel.
type Service struct {
	conns *conn.Service
	admin *admin.Service
	store *storage.Store

	mu   sync.Mutex
	srv  *http.Server
	port int
}

// Status is what the Preferences panel shows.
type Status struct {
	Enabled bool   `json:"enabled"`
	Port    int    `json:"port"`
	Token   string `json:"token"`
	URL     string `json:"url"`
	Error   string `json:"error,omitempty"`
}

// New loads persisted settings and autostarts the listener if enabled.
func New(conns *conn.Service, adm *admin.Service, store *storage.Store) *Service {
	s := &Service{conns: conns, admin: adm, store: store}
	if _, err := s.token(); err != nil {
		log.Printf("mcp: token: %v", err)
	}
	if enabled, _ := store.GetSetting(settingEnabled); enabled == "1" {
		if _, err := s.Configure(true, s.savedPort()); err != nil {
			log.Printf("mcp: autostart: %v", err)
		}
	}
	return s
}

func (s *Service) savedPort() int {
	if v, _ := s.store.GetSetting(settingPort); v != "" {
		if p, err := strconv.Atoi(v); err == nil && p > 0 && p < 65536 {
			return p
		}
	}
	return defaultPort
}

// token returns the persistent bearer token, creating it on first use.
func (s *Service) token() (string, error) {
	if t, err := s.store.GetSetting(settingToken); err != nil {
		return "", err
	} else if t != "" {
		return t, nil
	}
	buf := make([]byte, 24)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	t := hex.EncodeToString(buf)
	if err := s.store.SetSetting(settingToken, t); err != nil {
		return "", err
	}
	return t, nil
}

// Status reports the current listener state for the Preferences panel.
func (s *Service) Status() (*Status, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.statusLocked(""), nil
}

func (s *Service) statusLocked(errMsg string) *Status {
	t, _ := s.token()
	port := s.port
	if port == 0 {
		port = s.savedPort()
	}
	return &Status{
		Enabled: s.srv != nil,
		Port:    port,
		Token:   t,
		URL:     fmt.Sprintf("http://127.0.0.1:%d/mcp", port),
		Error:   errMsg,
	}
}

// Configure starts or stops the loopback listener and persists the choice.
func (s *Service) Configure(enabled bool, port int) (*Status, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if port <= 0 || port > 65535 {
		port = defaultPort
	}
	_ = s.store.SetSetting(settingPort, strconv.Itoa(port))
	if enabled {
		_ = s.store.SetSetting(settingEnabled, "1")
	} else {
		_ = s.store.SetSetting(settingEnabled, "0")
	}

	// Stop the current listener if the desired state differs.
	if s.srv != nil && (!enabled || s.port != port) {
		_ = s.srv.Close()
		s.srv = nil
		s.port = 0
	}
	if !enabled || s.srv != nil {
		return s.statusLocked(""), nil
	}

	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return s.statusLocked(err.Error()), nil
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/mcp", s.handle)
	srv := &http.Server{Handler: mux, ReadHeaderTimeout: 10 * time.Second}
	s.srv = srv
	s.port = port
	go func() {
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			log.Printf("mcp: serve: %v", err)
		}
	}()
	log.Printf("mcp: listening on 127.0.0.1:%d", port)
	return s.statusLocked(""), nil
}

// RegenerateToken invalidates the old bearer token.
func (s *Service) RegenerateToken() (*Status, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	buf := make([]byte, 24)
	if _, err := rand.Read(buf); err != nil {
		return nil, err
	}
	if err := s.store.SetSetting(settingToken, hex.EncodeToString(buf)); err != nil {
		return nil, err
	}
	return s.statusLocked(""), nil
}

// --- JSON-RPC over streamable HTTP -------------------------------------

type rpcRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

type rpcResponse struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      json.RawMessage `json:"id"`
	Result  any             `json:"result,omitempty"`
	Error   *rpcError       `json:"error,omitempty"`
}

func (s *Service) handle(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	t, err := s.token()
	if err != nil || r.Header.Get("Authorization") != "Bearer "+t {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}

	var req rpcRequest
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<20)).Decode(&req); err != nil {
		writeRPC(w, rpcResponse{JSONRPC: "2.0", Error: &rpcError{Code: -32700, Message: "parse error"}})
		return
	}

	// Notifications (no id) are acknowledged without a body.
	if len(req.ID) == 0 || string(req.ID) == "null" {
		w.WriteHeader(http.StatusAccepted)
		return
	}

	resp := rpcResponse{JSONRPC: "2.0", ID: req.ID}
	switch req.Method {
	case "initialize":
		w.Header().Set("Mcp-Session-Id", "mybench")
		resp.Result = map[string]any{
			"protocolVersion": protocolVersion,
			"capabilities":    map[string]any{"tools": map[string]any{}},
			"serverInfo":      map[string]any{"name": "mybench", "version": "0.1"},
		}
	case "ping":
		resp.Result = map[string]any{}
	case "tools/list":
		resp.Result = map[string]any{"tools": toolDefs()}
	case "tools/call":
		result, err := s.callTool(req.Params)
		if err != nil {
			// Tool-level failures are results with isError, not RPC errors.
			result = toolText(fmt.Sprintf("error: %v", err), true)
		}
		resp.Result = result
	default:
		resp.Error = &rpcError{Code: -32601, Message: "method not found: " + req.Method}
	}
	writeRPC(w, resp)
}

func writeRPC(w http.ResponseWriter, resp rpcResponse) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(resp)
}
