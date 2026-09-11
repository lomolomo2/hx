// 在沙箱里起进程。
// M1 先做前台同步版（--sandbox-exec 用），M4 扩成 cell + pty 的异步版。
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

#include "sandbox/caps.hpp"
#include "sandbox/landlock.hpp"
#include "sandbox/policy.hpp"
#include "exec/rlimit.hpp"
#include "sandbox/seccomp.hpp"

namespace hx {

struct SpawnCellRequest {
  std::vector<std::string> argv;
  std::string cwd;                // 绝对路径，已过 path_guard
  std::vector<std::string> envp;  // 完整环境（"K=V"），不继承宿主 environ
  int ruleset_fd = -1;
  SeccompPlan seccomp;            // Landlock 之外再封一层
  Limits limits;                  // 进程数 / 文件大小 / fd 数
};

struct SpawnCellResult {
  bool ok = false;
  pid_t pid = -1;
  int in_fd = -1;   // 写端 → 子进程 stdin
  int out_fd = -1;  // 读端 ← 子进程 stdout
  int err_fd = -1;  // 读端 ← 子进程 stderr
  std::string error;
};

// 异步启动：返回三条管道，父进程用 epoll 驱动。
SpawnCellResult SpawnCell(const SpawnCellRequest& req);

struct SpawnResult {
  bool started = false;
  int exit_code = -1;
  int term_signal = 0;
  std::string error;  // started == false 时有值
};

// 前台运行到结束。ruleset_fd 由调用方构建（父进程），本函数在子进程里施加。
SpawnResult RunForeground(const std::vector<std::string>& argv, const std::string& cwd,
                          const std::vector<std::string>& envp, int ruleset_fd,
                          const SeccompPlan& seccomp, const Limits& limits);

}  // namespace hx
