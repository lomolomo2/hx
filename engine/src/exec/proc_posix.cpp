#ifndef _WIN32
#include "exec/proc.hpp"

#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>

namespace hx {
namespace {

int ToSignal(Sig s) {
  switch (s) {
    case Sig::kTerm: return SIGTERM;
    case Sig::kKill: return SIGKILL;
    case Sig::kInt: return SIGINT;
    case Sig::kHup: return SIGHUP;
    case Sig::kQuit: return SIGQUIT;
  }
  return 0;
}

}  // namespace

bool ParseSignal(const std::string& name, Sig* out) {
  if (name == "TERM") { *out = Sig::kTerm; return true; }
  if (name == "KILL") { *out = Sig::kKill; return true; }
  if (name == "INT") { *out = Sig::kInt; return true; }
  if (name == "HUP") { *out = Sig::kHup; return true; }
  if (name == "QUIT") { *out = Sig::kQuit; return true; }
  return false;
}

bool GroupAlive(const Proc& p) {
  if (!p.valid()) return false;
  // The child called setsid, so pgid == pid. The negative sign targets the
  // whole group.
  return ::kill(-static_cast<pid_t>(p.pid), 0) == 0;
}

bool Reap(Proc* p, int* exit_code, int* term_signal) {
  if (!p->valid()) return false;
  int status = 0;
  const pid_t got = ::waitpid(static_cast<pid_t>(p->pid), &status, WNOHANG);
  if (got != static_cast<pid_t>(p->pid)) return false;
  if (WIFEXITED(status)) {
    *exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    *term_signal = WTERMSIG(status);
    *exit_code = 128 + *term_signal;
  }
  return true;
}

bool SignalGroup(const Proc& p, Sig s) {
  if (!p.valid()) return false;
  return ::kill(-static_cast<pid_t>(p.pid), ToSignal(s)) == 0;
}

void FreezeAndKillGroup(const Proc& p) {
  if (!p.valid()) return;
  const pid_t pgid = static_cast<pid_t>(p.pid);
  // Freeze first: a stopped process cannot fork, so SIGKILL is not racing its
  // breeding rate.
  ::kill(-pgid, SIGSTOP);
  ::kill(-pgid, SIGKILL);
  // ★ Never SIGCONT here.
  //   SIGKILL still takes effect on a stopped process, so thawing is
  //   unnecessary; and the moment you thaw, survivors this round missed start
  //   breeding again -- measured, that one line is what made a fork bomb
  //   impossible to clean up. Anything missed stays stopped and is swept next
  //   round.
}

bool DupGroupForSweep(const Proc& p, Proc* out) {
  if (!p.valid()) return false;
  // A pgid is just an integer with no ownership attached -- copying it is
  // enough.
  out->pid = p.pid;
  return true;
}

void ReleaseProc(Proc* p) { p->pid = -1; }

}  // namespace hx
#endif  // !_WIN32
