#ifdef _WIN32
#include "exec/proc.hpp"

#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <string>

#include "exec/pty.hpp"
#include "platform/platform.hpp"

namespace hx {
namespace {

/**
 * 这个进程是不是系统给控制台程序配的 conhost.exe？
 *
 * ★ 为什么需要这个判断，以及为什么它不是在"放过一类进程名"。
 *
 *   带 CREATE_NO_WINDOW 起的控制台程序，系统会配一个 conhost.exe，
 *   它落在我们的 Job 里，并且在客户端退出后还活一小会儿。不排除它，
 *   每一条普通命令都会被判成「留下了孤儿进程」，orphans_killed 这个
 *   字段就永远是 true，也就再没有任何信号量了。
 *
 *   判据是**完整镜像路径**等于 %SystemRoot%\System32\conhost.exe，
 *   不是进程名。沙箱里的进程写不了 System32，所以伪造不出这个路径。
 *
 *   而且就算这一层被绕过也不会漏进程：Job 设了 KILL_ON_JOB_CLOSE，
 *   cell 回收时（~Cell -> ReleaseProc -> CloseHandle(job)）内核会把
 *   里面剩下的一切收干净。这里的判断只影响"要不要提前杀、要不要上报"，
 *   影响不到"最终会不会被清理"。
 */
bool IsConsoleHost(HANDLE process) {
  static const std::wstring expected = [] {
    wchar_t buf[MAX_PATH]{};
    const UINT n = ::GetSystemDirectoryW(buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : L"";
    if (dir.empty()) return std::wstring();
    if (dir.back() != L'\\') dir.push_back(L'\\');
    std::wstring p = dir + L"conhost.exe";
    for (wchar_t& c : p) c = static_cast<wchar_t>(::towlower(c));
    return p;
  }();
  if (expected.empty()) return false;

  wchar_t path[MAX_PATH]{};
  DWORD len = MAX_PATH;
  if (::QueryFullProcessImageNameW(process, 0, path, &len) == 0) return false;
  std::wstring actual(path, len);
  for (wchar_t& c : actual) c = static_cast<wchar_t>(::towlower(c));
  return actual == expected;
}

}  // namespace

bool ParseSignal(const std::string& name, Sig* out) {
  if (name == "TERM") { *out = Sig::kTerm; return true; }
  if (name == "KILL") { *out = Sig::kKill; return true; }
  if (name == "INT") { *out = Sig::kInt; return true; }
  if (name == "HUP") { *out = Sig::kHup; return true; }
  if (name == "QUIT") { *out = Sig::kQuit; return true; }
  return false;
}

bool GroupAlive(const Proc& p) {
  // ★ 问的是 Job 里还有没有活着的进程，不是「直接子进程还在吗」。
  //   这两件事分开，正是 README 里 `bash -c 'cmd &'` 那个坑的解药：
  //   顶层进程 exit 0 之后，后台进程仍然在 Job 里，这里照样返回 true。
  if (p.group == nullptr) return false;

  // ★ 不能用 ActiveProcesses 计数，有两个坑，都会造成 orphans_killed 误报：
  //
  //   ① **直接子进程自己还在名单里。** 进程与 Job 的关联要等进程对象销毁
  //      才解除，而我们一直持有它的 HANDLE（Reap 要用），于是它退出之后
  //      仍然被算作 Job 成员。实测 `cmd /c echo` 退出后 active=2，
  //      其中一个就是它自己。
  //   ② **控制台程序会带一个 conhost.exe 进来。** 带 CREATE_NO_WINDOW 建的
  //      控制台进程由系统配一个 conhost，它同样落在我们的 Job 里，
  //      并在客户端退出后再存活一小会儿。
  //
  //   所以这里改成：逐个看名单里的 pid，跳过直接子进程本身，其余**逐一确认
  //   是不是真的还在跑**（句柄还在但已退出的，WaitForSingleObject 立刻就绪）。
  //   问的是「除了它自己，还有没有活着的」，这才是 orphans 的定义。
  struct {
    JOBOBJECT_BASIC_PROCESS_ID_LIST list;
    ULONG_PTR more[255];
  } ids{};
  if (::QueryInformationJobObject(p.group, JobObjectBasicProcessIdList, &ids, sizeof(ids),
                                  nullptr) == 0) {
    // 名单装不下（超过 256 个）时会失败并返回 ERROR_MORE_DATA ——
    // 那本身就说明子孙多得离谱，按"还活着"处理是对的。
    return ::GetLastError() == ERROR_MORE_DATA;
  }

  bool alive = false;
  for (DWORD i = 0; i < ids.list.NumberOfProcessIdsInList; ++i) {
    const DWORD pid = static_cast<DWORD>(ids.list.ProcessIdList[i]);
    if (static_cast<int64_t>(pid) == p.pid) continue;  // 直接子进程，已经单独 Reap 过
    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
      // 打不开通常意味着已经没了；真打不开又确实存在的话我们也无能为力，
      // 后面 TerminateJobObject 照样能收掉它。
      continue;
    }
    const DWORD w = ::WaitForSingleObject(h, 0);
    const bool running = (w != WAIT_OBJECT_0);
    const bool console_host = running && IsConsoleHost(h);
    ::CloseHandle(h);
    if (running && !console_host) {
      alive = true;
      break;
    }
  }

