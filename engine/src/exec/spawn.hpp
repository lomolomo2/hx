// Starting a process inside the sandbox.
//
// ★ The two platforms differ on *when* privileges come down, but the invariant
//   is the same: the child is inside the constraints from its very first
//   instruction, with no window at all.
//
//   Linux    fork -> chdir -> landlock_restrict_self -> rlimit -> seccomp -> execve
//            The wrong order means it never happened: Landlock cannot revoke
//            already-open fds, and seccomp applied too early blocks the steps
//            that follow it.
//
//   Windows  the AppContainer token is fixed at
//            CreateProcess(CREATE_SUSPENDED) time
//            -> AssignProcessToJobObject -> ResumeThread
//            Created suspended, joined to the Job, then released -- the child
//            has not executed a single instruction before any of it.
#pragma once

#include <string>
#include <vector>

#include "exec/proc.hpp"
#include "exec/rlimit.hpp"
#include "io/io.hpp"
#include "sandbox/caps.hpp"
#include "sandbox/confine.hpp"
#include "sandbox/policy.hpp"
#include "sandbox/seccomp.hpp"

namespace hx {

struct SpawnCellRequest {
  std::vector<std::string> argv;
  std::string cwd;                // absolute, already through path_guard
  std::vector<std::string> envp;  // the complete environment ("K=V"); the host's environ is not inherited
  const Confinement* conf = nullptr;
  SeccompPlan seccomp;            // Linux only: one more layer beyond Landlock
  Limits limits;                  // process count / file size / fd count
};

struct SpawnCellResult {
  bool ok = false;
  Proc proc;
  io::Fd in_fd = io::kInvalid;   // write end -> the child's stdin
  io::Fd out_fd = io::kInvalid;  // read end <- the child's stdout
  io::Fd err_fd = io::kInvalid;  // read end <- the child's stderr
  std::string error;
};

// Asynchronous start: returns three pipes, driven by the parent's Reactor.
SpawnCellResult SpawnCell(const SpawnCellRequest& req);

struct SpawnResult {
  bool started = false;
  int exit_code = -1;
  int term_signal = 0;
  std::string error;  // set when started == false
};

// Run in the foreground to completion. conf is built by the caller (the parent).
SpawnResult RunForeground(const std::vector<std::string>& argv, const std::string& cwd,
                          const std::vector<std::string>& envp, const Confinement* conf,
                          const SeccompPlan& seccomp, const Limits& limits);

}  // namespace hx
