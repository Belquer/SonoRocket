//go:build !windows

package main

import (
	"os"
	"os/exec"
	"syscall"
)

func hideWindow(*exec.Cmd) {}

func parentAlive(pid int) bool {
	if os.Getppid() == 1 && pid != 1 { // reparented: our parent is gone
		return false
	}
	return syscall.Kill(pid, 0) == nil
}

func stopChild(cmd *exec.Cmd) { _ = cmd.Process.Signal(syscall.SIGTERM) }
