//go:build !windows && !linux

package conn

import "os/exec"

// No parent-death plumbing here (macOS has no pdeathsig equivalent);
// orphan prevention relies on ServiceShutdown closing tunnels on quit.
func prepareCmd(*exec.Cmd) {}

func adoptCmd(*exec.Cmd) {}
