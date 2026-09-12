#ifdef _WIN32
#include "sandbox/caps.hpp"

#include <windows.h>

#include <userenv.h>

#include <cstdio>

#include "platform/win_util.hpp"

namespace hx {
namespace {

std::string ProbeVersion() {
  // GetVersionEx lies to programs without a manifest (it always reports 6.2).
  // RtlGetVersion is the kernel's own and is unaffected by manifests.
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
  if (nt != nullptr) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(::GetProcAddress(nt, "RtlGetVersion")));
    if (fn != nullptr) {
      RTL_OSVERSIONINFOW vi{};
      vi.dwOSVersionInfoSize = sizeof(vi);
      if (fn(&vi) == 0) {
        char buf[64];
        ::snprintf(buf, sizeof(buf), "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion,
                   vi.dwBuildNumber);
        return buf;
      }
    }
  }
  return "unknown";
}

/**
 * AppContainer probe.
 *
 * ★ Follows the cgroup probe's approach: actually walk through "create -> get
 *   the SID -> delete" rather than just checking whether a function exists.
 *   The README's lesson is blunt -- the filesystem being there does not mean
 *   the limits can be applied. Likewise: userenv.dll exporting
 *   CreateAppContainerProfile does not mean this machine (policy-restricted,
 *   running in a container, disabled by GPO) can really create a profile.
 *
 * The probe has no side effects: it uses the dedicated name hx-probe-<pid>,
 * deletes it immediately on success, and never touches the user's existing
 * profiles.
 */
bool ProbeAppContainer(std::string* reason) {
  wchar_t name[64];
  ::swprintf(name, 64, L"hx-probe-%lu", static_cast<unsigned long>(::GetCurrentProcessId()));

  PSID sid = nullptr;
  HRESULT hr = ::CreateAppContainerProfile(name, name, L"hx capability probe", nullptr, 0, &sid);
  if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
    // A previous probe did not clean up (killed with -9, say). Delete and
    // retry rather than counting it as a failure.
    ::DeleteAppContainerProfile(name);
    hr = ::CreateAppContainerProfile(name, name, L"hx capability probe", nullptr, 0, &sid);
  }
  if (FAILED(hr) || sid == nullptr) {
    *reason = "CreateAppContainerProfile: " + win::ErrorMessage(static_cast<DWORD>(hr));
    return false;
  }
  ::FreeSid(sid);
  ::DeleteAppContainerProfile(name);
  return true;
}

/**
 * Job object probe.
 *
 * ★ Also "write it, then read it back". The lesson the model itself found in
 *   the README (verify pids.max by reading it back after writing -- "the file
 *   existing does not mean the controller was delegated") applies here word
 *   for word: SetInformationJobObject returning success does not mean the
 *   limit is actually attached.
 */
bool ProbeJobObjects(bool* nested) {
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return false;

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION set{};
  set.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  set.BasicLimitInformation.ActiveProcessLimit = 7;  // an arbitrary recognizable value
  bool ok = ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &set,
                                      sizeof(set)) != 0;
  if (ok) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION got{};
    ok = ::QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &got, sizeof(got),
                                     nullptr) != 0 &&
         got.BasicLimitInformation.ActiveProcessLimit == 7 &&
         (got.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) != 0;
  }
  ::CloseHandle(job);

  // Nested Jobs arrived in Win8 and are the prerequisite for "a child creating
  // its own Job still cannot escape". Without them, a process that knows it is
  // in a Job can get out via CREATE_BREAKAWAY_FROM_JOB.
  *nested = false;
  if (ok) {
    BOOL in_job = FALSE;
    if (::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) != 0) {
      // Getting an answer means the API is there; nesting support arrived in
      // 6.2, so probing the version number is enough
      using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
      HMODULE nt = ::GetModuleHandleW(L"ntdll.dll");
      auto fn = nt == nullptr ? nullptr
                              : reinterpret_cast<RtlGetVersionFn>(
                                    reinterpret_cast<void*>(::GetProcAddress(nt, "RtlGetVersion")));
      RTL_OSVERSIONINFOW vi{};
      vi.dwOSVersionInfoSize = sizeof(vi);
      if (fn != nullptr && fn(&vi) == 0) {
        *nested = (vi.dwMajorVersion > 6) || (vi.dwMajorVersion == 6 && vi.dwMinorVersion >= 2);
      }
    }
  }
  return ok;
}

/** ConPTY exists only from Windows 10 1809 (17763) on, and it is forkpty's
 *  only counterpart. */
bool ProbeConPty() {
  HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
  if (k32 == nullptr) return false;
  return ::GetProcAddress(k32, "CreatePseudoConsole") != nullptr;
}

}  // namespace

bool HasRealSandbox(const Caps& c) {
  // ★ The same statement as "no Landlock means no real sandbox" on the Linux
  //   side: AppContainer is this machine's only source of filesystem
  //   isolation. Job objects govern resources and the process tree, and stop
  //   nothing about `type %USERPROFILE%\.ssh\id_rsa`.
  return c.appcontainer;
}

Caps DetectCaps() {
  Caps c;
  c.kernel = ProbeVersion();
  c.appcontainer = ProbeAppContainer(&c.appcontainer_reason);
  c.job_objects = ProbeJobObjects(&c.nested_jobs);
  c.conpty = ProbeConPty();
  return c;
}

json CapsToJson(const Caps& c) {
  return json{
      {"proto", "hxp/0"},
      {"platform", "windows"},
      {"kernel", c.kernel},
      // ★ The key names deliberately follow the Linux side's semantic
      //   grouping rather than copying Win32 terminology: the host has always
      //   asked "is there filesystem isolation" and "is there a network
      //   restriction", not "landlock or appcontainer". Change the
      //   implementation and the host needs no change.
      {"filesystem", json{{"available", c.appcontainer},
                          {"backend", "appcontainer"},
                          {"reason", c.appcontainer_reason}}},
      {"network", json{{"available", c.appcontainer},
                       {"backend", "appcontainer-capabilities"},
                       // Without the internetClient capability WFP blocks UDP
                       // as well, so there is no UDP blind spot the way
                       // Landlock has one.
                       {"covers_udp", true}}},
      {"limits", json{{"available", c.job_objects},
                      {"backend", "job-object"},
                      {"nested_jobs", c.nested_jobs},
                      {"max_processes", c.job_objects},
                      {"max_memory", c.job_objects},
                      // The honest gaps: Job objects have no counterpart for these two
                      {"max_file_bytes", false},
                      {"max_open_files", false}}},
      {"pty", json{{"available", c.conpty}, {"backend", "conpty"}}},
      // The Linux-only entries are reported explicitly as not applicable
      // rather than omitted -- omitting them would make someone reading the
      // log think "it was not probed" instead of "it does not exist".
      {"landlock", json{{"available", false}, {"reason", "linux-only"}}},
      {"seccomp", json{{"available", false}, {"reason", "linux-only"}}},
      {"cgroup2", json{{"available", false}, {"reason", "linux-only"}}},
  };
}

}  // namespace hx
#endif  // _WIN32
