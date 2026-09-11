// 内核隔离能力探测。引擎启动时自检一次，结果写进 rollout 的 session_meta——
// 将来看日志能知道"这次运行到底有没有真沙箱"。
#pragma once

#include <string>

#include "proto.hpp"

namespace hx {

struct Caps {
  std::string kernel;
  int landlock_abi = 0;  // <= 0 表示不可用
  bool seccomp = false;
  bool userns = false;
  bool cgroup2 = false;
  std::string cgroup_path;
  // cgroup2 是否**真能**建出带 pids.max + memory.max 的子 cgroup。
  // available 只说文件系统在；usable 才说限额装得上（无内部进程规则可能挡住）。
  bool cgroup2_usable = false;
  // 不可用时的原因。引擎不假装限额生效，也不隐瞒为什么没生效。
  std::string cgroup2_reason;
};

Caps DetectCaps();
json CapsToJson(const Caps& c);

// Landlock ABI 与能力的对应（uapi/linux/landlock.h）
inline bool LandlockHasFs(int abi) { return abi >= 1; }
inline bool LandlockHasTruncate(int abi) { return abi >= 3; }
// ABI 4 起支持 TCP bind/connect 限制。注意：Landlock 不管 UDP，
// 断网仍需 seccomp 或 netns —— 见计划「风险与已知限制」。
inline bool LandlockHasNet(int abi) { return abi >= 4; }

}  // namespace hx
