// cgroup v2 会话级资源隔离。
//
// 为什么需要它（见 exec/rlimit.hpp 的说明）：
//   rlimit 只是缓解不是隔离，RLIMIT_NPROC 按真实 UID 计数，会波及用户的其它进程。
//   cgroup v2 的 pids.max / memory.max 只作用于该 cgroup，才是真正的按沙箱限额。
//
// 必须处理的约束 —— cgroup v2 的「无内部进程」规则：
//   一个已经有进程的 cgroup 不能再为它的子 cgroup 启用控制器。普通用户会话里
//   hxd 与 shell 同处一个 scope，往该 scope 的 cgroup.subtree_control 写
//   "+pids +memory" 会 EBUSY（已在目标机器实测）。所以本模块必须能**探测**出
//   到底能不能建出真正带 pids.max + memory.max 的子 cgroup，不能时干净地降级，
//   绝不假装限额已生效。
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>

namespace hx {

// 会话级 cgroup 限额。0 表示该项不设限。
struct CgroupLimits {
  uint64_t max_pids = 0;         // pids.max
  uint64_t max_memory_bytes = 0; // memory.max
};

/**
 * 探测 hxd 当前能否建出一个真正带 pids.max + memory.max 的子 cgroup。
 *
 * 这是"能否真限额"的唯一权威答案：它会实际走一遍
 *   启用父控制器 → 建子 cgroup → 写 pids.max/memory.max
 * 的完整流程，任一步失败（典型是父 scope 有内部进程导致 EBUSY）即返回 false。
 * 探测用的临时 cgroup 会在返回前清理掉。
 *
 * 成功返回 true 并把可用子 cgroup 的示例路径写入 *path；
 * 失败返回 false 并把原因写入 *reason（便于 --self-test 诚实报告）。
 */
bool Cgroup2Usable(std::string* path, std::string* reason);

/**
 * RAII 的 per-session cgroup：构造不做事，Create 建 cgroup 并设限额，
 * 析构时把子 cgroup 移除。无裸 new/delete。
 */
class Cgroup2Session {
public:
  Cgroup2Session() = default;
  ~Cgroup2Session();

  Cgroup2Session(const Cgroup2Session&) = delete;
  Cgroup2Session& operator=(const Cgroup2Session&) = delete;

  // 建一个子 cgroup 并写入 limits 里的限额。成功返回 true（此后 Active() 为真）。
  // 失败返回 false 并填 *err，此时本对象等价于未创建。
  bool Create(const CgroupLimits& limits, std::string* err);

  // 把一个 pid 移进本 cgroup（写 cgroup.procs）。失败返回 false 并填 *err。
  bool AddPid(pid_t pid, std::string* err);

  // 限额是否真的建好了。未建好时调用方应视为"没有 cgroup 隔离"。
  bool Active() const { return active_; }
  const std::string& Path() const { return path_; }

private:
  std::string path_;
  bool active_ = false;
};

}  // namespace hx
