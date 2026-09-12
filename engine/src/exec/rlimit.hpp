// 资源上限。
//
// 为什么不是 cgroup v2：
//   cgroup 需要父 cgroup 先启用控制器（subtree_control），而 cgroup v2 的
//   「无内部进程」规则不允许一个已有进程的 cgroup 启用子树控制器。在普通的
//   用户会话里（hxd 与 shell 同处一个 scope），这一步必然 EBUSY。要绕开就得
//   把引擎挪进独立 scope（systemd-run --scope -p Delegate=yes），那是部署方式的
//   选择，不该由引擎自作主张。
//
// rlimit 则零权限、永远可用。诚实地说清它的边界：
//   ★ RLIMIT_NPROC 按「真实 UID」计数，而且数的是**线程（task）不是进程**。
//     实测这台机器 104 个进程对应 667 个线程（tokio/gmain/gdbus 之类的
//     后台线程池占大头）。所以绝不能写死一个绝对值 —— 曾经写死 256，
//     结果沙箱里每一次 fork 都失败，而逃逸测试全绿（它只验了"限制装上了"，
//     没验"正常工作还跑得动"）。
//     正确做法：上限 = 当前用量 + 余量。
//     它仍然只是缓解不是隔离；真正的按沙箱限额要靠 cgroup pids.max。
#pragma once

#include <cstdint>
#include <string>

namespace hx {

struct Limits {
  // 0 表示不设限
  uint64_t max_processes = 0;   // RLIMIT_NPROC（绝对值，应由 LimitsFor 算出）
  uint64_t max_file_bytes = 0;  // RLIMIT_FSIZE：防止写爆磁盘
  uint64_t max_open_files = 0;  // RLIMIT_NOFILE
  bool disable_core_dumps = true;

  /**
   * 整棵子树的内存上限（字节）。0 = 不设限。
   *
   * ★ Linux 侧目前**不生效**：这相当于 cgroup 的 memory.max，而 README
   *   说得很清楚，Cgroup2Session 还没接进 spawn 路径。setrlimit 没有等价物
   *   （RLIMIT_AS 限的是地址空间不是驻留内存，对现代运行时基本没用）。
   *   Windows 侧生效：JOB_OBJECT_LIMIT_JOB_MEMORY 是按 Job 算的真实上限。
   *   字段留在这里而不是藏进 Windows 私有结构，是为了让这处不对称
   *   在类型上就看得见。
   */
  uint64_t max_job_memory_bytes = 0;
};

#ifndef _WIN32
/** 统计该 UID 当前的 task 数（线程，不是进程）。失败返回 0。 */
uint64_t CountUserTasks(unsigned uid);
#endif

/**
 * 算出本会话可用的上限：max_processes = 当前 task 数 + headroom。
 *
 * 数不出当前用量时**不设 NPROC 上限**，而不是退回一个猜的值 ——
 * 猜低了会让整个沙箱不可用，而 fork 炸弹还有超时 + 杀进程组兜底。
 */
Limits LimitsFor(uint64_t headroom = 512);

/** 等价于 LimitsFor()，保留给不关心余量的调用方。 */
Limits DefaultLimits();

#ifndef _WIN32
// 在 fork 之后、execve 之前调用。失败返回 false 并填 err。
bool ApplyLimits(const Limits& l, std::string* err);
#else
/**
 * 把上限装到 Job 对象上，在 AssignProcessToJobObject **之前**调用。
 *
 * ★ Windows 这边为什么根本不会重演 RLIMIT_NPROC 那个坑：
 *   JOB_OBJECT_LIMIT_ACTIVE_PROCESS 数的是**这个 Job 里的进程**，
 *   既不是全系统、也不按 UID、更不是线程。所以可以写死一个绝对值，
 *   它只约束这一个 cell，本机跑着多少别的东西都不影响。
 *   README 里「当前用量 + 512」那套相对算法是 Linux 特有的补丁，
 *   不该也不需要照搬过来。
 */
bool ApplyLimitsToJob(void* job, const Limits& l, std::string* err);
#endif

}  // namespace hx
