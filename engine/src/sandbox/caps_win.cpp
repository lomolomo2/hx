#ifdef _WIN32
#include "sandbox/caps.hpp"

#include <windows.h>

#include <userenv.h>

#include <cstdio>

#include "platform/win_util.hpp"

namespace hx {
namespace {

std::string ProbeVersion() {
  // GetVersionEx 对没写 manifest 的程序会撒谎（永远报 6.2）。
  // RtlGetVersion 是内核那一份，不受 manifest 影响。
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
 * AppContainer 探测。
 *
 * ★ 照 cgroup 探测的做法：真的走一遍「建 -> 拿到 SID -> 删」，而不是
 *   只看函数在不在。README 的教训很直白 —— 文件系统在不等于限额装得上。
 *   同理：userenv.dll 导出了 CreateAppContainerProfile，不等于这台机器
 *   （策略限制、容器里跑、被 GPO 关掉）真能建出 profile 来。
 *
 * 探测无副作用：用 hx-probe-<pid> 这个专属名字，成功后立刻删掉，
 * 绝不碰用户已有的 profile。
 */
bool ProbeAppContainer(std::string* reason) {
  wchar_t name[64];
  ::swprintf(name, 64, L"hx-probe-%lu", static_cast<unsigned long>(::GetCurrentProcessId()));

  PSID sid = nullptr;
  HRESULT hr = ::CreateAppContainerProfile(name, name, L"hx capability probe", nullptr, 0, &sid);
  if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
    // 上一次探测没清干净（比如被 kill -9）。删掉重来，不把它当失败。
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
 * Job 对象探测。
 *
 * ★ 同样是「写进去再读回来」。README 里模型自己发现的那条经验
 *   （写完 pids.max 要读回校验 ——「文件存在不等于控制器已下发」）
 *   在这里一字不改地适用：SetInformationJobObject 返回成功，
 *   不等于限额真的挂上了。
 */
bool ProbeJobObjects(bool* nested) {
  HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return false;

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION set{};
  set.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  set.BasicLimitInformation.ActiveProcessLimit = 7;  // 随便一个可辨认的值
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

  // 嵌套 Job 是 Win8 起的能力，也是「子进程自己建 Job 也逃不出去」的前提。
  // 没有它，一个知道自己在 Job 里的进程可以靠 CREATE_BREAKAWAY_FROM_JOB 跑掉。
  *nested = false;
  if (ok) {
    BOOL in_job = FALSE;
    if (::IsProcessInJob(::GetCurrentProcess(), nullptr, &in_job) != 0) {
      // 能问出来就说明 API 在；嵌套支持从 6.2 起，探测版本号即可
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

/** ConPTY 是 Windows 10 1809（17763）起才有的，这是 forkpty 的唯一对位。 */
bool ProbeConPty() {
  HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
  if (k32 == nullptr) return false;
  return ::GetProcAddress(k32, "CreatePseudoConsole") != nullptr;
}

}  // namespace

bool HasRealSandbox(const Caps& c) {
  // ★ 与 Linux 侧「没有 Landlock 就没有真沙箱」是同一句话：
  //   AppContainer 是这台机器上唯一的文件系统隔离来源。
  //   Job 对象管的是资源与进程树，挡不住 `type %USERPROFILE%\.ssh\id_rsa`。
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
      // ★ 键名故意沿用 Linux 侧的语义分组，而不是照搬 Win32 的叫法：
      //   宿主问的一直是「文件系统隔离有没有」「网络限制有没有」，
      //   不是「landlock 还是 appcontainer」。换了实现，宿主不用改。
      {"filesystem", json{{"available", c.appcontainer},
                          {"backend", "appcontainer"},
                          {"reason", c.appcontainer_reason}}},
      {"network", json{{"available", c.appcontainer},
                       {"backend", "appcontainer-capabilities"},
                       // 没有 internetClient 能力时 WFP 连 UDP 一起挡，
                       // 所以不像 Landlock 那样存在 UDP 盲区。
                       {"covers_udp", true}}},
      {"limits", json{{"available", c.job_objects},
                      {"backend", "job-object"},
                      {"nested_jobs", c.nested_jobs},
                      {"max_processes", c.job_objects},
                      {"max_memory", c.job_objects},
                      // 诚实的缺口：Job 对象没有这两项的对位
                      {"max_file_bytes", false},
                      {"max_open_files", false}}},
      {"pty", json{{"available", c.conpty}, {"backend", "conpty"}}},
      // Linux 专有的几项在这里明确报不适用，而不是省略 ——
      // 省略会让读日志的人以为「没探测」，而不是「不存在」。
      {"landlock", json{{"available", false}, {"reason", "linux-only"}}},
      {"seccomp", json{{"available", false}, {"reason", "linux-only"}}},
      {"cgroup2", json{{"available", false}, {"reason", "linux-only"}}},
  };
}

}  // namespace hx
#endif  // _WIN32
