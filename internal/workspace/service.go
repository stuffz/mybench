// Package workspace persists the frontend's session state (open tabs,
// editor contents, prefs, layout) so the app reopens where it left off.
package workspace

// The payload is an opaque JSON blob owned by the frontend store — the
// backend only gives it a durable home. Since the SQLite move the blob lives
// in mybench.db (written through on every change); a pre-existing
// workspace.json is imported once and left in place as a backup.

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"

	"github.com/stuffz/mybench/internal/storage"
)

// Service loads and saves the workspace blob in the SQLite store.
type Service struct {
	mu         sync.Mutex
	store      *storage.Store
	legacyPath string
}

// New wires the service to the store.
func New(store *storage.Store) *Service {
	dir, err := os.UserConfigDir()
	if err != nil {
		dir = "."
	}
	return &Service{store: store, legacyPath: filepath.Join(dir, "mybench", "workspace.json")}
}

// Load returns the saved workspace JSON, or "" on first launch. An empty
// store falls back to the legacy workspace.json once and imports it.
func (s *Service) Load() (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	blob, err := s.store.LoadWorkspace()
	if err != nil {
		return "", err
	}
	if blob != "" {
		return blob, nil
	}
	legacy, err := os.ReadFile(s.legacyPath)
	if os.IsNotExist(err) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("read legacy workspace: %w", err)
	}
	if err := s.store.SaveWorkspace(string(legacy)); err != nil {
		return "", err
	}
	return string(legacy), nil
}

// Save writes the workspace JSON through to SQLite.
func (s *Service) Save(data string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.store.SaveWorkspace(data)
}
