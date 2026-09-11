// PTY 会话。
//
// 为什么需要它：管道版的 exec 只能「跑一条命令、拿全部输出」。真实工作里有一整
// 类事情做不了 ——
//   · REPL（python3 -i、node、psql）
//   · 会反问的命令（git rebase -i、sudo、npm init）
//   · 需要边看边决定要不要停的长任务
// 实测 codex 的 write_stdin 被调用 1463 次、wait 224 次，约占其 shell 交互的四分之一。
//
// 与管道版的差别只在「怎么连」：沙箱、rlimit、seccomp 的施加顺序完全一致，
// 所以这里只负责把 pty 主从两端接好，安全策略仍然走 spawn.cpp 那条路。
#pragma once

#include <string>
#include <vector>

#include "exec/rlimit.hpp"
#include "sandbox/seccomp.hpp"

namespace hx {

struct PtySpawnRequest {
  std::vector<std::string> argv;
  std::string cwd;
  std::vector<std::string> envp;
  int ruleset_fd = -1;
  SeccompPlan seccomp;
  Limits limits;
  unsigned short rows = 24;
  unsigned short cols = 120;
};

struct PtySpawnResult {
  bool ok = false;
  pid_t pid = -1;
  /** 主端：读是子进程输出，写是喂给它的输入。PTY 只有一个 fd，不分 stdout/stderr。 */
  int master_fd = -1;
  std::string error;
};

PtySpawnResult SpawnPty(const PtySpawnRequest& req);

}  // namespace hx
