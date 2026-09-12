// 在沙箱里起进程。
//
// ★ 两个平台在「什么时候把权限降下去」上是不同的，但不变式一样：
//   子进程从第一条指令开始就已经在约束里，没有任何窗口期。
//
//   Linux    fork -> chdir -> landlock_restrict_self -> rlimit -> seccomp -> execve
//            顺序错了等于没做：Landlock 撤不掉已打开的 fd，
//            seccomp 装早了会挡掉它后面的步骤。
//
//   Windows  CreateProcess(CREATE_SUSPENDED) 时就把 AppContainer token 定死
//            -> AssignProcessToJobObject -> ResumeThread
//            挂起着建、先进 Job 再放行，子进程连一条指令都还没跑过。
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
  std::string cwd;                // 绝对路径，已过 path_guard
  std::vector<std::string> envp;  // 完整环境（"K=V"），不继承宿主 environ
  const Confinement* conf = nullptr;
  SeccompPlan seccomp;            // 仅 Linux：Landlock 之外再封一层
  Limits limits;                  // 进程数 / 文件大小 / fd 数
};

struct SpawnCellResult {
  bool ok = false;
  Proc proc;
  io::Fd in_fd = io::kInvalid;   // 写端 -> 子进程 stdin
  io::Fd out_fd = io::kInvalid;  // 读端 <- 子进程 stdout
  io::Fd err_fd = io::kInvalid;  // 读端 <- 子进程 stderr
  std::string error;
};

// 异步启动：返回三条管道，父进程用 Reactor 驱动。
SpawnCellResult SpawnCell(const SpawnCellRequest& req);

struct SpawnResult {
  bool started = false;
  int exit_code = -1;
  int term_signal = 0;
  std::string error;  // started == false 时有值
};

// 前台运行到结束。conf 由调用方构建（父进程）。
SpawnResult RunForeground(const std::vector<std::string>& argv, const std::string& cwd,
                          const std::vector<std::string>& envp, const Confinement* conf,
                          const SeccompPlan& seccomp, const Limits& limits);

}  // namespace hx
