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
   * 主端。读是子进程输出，写是喂给它的输入；不分 stdout/stderr ——
   * 这一点两个平台一致，因为它是 pty 语义本身决定的。
   *
   * ★ 但「主端是几条流」不一致，这是必须暴露在类型里的差异：
   *     Linux   pty 主端就是**一个** fd，读写同一个 -> 两个字段相等
   *     Windows ConPTY 要求调用方自己给两条管道（一进一出），
   *             主端因此是**两个**独立句柄 -> 两个字段不等
   *   engine.cpp 于是不能再假设 in_fd == out_fd（原来的 pty 分支就是这么写的），
   *   关闭时也要分别处理，否则 Windows 上会漏掉一条管道、cell 永远不 EOF。
   */
  io::Fd master_out = io::kInvalid;  // 读：子进程输出
  io::Fd master_in = io::kInvalid;   // 写：喂给子进程
  std::string error;
};

PtySpawnResult SpawnPty(const PtySpawnRequest& req);

#ifdef _WIN32
/**
 * 关掉 ConPTY 伪控制台。
 *
 * ★ 必须在进程已经回收之后才调用：ClosePseudoConsole 会等附着其上的
 *   进程退出。进程还活着时调它，单线程事件循环就直接挂死在这里。
 *   所以生命周期挂在 ReleaseProc 上，而不是 cell 关流的时候。
 */
void ClosePtyHandle(void* pcon);
#endif

}  // namespace hx
