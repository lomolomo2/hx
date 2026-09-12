#ifdef _WIN32
#include "exec/rlimit.hpp"

#include <windows.h>

#include "platform/win_util.hpp"

namespace hx {

Limits LimitsFor(uint64_t headroom) {
  Limits l;
  // ★ 与 Linux 侧最大的区别就在这一行。
  //
  //   Linux 的 RLIMIT_NPROC 按真实 UID 计数、而且数的是**线程**，
  //   所以上限只能算成「当前用量 + 余量」（README：本机 104 进程 = 667 线程，
  //   写死 256 让沙箱里每次 fork 都失败）。
  //
  //   Windows 的 JOB_OBJECT_LIMIT_ACTIVE_PROCESS 数的是**这个 Job 里的进程**。
  //   跟系统里跑着什么、跟别的会话、跟线程数都无关。于是绝对值是对的：
  //   512 个进程足够任何构建任务，又能挡住 fork 炸弹。
  //   headroom 在这里没有意义，但保留参数是为了两边同签名。
  (void)headroom;
  l.max_processes = 512;
  // 单文件 2 GiB。Job 对象没有这一项，见 ApplyLimitsToJob 的诚实说明。
  l.max_file_bytes = 2ull * 1024 * 1024 * 1024;
  l.max_open_files = 4096;
  l.disable_core_dumps = true;
  // ★ 这一项 Linux 侧反而没有：它等价于 cgroup 的 memory.max，
  //   而 README 说 Cgroup2Session 还没接进 spawn 路径。
  //   4 GiB 够编译，又不至于让一个失控的进程把机器拖垮。
  l.max_job_memory_bytes = 4ull * 1024 * 1024 * 1024;
  return l;
}

Limits DefaultLimits() { return LimitsFor(); }

bool ApplyLimitsToJob(void* job, const Limits& l, std::string* err) {
  if (job == nullptr) {
    *err = "ApplyLimitsToJob: null job";
    return false;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  auto& basic = info.BasicLimitInformation;

  // ★ 这一条是「cell 拥有它的进程组」在 Windows 上的实现，
  //   而且比 Linux 侧强：最后一个 Job 句柄关闭时，内核把里面还活着的进程
  //   全部收掉。也就是说 hxd 自己被 kill -9，子树跟着走 ——
  //   Linux 侧做不到这一点（孤儿会被 init 收养，继续活着）。
  basic.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  if (l.max_processes > 0) {
    basic.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    basic.ActiveProcessLimit = static_cast<DWORD>(l.max_processes);
  }
  if (l.max_job_memory_bytes > 0) {
    basic.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
    info.JobMemoryLimit = static_cast<SIZE_T>(l.max_job_memory_bytes);
  }
  if (l.disable_core_dumps) {
    // 最接近「别留 core dump」的一条：崩溃时直接结束，不弹 WER 对话框、
    // 不写 crash dump。沙箱里的子进程崩了应该安静地返回非零退出码，
    // 而不是挂起一个等人点「确定」的窗口 —— 那会让 cell 永远不 done。
    basic.LimitFlags |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  }

  if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ==
      0) {
    *err = win::LastError("SetInformationJobObject(limits)");
    return false;
  }

  // UI 限制：禁掉读剪贴板、改显示设置、访问其它桌面上的窗口。
  // Landlock/seccomp 没有对应物（X11 那层不在内核里），这里是白捡的一道。
  JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
  ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                           JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
                           JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                           JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_DESKTOP |
                           JOB_OBJECT_UILIMIT_EXITWINDOWS;
  // 失败不致命：某些 Job 上这一项不可设，而它不是安全模型的支柱。
  ::SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui));

  // ---- 诚实的缺口 ----
  //
  // l.max_file_bytes（RLIMIT_FSIZE）与 l.max_open_files（RLIMIT_NOFILE）
  // 在 Windows 上**没有**内核级对位：Job 对象不限单文件大小，也不限句柄数。
  // 这里不假装设上了 —— caps 的 job_objects.limits 会把这两项报成 false，
  // session_meta 里因此能看出「这次运行到底限住了什么」。
  //   · 写爆磁盘：靠 max_job_memory + 超时 + 人工审批兜底，不是内核强制
  //   · 句柄耗尽：只影响这个 Job 里的进程，影响面本来就被 Job 圈住了
  return true;
}

}  // namespace hx
#endif  // _WIN32
