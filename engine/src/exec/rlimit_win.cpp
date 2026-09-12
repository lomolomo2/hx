#ifdef _WIN32
#include "exec/rlimit.hpp"

#include <windows.h>

#include "platform/win_util.hpp"

namespace hx {

Limits LimitsFor(uint64_t headroom) {
  Limits l;
  // ★ This one line is the biggest difference from the Linux side.
  //
  //   Linux's RLIMIT_NPROC counts by real UID and counts **threads**, so the
  //   limit can only be computed as "current usage + headroom" (README: on
  //   this machine 104 processes = 667 threads, and hardcoding 256 made every
  //   fork inside the sandbox fail).
  //
  //   Windows's JOB_OBJECT_LIMIT_ACTIVE_PROCESS counts **processes in this
  //   Job**. It is unaffected by what else runs on the system, by other
  //   sessions, or by thread counts. So an absolute value is correct: 512
  //   processes is plenty for any build task and still stops a fork bomb.
  //   headroom is meaningless here, but the parameter is kept so both sides
  //   share a signature.
  (void)headroom;
  l.max_processes = 512;
  // 2 GiB per file. Job objects have no such setting; see the honest note in
  // ApplyLimitsToJob.
  l.max_file_bytes = 2ull * 1024 * 1024 * 1024;
  l.max_open_files = 4096;
  l.disable_core_dumps = true;
  // ★ This one the Linux side actually lacks: it is equivalent to a cgroup's
  //   memory.max, and per the README Cgroup2Session is not wired into the
  //   spawn path yet. 4 GiB is enough to compile with, without letting a
  //   runaway process drag the machine down.
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

  // ★ This is how "a cell owns its process group" is implemented on Windows,
  //   and it is stronger than the Linux side: when the last Job handle closes,
  //   the kernel collects every process still alive inside. Which means that
  //   if hxd itself is killed with -9, the subtree goes with it -- something
  //   the Linux side cannot do (orphans get reparented to init and live on).
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
    // The closest thing to "leave no core dump": terminate outright on a
    // crash, with no WER dialog and no crash dump written. A child that
    // crashes inside the sandbox should quietly return a non-zero exit code
    // rather than leaving a window hanging around waiting for someone to click
    // OK -- which would leave the cell never done.
    basic.LimitFlags |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  }

  if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ==
      0) {
    *err = win::LastError("SetInformationJobObject(limits)");
    return false;
  }

  // UI restrictions: block reading the clipboard, changing display settings,
  // and reaching windows on other desktops. Landlock/seccomp have no
  // counterpart (the X11 layer is not in the kernel), so this one is free.
  JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
  ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                           JOB_OBJECT_UILIMIT_WRITECLIPBOARD |
                           JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                           JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_DESKTOP |
                           JOB_OBJECT_UILIMIT_EXITWINDOWS;
  // Failure is not fatal: on some Jobs this cannot be set, and it is not a
  // pillar of the security model.
  ::SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui));

  // ---- The honest gaps ----
  //
  // l.max_file_bytes (RLIMIT_FSIZE) and l.max_open_files (RLIMIT_NOFILE) have
  // **no** kernel-level counterpart on Windows: Job objects limit neither the
  // size of a single file nor the number of handles. We do not pretend they
  // were applied -- caps reports both as false under job_objects.limits, so
  // session_meta shows what this run actually constrained.
  //   - filling the disk: backstopped by max_job_memory + timeouts + human
  //     approval, not by the kernel
  //   - handle exhaustion: affects only processes in this Job, whose blast
  //     radius the Job already bounds
  return true;
}

}  // namespace hx
#endif  // _WIN32
