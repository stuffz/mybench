package conn

// TunnelMgr spawns `tsh proxy db --tunnel` per Teleport connection (one
// tunnel per open connection — SPEC.md). Readiness is detected by dialing
// the local port rather than parsing tsh's output, which changes between
// versions. The tunnel authenticates with the tsh session's certs; the
// MySQL user/database go to tsh as --db-user/--db-name.

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

type tunnel struct {
	port int
	cmd  *exec.Cmd
}

// TunnelMgr starts tsh database tunnels for Teleport connections.
type TunnelMgr struct{}

func freePort() (int, error) {
	var lc net.ListenConfig
	l, err := lc.Listen(context.Background(), "tcp", "127.0.0.1:0")
	if err != nil {
		return 0, fmt.Errorf("find free port: %w", err)
	}
	port := l.Addr().(*net.TCPAddr).Port //nolint:forcetypeassert // net.Listen("tcp") always returns *net.TCPAddr
	_ = l.Close()
	return port, nil
}

func (m *TunnelMgr) start(teleportDB, dbUser, dbName string) (*tunnel, error) {
	if teleportDB == "" {
		return nil, errors.New("teleport connection has no database resource name")
	}
	if _, err := exec.LookPath("tsh"); err != nil {
		return nil, errors.New("tsh not found on PATH — install the Teleport client and log in first")
	}

	// Preflight: an expired or missing login otherwise surfaces as a
	// cryptic dial timeout twenty seconds later.
	statusCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	statusCmd := exec.CommandContext(statusCtx, "tsh", "status")
	prepareCmd(statusCmd)
	if out, err := statusCmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("tsh is not logged in — run `tsh login` first (%s)", firstLine(out))
	}

	port, err := freePort()
	if err != nil {
		return nil, err
	}

	args := []string{"proxy", "db", "--tunnel", teleportDB, "--port", strconv.Itoa(port)}
	if dbUser != "" {
		args = append(args, "--db-user", dbUser)
	}
	if dbName != "" {
		args = append(args, "--db-name", dbName)
	}
	return spawnTunnel("tsh", args, port, "is `tsh status` logged in?")
}

// startSSH forwards a local port to the MySQL target through the system ssh
// client. BatchMode keeps ssh from prompting — auth must come from keys, an
// agent, or ssh_config; a password prompt would hang a GUI app forever.
// The agent is passed explicitly (see agentEnv): a GUI process inherits the
// desktop session's environment, which usually has no SSH_AUTH_SOCK.
func (m *TunnelMgr) startSSH(c SavedConn) (*tunnel, error) {
	if c.SSHHost == "" {
		return nil, errors.New("ssh connection has no SSH host")
	}
	if _, err := exec.LookPath("ssh"); err != nil {
		return nil, errors.New("ssh not found on PATH")
	}
	port, err := freePort()
	if err != nil {
		return nil, err
	}
	target := c.Host
	if target == "" {
		target = "127.0.0.1"
	}
	args := []string{
		"-o", "BatchMode=yes",
		"-o", "ExitOnForwardFailure=yes",
		"-o", "ConnectTimeout=10",
		"-N",
		"-L", fmt.Sprintf("127.0.0.1:%d:%s:%d", port, target, c.Port),
	}
	if c.SSHPort != 0 && c.SSHPort != 22 {
		args = append(args, "-p", strconv.Itoa(c.SSHPort))
	}
	if c.SSHKeyFile != "" {
		args = append(args, "-i", c.SSHKeyFile)
	}
	dest := c.SSHHost
	if c.SSHUser != "" {
		dest = c.SSHUser + "@" + c.SSHHost
	}
	args = append(args, dest)
	env := agentEnv()
	hint := "ssh runs with BatchMode — key or agent auth must work non-interactively"
	if !hasEnv(env, "SSH_AUTH_SOCK") {
		hint += "; no ssh-agent was found, so only an on-disk key file can work"
	}
	return spawnTunnelEnv("ssh", args, env, port, hint)
}

func hasEnv(env []string, key string) bool {
	for _, kv := range env {
		if strings.HasPrefix(kv, key+"=") && kv != key+"=" {
			return true
		}
	}
	return false
}

func spawnTunnel(bin string, args []string, port int, hint string) (*tunnel, error) {
	return spawnTunnelEnv(bin, args, nil, port, hint)
}

// spawnTunnelEnv starts the tunnel process; a nil env inherits this process's.
func spawnTunnelEnv(bin string, args, env []string, port int, hint string) (*tunnel, error) {
	//nolint:gosec // args come from the user's own saved connection config
	cmd := exec.CommandContext(context.Background(), bin, args...)
	cmd.Env = env
	prepareCmd(cmd)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start %s: %w", bin, err)
	}
	adoptCmd(cmd)
	exited := make(chan error, 1)
	go func() { exited <- cmd.Wait() }()

	t := &tunnel{port: port, cmd: cmd}
	deadline := time.Now().Add(20 * time.Second)
	for time.Now().Before(deadline) {
		select {
		case werr := <-exited:
			return nil, fmt.Errorf("%s exited: %v — %s", bin, werr, firstLine(stderr.Bytes()))
		default:
		}
		dialer := net.Dialer{Timeout: 300 * time.Millisecond}
		conn, err := dialer.DialContext(context.Background(), "tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)))
		if err == nil {
			_ = conn.Close()
			return t, nil
		}
		time.Sleep(300 * time.Millisecond)
	}
	t.stop()
	return nil, fmt.Errorf("%s tunnel did not become ready — %s (%s)", bin, hint, firstLine(stderr.Bytes()))
}

func (t *tunnel) stop() {
	if t.cmd != nil && t.cmd.Process != nil {
		_ = t.cmd.Process.Kill()
		// Wait runs in the start goroutine; killing is enough here.
	}
}

func firstLine(b []byte) string {
	s := strings.TrimSpace(string(b))
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = s[:i]
	}
	if s == "" {
		return "no output"
	}
	return s
}
