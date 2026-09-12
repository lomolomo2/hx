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
  // 子进程 setsid 过，所以 pgid == pid。负号打的是整个组。
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
  // 先冻住：停住的进程 fork 不动，SIGKILL 才不用和繁殖速度赛跑。
  ::kill(-pgid, SIGSTOP);
  ::kill(-pgid, SIGKILL);
  // ★ 绝不能在这里 SIGCONT。
  //   SIGKILL 对停住的进程照样生效，所以没必要解冻；而一旦解冻，
  //   这一轮没杀到的幸存者就会重新开始繁殖 —— 实测正是这一行让
  //   fork 炸弹永远清不干净。漏网的留在 stopped 状态，下一轮扫掉。
}

bool DupGroupForSweep(const Proc& p, Proc* out) {
  if (!p.valid()) return false;
  // pgid 只是个整数，没有所有权可言 —— 复制它就够了。
  out->pid = p.pid;
  return true;
}

void ReleaseProc(Proc* p) { p->pid = -1; }

}  // namespace hx
#endif  // !_WIN32
