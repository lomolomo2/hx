// Linux 的 Confinement 定义：一个 Landlock ruleset fd。
//
// ★ 顺序契约（写错等于没做沙箱）：
//     父进程 BuildConfinement()  →  fork()  →  子进程 ApplyRestrictSelf()  →  execve()
//   Landlock 不能撤销已经打开的 fd，所以必须在 execve 之前、且在子进程里施加。
#pragma once
#ifndef _WIN32

#include <unistd.h>

#include <string>

#include "sandbox/confine.hpp"

namespace hx {

struct Confinement {
  int fd = -1;
  ~Confinement() {
    if (fd >= 0) ::close(fd);
  }
};

// 在 fork 之后的子进程里调用；失败返回 false 并填 err。
// 内部会先 prctl(PR_SET_NO_NEW_PRIVS)，这是 landlock_restrict_self 的前置要求。
bool ApplyRestrictSelf(const Confinement* conf, std::string* err);

uint64_t FsAccessMaskForAbi(int abi);

}  // namespace hx

#endif  // !_WIN32
