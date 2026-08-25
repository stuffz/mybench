package conn

import (
	"context"
	"encoding/binary"
	"io"
	"net"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

// fakeAgent serves identity-list answers on a unix socket, like ssh-agent.
func fakeAgent(t *testing.T, keys uint32) string {
	t.Helper()
	// Socket paths are length-limited; keep the temp dir out of it.
	dir, err := os.MkdirTemp("", "ag")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(dir) })
	path := filepath.Join(dir, "s")
	var lc net.ListenConfig
	l, err := lc.Listen(context.Background(), "unix", path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = l.Close() })
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			var head [5]byte
			if _, err := io.ReadFull(c, head[:]); err != nil || head[4] != 11 {
				_ = c.Close()
				continue
			}
			resp := make([]byte, 9)
			binary.BigEndian.PutUint32(resp[:4], 5)
			resp[4] = 12
			binary.BigEndian.PutUint32(resp[5:], keys)
			_, _ = c.Write(resp)
			_ = c.Close()
		}
	}()
	return path
}

func TestProbeAgent(t *testing.T) {
	t.Parallel()
	sock := fakeAgent(t, 2)
	if n, ok := probeAgent(sock); !ok || n != 2 {
		t.Fatalf("probeAgent(live) = %d, %v; want 2, true", n, ok)
	}
	// A plain file is not an agent, however suggestively named.
	plain := filepath.Join(t.TempDir(), "ssh-agent.socket")
	if err := os.WriteFile(plain, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, ok := probeAgent(plain); ok {
		t.Fatal("probeAgent(regular file) = true; want false")
	}
	if _, ok := probeAgent(filepath.Join(t.TempDir(), "missing")); ok {
		t.Fatal("probeAgent(missing) = true; want false")
	}
}

func TestAgentEnvKeepsLiveInheritedSocket(t *testing.T) {
	t.Setenv("SSH_AUTH_SOCK", fakeAgent(t, 1))
	want := "SSH_AUTH_SOCK=" + os.Getenv("SSH_AUTH_SOCK")
	if !slices.Contains(agentEnv(), want) {
		t.Fatalf("agentEnv() dropped the live inherited socket")
	}
}

func TestAgentEnvReplacesDeadSocket(t *testing.T) {
	// A GUI process typically has no SSH_AUTH_SOCK at all; a stale one from a
	// dead agent is just as unusable. Either way discovery must win.
	dead := filepath.Join(t.TempDir(), "dead.sock")
	t.Setenv("SSH_AUTH_SOCK", dead)
	live := fakeAgent(t, 1)
	t.Setenv("XDG_RUNTIME_DIR", filepath.Dir(live))
	if err := os.Symlink(live, filepath.Join(filepath.Dir(live), "ssh-agent.socket")); err != nil {
		t.Fatal(err)
	}
	env := agentEnv()
	if slices.Contains(env, "SSH_AUTH_SOCK="+dead) {
		t.Fatal("agentEnv() kept the dead socket")
	}
	if !hasEnv(env, "SSH_AUTH_SOCK") {
		t.Fatal("agentEnv() found no agent")
	}
}

func TestSSHBrowseListsKeysNotPublicHalves(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	fixtures := append([]string{"id_ed25519", "id_ed25519.pub", "work.pem", "notes.txt"}, notKeys...)
	for _, n := range fixtures {
		if err := os.WriteFile(filepath.Join(dir, n), nil, 0o600); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.Mkdir(filepath.Join(dir, "sub"), 0o700); err != nil {
		t.Fatal(err)
	}
	var s Service
	got, err := s.SSHBrowse(dir)
	if err != nil {
		t.Fatal(err)
	}
	names := make([]string, 0, len(got.Files))
	for _, f := range got.Files {
		names = append(names, f.Name)
	}
	// Directories first (so the browser can descend), then key candidates.
	want := []string{"sub", "id_ed25519", "work.pem"}
	if !slices.Equal(names, want) {
		t.Fatalf("SSHBrowse names = %v; want %v", names, want)
	}
	if got.Parent != filepath.Dir(dir) {
		t.Fatalf("Parent = %q; want %q", got.Parent, filepath.Dir(dir))
	}
}
