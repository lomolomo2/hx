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
 * Is this the conhost.exe the system attaches to a console program?
 *
 * ★ Why this check is needed, and why it is not "whitelisting a process name".
 *
 *   A console program started with CREATE_NO_WINDOW gets a conhost.exe from
 *   the system; it lands inside our Job and outlives its client by a moment.
 *   Without excluding it, every ordinary command would be judged to have "left
 *   orphan processes behind", the orphans_killed field would be permanently
 *   true, and it would carry no information at all.
 *
 *   The test is that the **full image path** equals
 *   %SystemRoot%\System32\conhost.exe, not the process name. A process inside
 *   the sandbox cannot write to System32, so it cannot forge that path.
 *
 *   And even if this layer were bypassed, no process leaks: the Job has
 *   KILL_ON_JOB_CLOSE, so when the cell is reaped (~Cell -> ReleaseProc ->
 *   CloseHandle(job)) the kernel cleans out everything still inside. This
 *   check only affects "kill early and report or not", never "does it
 *   eventually get cleaned up".
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
  // ★ The question is whether any process in the Job is still alive, not
  //   whether the direct child is. Keeping those separate is exactly the
  //   antidote to the `bash -c 'cmd &'` trap in the README: after the top
  //   process exits 0, the background process is still in the Job and this
  //   still returns true.
  if (p.group == nullptr) return false;

  // ★ The ActiveProcesses count cannot be used: two traps, both of which
  //   produce false orphans_killed reports.
  //
  //   1. **The direct child is still on the list.** A process's association
  //      with a Job is only released when the process object is destroyed, and
  //      we hold its HANDLE throughout (Reap needs it), so it still counts as
  //      a Job member after exiting. Measured: after `cmd /c echo` exits,
  //      active=2, and one of those is the child itself.
  //   2. **A console program drags a conhost.exe in.** A console process
  //      created with CREATE_NO_WINDOW gets a conhost from the system; it
  //      lands in our Job too and outlives its client by a moment.
  //
  //   So instead: walk the pids on the list, skip the direct child, and for
  //   each of the rest **confirm individually that it is really running** (one
  //   whose handle still exists but has exited makes WaitForSingleObject ready
  //   immediately). The question is "besides the child itself, is anything
  //   alive" -- which is what orphans actually means.
  struct {
    JOBOBJECT_BASIC_PROCESS_ID_LIST list;
    ULONG_PTR more[255];
  } ids{};
  if (::QueryInformationJobObject(p.group, JobObjectBasicProcessIdList, &ids, sizeof(ids),
                                  nullptr) == 0) {
    // When the list does not fit (more than 256) this fails with
    // ERROR_MORE_DATA -- which itself says the descendants are absurdly
    // numerous, so treating that as "still alive" is right.
    return ::GetLastError() == ERROR_MORE_DATA;
  }

  bool alive = false;
  for (DWORD i = 0; i < ids.list.NumberOfProcessIdsInList; ++i) {
    const DWORD pid = static_cast<DWORD>(ids.list.ProcessIdList[i]);
    if (static_cast<int64_t>(pid) == p.pid) continue;  // the direct child; Reap handles it separately
    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) {
      // Failing to open usually means it is already gone; and if one really
      // exists but cannot be opened there is nothing to be done here anyway --
      // TerminateJobObject will still collect it later.
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
    // ★ STILL_ACTIVE is 259. A process is perfectly entitled to exit with 259,
    //   which would be misread here as "still running". So confirm with a
    //   zero-timeout wait: only WAIT_OBJECT_0 means it really exited.
    if (::WaitForSingleObject(p->handle, 0) != WAIT_OBJECT_0) return false;
  }
  *exit_code = static_cast<int>(code);
  // Windows has no notion of "killed by a signal". A process collected by
  // TerminateJobObject gets the exit code we passed in, and the engine side
  // distinguishes the case via killed_by_timeout, so report 0 honestly here
  // rather than inventing a signal number.
  *term_signal = 0;
  return true;
}

bool SignalGroup(const Proc& p, Sig s) {
  if (s == Sig::kKill) {
    if (p.group == nullptr) return false;
    return ::TerminateJobObject(p.group, 137) != 0;  // 128 + SIGKILL, matching the POSIX side
  }
  // ★ An honest boundary: this **almost certainly fails, and that is correct**.
  //
  //   Windows has no SIGTERM/SIGINT/SIGHUP/SIGQUIT. The closest thing is
  //   CTRL_BREAK, but GenerateConsoleCtrlEvent can only reach processes on
  //   **the caller's own console**, and a cell is started with
  //   CREATE_NO_WINDOW + CREATE_NEW_PROCESS_GROUP: it has its own new console
  //   and hxd is not on it. No shared console means it cannot be delivered.
  //
  //   Undeliverable returns false and **never silently escalates to
  //   TerminateJobObject**: substituting a hard kill for a graceful
  //   termination would let the caller believe the process had a chance to
  //   clean up when it did not. A host seeing killed=false knows to use KILL
  //   instead -- far better than faking success.
  //
  //   The pty path is the only one that can succeed: ConPTY gives the child a
  //   real console.
  if (p.pid <= 0) return false;
  return ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(p.pid)) != 0;
}

void FreezeAndKillGroup(const Proc& p) {
  // The Linux side needs "freeze + sweep repeatedly" because SIGKILL races
  // fork. TerminateJobObject is atomic: the kernel suspends the entire Job
  // before collecting it, so there is no race -- one shot is clean and no
  // sweeps are needed.
  if (p.group != nullptr) ::TerminateJobObject(p.group, 137);
}

bool DupGroupForSweep(const Proc& p, Proc* out) {
  // ★ Windows needs no sweeping, so this always returns false.
  //   TerminateJobObject locks the whole Job while it terminates: no process
  //   can fork during that window, so there is no "missed this round, sweep
  //   again next round" to speak of.
  //   The README's lesson about never sending SIGCONT, lest survivors start
  //   breeding again, has no corresponding failure mode here at all.
  (void)p;
  (void)out;
  return false;
}

void ReleaseProc(Proc* p) {
  // Order matters: the process must already be reaped (the caller guarantees
  // it), then close the pseudoconsole, and the Job last.
  if (p->pty != nullptr) {
    ClosePtyHandle(p->pty);
    p->pty = nullptr;
  }
  if (p->handle != nullptr) {
    ::CloseHandle(p->handle);
    p->handle = nullptr;
  }
  if (p->group != nullptr) {
    // ★ The Job has KILL_ON_JOB_CLOSE: when the last handle closes, every
    //   process still alive inside goes with it. So "closing the handle" is
    //   itself the backstop cleanup -- when hxd is killed with -9, the kernel
    //   clears out the subtree on our behalf, which the Linux side cannot do.
    ::CloseHandle(p->group);
    p->group = nullptr;
  }
  p->pid = -1;
}

}  // namespace hx
#endif  // _WIN32
