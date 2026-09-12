// 进程与「它拥有的那棵子树」。
//
// ★ 这是 cell 生命周期里最难的一块，两个平台的答案差得很远，所以必须有 seam。
//
//   Linux：子进程 setsid() 自成进程组，回收靠 kill(-pgid) 的
//          「冻结 + 反复清扫」—— 因为 SIGKILL 和 fork 速度在赛跑，
//          而且组空了没有任何通知，只能 kill(-pgid, 0) 探。
//
//   Windows：Job 对象。子树天然属于 Job（Win8 起支持嵌套，子进程自建 Job
//          也逃不出去），TerminateJobObject 是**原子**的 —— 没有赛跑，
//          不需要冻结，不需要清扫。JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
//          还额外保证 hxd 自己被杀时整棵树跟着走，这一条 Linux 侧做不到。
//
//   README「已知的坑」里那条 `bash -c 'cmd &'` 漏出后台进程的教训，
//   在 Windows 上不会重演：直接子进程退出与 Job 是否为空是两件独立的事，
//   引擎问的一直是后者。
#pragma once

#include <cstdint>
#include <string>

namespace hx {

/**
 * 一个 cell 背后的真实进程。
 *
 * ★ 协议里从不出现 pid（HANDLE 模型，见 cell.hpp）。这里留着它只为了
 *   日志和诊断 —— 一旦它漏进 hxp/0，把实现换成容器或远程机器就得改协议。
 */
struct Proc {
  int64_t pid = -1;
  void* handle = nullptr;  // Windows: 进程 HANDLE。Linux: 不用
  void* group = nullptr;   // Windows: Job HANDLE。Linux: 不用（pgid 就是 pid）
  void* pty = nullptr;     // Windows: ConPTY 的 HPCON。Linux: 不用（pty 就是个 fd）

  bool valid() const { return pid > 0; }
};

/** 协议里允许的信号名。Windows 上只有 kKill 是精确的，其余见 proc_win.cpp。 */
enum class Sig { kTerm, kKill, kInt, kHup, kQuit };

bool ParseSignal(const std::string& name, Sig* out);

/** 这棵子树里还有活着的进程吗？（不是「直接子进程还活着吗」——那是两件事） */
bool GroupAlive(const Proc& p);

/** 非阻塞回收直接子进程。已退出返回 true 并填出口码 / 终止信号。 */
bool Reap(Proc* p, int* exit_code, int* term_signal);

/** 给整棵子树发信号。送不到返回 false —— 绝不悄悄升级成强杀。 */
bool SignalGroup(const Proc& p, Sig s);

/**
 * 强制收掉整棵子树。
 *
 * Linux：SIGSTOP 冻住再 SIGKILL（停住的进程 fork 不动，这样才不用和繁殖速度
 *        赛跑），且**绝不 SIGCONT** —— 解冻会让这轮漏网的重新开始繁殖。
 * Windows：TerminateJobObject，一次到位。
 */
void FreezeAndKillGroup(const Proc& p);

/**
 * 为「反复清扫」复制一份组句柄，交给调用方持有。
 *
 * ★ 返回 false 不是失败，而是「这个平台不需要清扫」：
 *     Linux   true  —— SIGKILL 和 fork 在赛跑，一轮扫不干净，
 *                       而且 cell 早就被 exec.wait 回收了，清理必须能活得更久。
 *     Windows false —— TerminateJobObject 期间内核会锁住整个 Job，
 *                       没有幸存者能再 fork，一次就到位，没有可扫的东西。
 */
bool DupGroupForSweep(const Proc& p, Proc* out);

/** 释放持有的内核对象。Linux 上基本是空操作。 */
void ReleaseProc(Proc* p);

}  // namespace hx
