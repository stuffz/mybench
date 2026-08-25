package conn

// SSH helpers for the connection dialog: finding a usable ssh-agent, and
// browsing the filesystem for a private key.
//
// Why agent discovery exists at all: ssh only finds an agent through
// SSH_AUTH_SOCK, and a GUI app started from a launcher inherits the desktop
// session's environment — not the shell's. An agent started from a shell rc
// (`ssh-agent -a ~/.ssh/agent/s.*`, `eval $(ssh-agent -s)`) is therefore
// invisible here, and BatchMode ssh fails with "Permission denied" even
// though the key is loaded. So: if SSH_AUTH_SOCK is unset or dead, probe the
// well-known socket locations and hand the live one to the ssh child.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"runtime"
	"slices"
	"sort"
	"strings"
	"time"
)

const (
	agentProbeTimeout = 300 * time.Millisecond
	goosWindows       = "windows"
	authSockVar       = "SSH_AUTH_SOCK"
)

// SSHAgentStatus tells the dialog whether key auth can work at all.
type SSHAgentStatus struct {
	Found  bool   `json:"found"`  // a live agent answered
	Socket string `json:"socket"` // the socket in use (informational)
	Keys   int    `json:"keys"`   // identities the agent holds
	Detail string `json:"detail"` // help text when unusable
}

// SSHAgentStatus probes for a usable agent (read-only: it asks for the
// identity list and counts it, never for a signature).
func (s *Service) SSHAgentStatus() *SSHAgentStatus {
	if runtime.GOOS == goosWindows {
		// Windows ssh.exe talks to \\.\pipe\openssh-ssh-agent on its own;
		// SSH_AUTH_SOCK plays no part, so there is nothing to discover.
		return &SSHAgentStatus{Detail: "Windows OpenSSH finds its own agent — start it with `Start-Service ssh-agent`"}
	}
	sock, keys := findAgent()
	if sock == "" {
		return &SSHAgentStatus{Detail: "no ssh-agent found — start one (`ssh-agent`) and `ssh-add` your key, or point SSH Key File at the key"}
	}
	st := &SSHAgentStatus{Found: true, Socket: sock, Keys: keys}
	if keys == 0 {
		st.Detail = "agent is running but holds no keys — run `ssh-add`"
	}
	return st
}

// agentEnv returns the environment for an ssh child: the parent environment,
// plus a discovered SSH_AUTH_SOCK when the inherited one is missing or dead.
func agentEnv() []string {
	env := os.Environ()
	if runtime.GOOS == goosWindows {
		return env
	}
	if sock := os.Getenv(authSockVar); sock != "" {
		if _, ok := probeAgent(sock); ok {
			return env
		}
	}
	sock, _ := findAgent()
	if sock == "" {
		return env
	}
	out := make([]string, 0, len(env)+1)
	for _, kv := range env {
		if !strings.HasPrefix(kv, authSockVar+"=") {
			out = append(out, kv)
		}
	}
	return append(out, authSockVar+"="+sock)
}

// findAgent returns the first live agent socket and its identity count.
func findAgent() (socket string, keys int) {
	for _, p := range agentCandidates() {
		if n, ok := probeAgent(p); ok {
			return p, n
		}
	}
	return "", 0
}

// agentCandidates lists plausible agent sockets, inherited value first.
func agentCandidates() []string {
	var out []string
	add := func(p string) {
		if p != "" {
			out = append(out, p)
		}
	}
	add(os.Getenv(authSockVar))
	if run := os.Getenv("XDG_RUNTIME_DIR"); run != "" {
		// systemd --user ssh-agent.service, gnome-keyring, gcr.
		add(filepath.Join(run, "ssh-agent.socket"))
		add(filepath.Join(run, "gcr", "ssh"))
		add(filepath.Join(run, "keyring", "ssh"))
	}
	// `ssh-agent -s` puts its socket under a private temp dir; a common rc
	// pattern instead pins it under ~/.ssh so it survives reboots.
	globs := []string{"/tmp/ssh-*/agent.*"}
	if tmp := os.Getenv("TMPDIR"); tmp != "" {
		globs = append(globs, filepath.Join(tmp, "ssh-*", "agent.*"))
	}
	if home, err := os.UserHomeDir(); err == nil {
		globs = append(globs,
			filepath.Join(home, ".ssh", "agent", "*"),
			filepath.Join(home, ".ssh", "agent.sock"),
			filepath.Join(home, ".ssh", "ssh_auth_sock"),
		)
	}
	for _, g := range globs {
		matches, _ := filepath.Glob(g)
		sort.Strings(matches)
		for _, m := range matches {
			add(m)
		}
	}
	return out
}

