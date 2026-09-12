// 内核隔离能力探测。引擎启动时自检一次，结果写进 rollout 的 session_meta——
// 将来看日志能知道"这次运行到底有没有真沙箱"。
#pragma once

#include <string>

#include "proto.hpp"

namespace hx {

struct Caps {
  // ★ 这个结构体同时装着两个平台的探测结果，故意不拆成两个类型。
  //   理由是 session_meta 的形状必须稳定：翻三个月前的 rollout 时，
  //   "这次到底有没有真沙箱" 要能用同一套字段问出来，而不是先判断平台
  //   再决定读哪些键。用不上的字段留默认值，CapsToJson 只输出本平台那一半。
  std::string kernel;  // Linux: uname -r；Windows: 版本号
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

  // ---- Windows ----
  //
  // 对位关系（每一条都在 sandbox/confine_win.hpp 与 exec/proc.hpp 里有详述）：
  //   appcontainer  <-  landlock ＋ seccomp 的断网那一半
  //   job_objects   <-  rlimit ＋ cgroup ＋「进程组即所有权」
  //   conpty        <-  forkpty
  bool appcontainer = false;
  std::string appcontainer_reason;
  bool job_objects = false;
  bool nested_jobs = false;  // Win8+：子进程自建 Job 也逃不出我们的 Job
  bool conpty = false;
};

Caps DetectCaps();
json CapsToJson(const Caps& c);

/**
 * 这台机器上到底有没有真沙箱。
 *
 * --self-test 的退出码就靠它（0 = 有，2 = 没有）。做成函数而不是让调用方
 * 自己判断字段，是因为「什么算真沙箱」在两个平台上问的是不同的字段
 * （Linux 看 landlock_abi，Windows 看 appcontainer），而这个结论
 * 本身必须只有一处定义 —— 它同时决定了 session_meta 里那句话的真假。
 */
bool HasRealSandbox(const Caps& c);

// Landlock ABI 与能力的对应（uapi/linux/landlock.h）
inline bool LandlockHasFs(int abi) { return abi >= 1; }
inline bool LandlockHasTruncate(int abi) { return abi >= 3; }
// ABI 4 起支持 TCP bind/connect 限制。注意：Landlock 不管 UDP，
// 断网仍需 seccomp 或 netns —— 见计划「风险与已知限制」。
inline bool LandlockHasNet(int abi) { return abi >= 4; }

}  // namespace hx
