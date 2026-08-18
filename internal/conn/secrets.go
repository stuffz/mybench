package conn

// Passwords live in the OS keyring (SPEC.md security). Inside the
// devcontainer there is no Secret Service, so a file fallback exists for
// development only — it stores secrets in plaintext under the config dir and
// says so loudly on first use.

import (
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"sync"

	"github.com/zalando/go-keyring"
)

const keyringService = "mybench"

// SecretStore stores connection passwords out of band of the config file.
type SecretStore interface {
	SetSecret(key, value string) error
	GetSecret(key string) (string, error)
	DeleteSecret(key string) error
}

// newSecretStore probes the OS keyring and falls back to the dev file store.
//
//nolint:ireturn // factory deliberately returns the SecretStore interface — keyring or dev file fallback
func newSecretStore(configDir string) SecretStore {
	probe := "mybench-probe"
	if err := keyring.Set(keyringService, probe, "ok"); err == nil {
		_ = keyring.Delete(keyringService, probe)
		return keyringStore{}
	}
	slog.Warn("OS keyring unavailable — falling back to PLAINTEXT secret file; fine in the devcontainer, not on a real desktop",
		"path", filepath.Join(configDir, "secrets.json"))
	return &fileStore{path: filepath.Join(configDir, "secrets.json")}
}

type keyringStore struct{}

func (keyringStore) SetSecret(key, value string) error {
	if err := keyring.Set(keyringService, key, value); err != nil {
		return fmt.Errorf("keyring set: %w", err)
	}
	return nil
}

func (keyringStore) GetSecret(key string) (string, error) {
	v, err := keyring.Get(keyringService, key)
	if err != nil {
		return "", fmt.Errorf("keyring get: %w", err)
	}
	return v, nil
}

func (keyringStore) DeleteSecret(key string) error {
	err := keyring.Delete(keyringService, key)
	if err != nil && !errors.Is(err, keyring.ErrNotFound) {
		return fmt.Errorf("keyring delete: %w", err)
	}
	return nil
}

type fileStore struct {
	mu   sync.Mutex
	path string
}

func (f *fileStore) load() (map[string]string, error) {
	data, err := os.ReadFile(f.path)
	if errors.Is(err, os.ErrNotExist) {
		return map[string]string{}, nil
	}
	if err != nil {
		return nil, fmt.Errorf("read secret file: %w", err)
	}
	out := map[string]string{}
	if err := json.Unmarshal(data, &out); err != nil {
		return nil, fmt.Errorf("parse secret file: %w", err)
	}
	return out, nil
}

func (f *fileStore) save(m map[string]string) error {
	data, err := json.Marshal(m)
	if err != nil {
		return fmt.Errorf("encode secret file: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(f.path), 0o700); err != nil {
		return fmt.Errorf("create config dir: %w", err)
	}
	if err := os.WriteFile(f.path, data, 0o600); err != nil {
		return fmt.Errorf("write secret file: %w", err)
	}
	return nil
}

func (f *fileStore) SetSecret(key, value string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	m, err := f.load()
	if err != nil {
		return err
	}
	m[key] = value
	return f.save(m)
}

func (f *fileStore) GetSecret(key string) (string, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	m, err := f.load()
	if err != nil {
		return "", err
	}
	v, ok := m[key]
	if !ok {
		return "", fmt.Errorf("no secret for %q", key)
	}
	return v, nil
}

func (f *fileStore) DeleteSecret(key string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	m, err := f.load()
	if err != nil {
		return err
	}
	delete(m, key)
	return f.save(m)
}