  if (std::string dbg; platform::GetEnv("HX_DEBUG_SPAWN", &dbg)) {
    std::string pids;
    for (DWORD i = 0; i < ids.list.NumberOfProcessIdsInList; ++i) {
      pids += " " + std::to_string(static_cast<unsigned long long>(ids.list.ProcessIdList[i]));
    }
    std::fprintf(stderr, "hxd/debug: job of pid=%lld listed=%lu alive_others=%d pids:%s\n",
                 static_cast<long long>(p.pid),
                 static_cast<unsigned long>(ids.list.NumberOfProcessIdsInList), alive ? 1 : 0,
                 pids.c_str());
  }
  return alive;
}

bool Reap(Proc* p, int* exit_code, int* term_signal) {
  if (p->handle == nullptr) return false;
  DWORD code = 0;
  if (::GetExitCodeProcess(p->handle, &code) == 0) return false;
  if (code == STILL_ACTIVE) {
    // ★ STILL_ACTIVE 是 259。一个进程完全可以就以 259 退出，
    //   那样这里会把它误判成「还在跑」。所以再用 0 超时 wait 确认一次：
    //   WAIT_OBJECT_0 才是真的退了。
    if (::WaitForSingleObject(p->handle, 0) != WAIT_OBJECT_0) return false;
  }
  *exit_code = static_cast<int>(code);
  // Windows 没有「被信号杀死」这回事。被 TerminateJobObject 收掉的进程
  // 拿到的是我们传进去的退出码，engine 侧靠 killed_by_timeout 区分，
  // 这里如实报 0 而不是编一个信号号出来。
  *term_signal = 0;
  return true;
}

bool SignalGroup(const Proc& p, Sig s) {
  if (s == Sig::kKill) {
    if (p.group == nullptr) return false;
    return ::TerminateJobObject(p.group, 137) != 0;  // 128 + SIGKILL，与 POSIX 侧对齐
  }
  // ★ 诚实的边界：这里**几乎一定会失败，而且这是对的**。
  //
  //   Windows 没有 SIGTERM/SIGINT/SIGHUP/SIGQUIT。最接近的是 CTRL_BREAK，
  //   但 GenerateConsoleCtrlEvent 只能发给**与调用方同一个控制台**上的进程，
  //   而 cell 是带 CREATE_NO_WINDOW + CREATE_NEW_PROCESS_GROUP 起的：
  //   它有自己的新控制台，hxd 根本不在上面。没有共享控制台 = 送不到。
  //
  //   送不到就返回 false，**绝不悄悄升级成 TerminateJobObject**：
  //   把「优雅终止」偷换成「强杀」会让调用方以为进程有机会清理，而它没有。
  //   宿主看到 killed=false，就知道该改用 KILL —— 这比假装成功强得多。
  //
  //   pty 路径是唯一可能成功的：ConPTY 给了子进程一个真控制台。
  if (p.pid <= 0) return false;
  return ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(p.pid)) != 0;
}

void FreezeAndKillGroup(const Proc& p) {
  // Linux 侧要「冻结 + 反复清扫」，是因为 SIGKILL 和 fork 在赛跑。
  // TerminateJobObject 是原子的：内核先挂起整个 Job 再收，没有赛跑，
  // 所以这里一次就干净，不需要 sweeps。
  if (p.group != nullptr) ::TerminateJobObject(p.group, 137);
}

bool DupGroupForSweep(const Proc& p, Proc* out) {
  // ★ Windows 不需要清扫，所以这里永远返回 false。
  //   TerminateJobObject 在终止期间锁住整个 Job：没有进程能在这期间 fork，
  //   也就没有「这一轮漏网、下一轮再扫」这回事。
  //   README 里那条「绝不 SIGCONT，否则幸存者重新繁殖」的教训，
  //   在这里根本不存在对应的失败模式。
  (void)p;
  (void)out;
  return false;
}

void ReleaseProc(Proc* p) {
  // 顺序要紧：先确认进程已回收（调用方保证），再关伪控制台，最后关 Job。
  if (p->pty != nullptr) {
    ClosePtyHandle(p->pty);
    p->pty = nullptr;
  }
  if (p->handle != nullptr) {
    ::CloseHandle(p->handle);
    p->handle = nullptr;
  }
  if (p->group != nullptr) {
    // ★ Job 设了 KILL_ON_JOB_CLOSE：最后一个句柄关掉时，里面还活着的进程
    //   一并被收。所以「关句柄」本身就是兜底清理 —— hxd 被 kill -9 时，
    //   内核替我们把子树收干净，这一条 Linux 侧做不到。
    ::CloseHandle(p->group);
    p->group = nullptr;
  }
  p->pid = -1;
}

}  // namespace hx
#endif  // _WIN32
