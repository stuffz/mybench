package conn

// Windows child-process plumbing for the tsh/ssh tunnels. Two problems to
// solve: console-mode children allocate a visible console window when
// spawned from a windowsgui binary (every tsh call flashed a cmd window,
// and `tsh proxy db` kept one open for the tunnel's lifetime), and children
// outlive the app if it exits without running shutdown hooks. The first is
// CREATE_NO_WINDOW; the second is a kill-on-close job object, which the OS
// tears down — children included — however this process dies.

import (
	"os/exec"
	"sync"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

// prepareCmd keeps the console-mode child from opening a console window.
func prepareCmd(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{
		HideWindow:    true,
		CreationFlags: windows.CREATE_NO_WINDOW,
	}
}

var tunnelJob = sync.OnceValue(func() windows.Handle {
	job, err := windows.CreateJobObject(nil, nil)
	if err != nil {
		return 0
	}
	info := windows.JOBOBJECT_EXTENDED_LIMIT_INFORMATION{
		BasicLimitInformation: windows.JOBOBJECT_BASIC_LIMIT_INFORMATION{
			LimitFlags: windows.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
		},
	}
	if _, err := windows.SetInformationJobObject(
		job, windows.JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&info)), uint32(unsafe.Sizeof(info)),
	); err != nil {
		_ = windows.CloseHandle(job)
		return 0
	}
	// The handle is intentionally never closed: it must live exactly as long
	// as the process so job teardown coincides with process death.
	return job
})

// adoptCmd ties a started child's lifetime to ours via the job object, so
// tunnels die with the app even on crash or task-manager kill.
func adoptCmd(cmd *exec.Cmd) {
	job := tunnelJob()
	if job == 0 || cmd.Process == nil {
		return
	}
	proc, err := windows.OpenProcess(
		windows.PROCESS_SET_QUOTA|windows.PROCESS_TERMINATE, false, uint32(cmd.Process.Pid))
	if err != nil {
		return
	}
	defer func() { _ = windows.CloseHandle(proc) }()
	_ = windows.AssignProcessToJobObject(job, proc)
}