// probeAgent asks a socket for its identity list — the cheapest request that
// proves a live agent is behind it — and returns how many it holds.
func probeAgent(path string) (keys int, ok bool) {
	fi, err := os.Stat(path) //nolint:gosec // the path is a socket the user's own session created

	if err != nil || fi.Mode()&os.ModeSocket == 0 {
		return 0, false
	}
	c, err := net.DialTimeout("unix", path, agentProbeTimeout) //nolint:gosec,noctx // local unix socket, bounded by the deadline below
	if err != nil {
		return 0, false
	}
	defer func() { _ = c.Close() }()
	_ = c.SetDeadline(time.Now().Add(agentProbeTimeout))
	// SSH_AGENTC_REQUEST_IDENTITIES (11) — a bare one-byte request.
	if _, err := c.Write([]byte{0, 0, 0, 1, 11}); err != nil {
		return 0, false
	}
	var head [9]byte // length(4) + type(1) + identity count(4)
	if _, err := io.ReadFull(c, head[:5]); err != nil {
		return 0, false
	}
	// SSH_AGENT_IDENTITIES_ANSWER (12); anything else is not an agent.
	if head[4] != 12 {
		return 0, false
	}
	if binary.BigEndian.Uint32(head[:4]) < 5 {
		return 0, true // well-formed but truncated; the agent is alive
	}
	if _, err := io.ReadFull(c, head[5:9]); err != nil {
		return 0, true
	}
	return int(binary.BigEndian.Uint32(head[5:9])), true
}

// SSHFile is one entry in the key browser.
type SSHFile struct {
	Name string `json:"name"`
	Path string `json:"path"`
	Dir  bool   `json:"dir"`
}

// SSHBrowse is one directory listing for the key browser.
type SSHBrowse struct {
	Dir    string    `json:"dir"`
	Parent string    `json:"parent"` // empty at the filesystem root
	Files  []SSHFile `json:"files"`
}

// SSHBrowse lists directories and private-key candidates under dir, starting
// at ~/.ssh when dir is empty. Key files are identified by name and by a
// sibling .pub — never by reading them, so no key material is touched.
func (s *Service) SSHBrowse(dir string) (*SSHBrowse, error) {
	home, _ := os.UserHomeDir()
	if dir == "" {
		if home == "" {
			return nil, errors.New("cannot locate the home directory")
		}
		// ~/.ssh is where keys nearly always live; fall back to the home
		// directory when it does not exist, so Browse still opens somewhere.
		dir = filepath.Join(home, ".ssh")
		if fi, err := os.Stat(dir); err != nil || !fi.IsDir() {
			dir = home
		}
	}
	if strings.HasPrefix(dir, "~") && home != "" {
		dir = filepath.Join(home, strings.TrimPrefix(dir, "~"))
	}
	dir = filepath.Clean(dir)
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, fmt.Errorf("list %s: %w", dir, err)
	}
	out := &SSHBrowse{Dir: dir}
	if parent := filepath.Dir(dir); parent != dir {
		out.Parent = parent
	}
	names := make(map[string]bool, len(entries))
	for _, e := range entries {
		names[e.Name()] = true
	}
	var dirs, files []SSHFile
	for _, e := range entries {
		f := SSHFile{Name: e.Name(), Path: filepath.Join(dir, e.Name()), Dir: e.IsDir()}
		switch {
		case e.IsDir():
			dirs = append(dirs, f)
		case looksLikeKey(e.Name(), names):
			files = append(files, f)
		}
	}
	sort.Slice(dirs, func(i, j int) bool { return dirs[i].Name < dirs[j].Name })
	sort.Slice(files, func(i, j int) bool { return files[i].Name < files[j].Name })
	out.Files = append(out.Files, dirs...)
	out.Files = append(out.Files, files...)
	return out, nil
}

// notKeys are the ~/.ssh regulars that are never private keys.
var notKeys = []string{"config", "known_hosts", "known_hosts.old", "authorized_keys", "agent.sock"}

// looksLikeKey guesses private keys from the name plus a sibling .pub.
func looksLikeKey(name string, siblings map[string]bool) bool {
	if slices.Contains(notKeys, name) {
		return false
	}
	ext := strings.ToLower(filepath.Ext(name))
	if ext == ".pub" || ext == ".sock" || ext == ".old" {
		return false
	}
	if siblings[name+".pub"] {
		return true
	}
	return ext == ".pem" || ext == ".key" || ext == ".ppk" || strings.HasPrefix(name, "id_")
}
