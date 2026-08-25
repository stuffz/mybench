package conn

import (
	"os/exec"
	"syscall"
)

// prepareCmd ties the tunnel to this process: if the server dies without
// running shutdown hooks (SIGKILL, crash), the kernel kills tsh/ssh too
// instead of leaving an invisible orphan holding the proxy port.
func prepareCmd(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL}
}

func adoptCmd(*exec.Cmd) {}
