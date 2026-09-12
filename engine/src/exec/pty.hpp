// PTY sessions.
//
// Why they are needed: the pipe flavour of exec can only "run one command and
// collect all the output". A whole class of real work is impossible that way --
//   - REPLs (python3 -i, node, psql)
//   - commands that ask questions back (git rebase -i, sudo, npm init)
//   - long tasks you watch and decide whether to stop
// Measured against codex: write_stdin called 1463 times and wait 224 times,
// roughly a quarter of its shell interaction.
//
// The only difference from the pipe flavour is *how things are connected*: the
// order in which sandbox, rlimit and seccomp are applied is identical, so this
// file only wires up the pty master and slave ends, and the security policy
// still goes through spawn.cpp.
#pragma once

#include <string>
#include <vector>

#include "exec/proc.hpp"
#include "exec/rlimit.hpp"
#include "io/io.hpp"
#include "sandbox/confine.hpp"
#include "sandbox/seccomp.hpp"

namespace hx {

struct PtySpawnRequest {
  std::vector<std::string> argv;
  std::string cwd;
  std::vector<std::string> envp;
  const Confinement* conf = nullptr;
  SeccompPlan seccomp;
  Limits limits;
  unsigned short rows = 24;
  unsigned short cols = 120;
};

struct PtySpawnResult {
  bool ok = false;
  Proc proc;
  /**
   * The master end. Reading gives the child's output, writing feeds it input;
   * stdout and stderr are not separated -- that part is the same on both
   * platforms, because pty semantics themselves dictate it.
   *
   * ★ But *how many streams the master end is* differs, and that difference
   *   has to be visible in the types:
   *     Linux   the pty master is **one** fd, read and written alike
   *             -> the two fields are equal
   *     Windows ConPTY requires the caller to supply two pipes (one in, one
   *             out), so the master end is **two** independent handles
   *             -> the two fields differ
   *   engine.cpp therefore can no longer assume in_fd == out_fd (which is how
   *   the original pty branch was written), and has to close them separately
   *   -- otherwise one pipe is missed on Windows and the cell never reaches
   *   EOF.
   */
  io::Fd master_out = io::kInvalid;  // read: the child's output
  io::Fd master_in = io::kInvalid;   // write: input fed to the child
  std::string error;
};

PtySpawnResult SpawnPty(const PtySpawnRequest& req);

#ifdef _WIN32
/**
 * Close the ConPTY pseudoconsole.
 *
 * ★ Must only be called once the process has already been reaped:
 *   ClosePseudoConsole waits for the processes attached to it to exit. Call it
 *   while the process is still alive and the single-threaded event loop hangs
 *   right here. That is why its lifetime hangs off ReleaseProc rather than the
 *   point where the cell closes its streams.
 */
void ClosePtyHandle(void* pcon);
#endif

}  // namespace hx
