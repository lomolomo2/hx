// Landlock ruleset 构建与施加。
//
// ★ 顺序契约（写错等于没做沙箱）：
//     父进程 BuildRulesetFd()  →  fork()  →  子进程 ApplyRestrictSelf()  →  execve()
//   Landlock 不能撤销已经打开的 fd，所以必须在 execve 之前、且在子进程里施加。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sandbox/caps.hpp"
#include "sandbox/policy.hpp"

namespace hx {

struct RulesetBuild {
  int fd = -1;                        // -1 表示不施加（danger-full-access 或内核不支持）
  bool enforced = false;              // 是否真的会有内核强制
  bool net_enforced = false;          // 网络限制是否真的生效
  std::vector<std::string> warnings;  // 降级说明，必须如实上报给 host
};

// 在父进程构建。返回的 fd 由调用方负责 close，可被多次 fork 复用。
RulesetBuild BuildRulesetFd(const Policy& p, const Caps& caps);

// 在 fork 之后的子进程里调用；失败返回 false 并填 err。
// 内部会先 prctl(PR_SET_NO_NEW_PRIVS)，这是 landlock_restrict_self 的前置要求。
bool ApplyRestrictSelf(int ruleset_fd, std::string* err);

uint64_t FsAccessMaskForAbi(int abi);

}  // namespace hx
