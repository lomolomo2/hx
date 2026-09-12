#ifdef _WIN32
#include "sandbox/confine_win.hpp"

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "platform/platform.hpp"
#include "platform/win_util.hpp"

namespace hx {
namespace {

// 都定义在下面；这里前置声明，免得为了调用顺序把定义挪来挪去。
void RemoveOrphanAppContainerAces(const std::wstring& path);
PSID MakeCapabilitySid(WELL_KNOWN_SID_TYPE type);

/**
 * 在一个路径上给 SID 加一条可继承的 ACE。
 *
 * ★ 用 GRANT_ACCESS + SetEntriesInAcl（而不是自己拼 ACL）的原因：
 *   它会把新 ACE 放在**已有 deny ACE 之后、allow ACE 之前**的正确位置。
 *   手拼顺序错了，一条本该生效的 deny 会被我们的 allow 抢先匹配掉 ——
 *   那是在给用户的目录开洞，而不是给沙箱授权。
 */
bool GrantOnPath(const std::wstring& path, PSID sid, DWORD access,
                 std::vector<std::string>* warnings) {
  // 先把上一次崩溃留下的孤儿 ACE 清掉，再加自己的 —— 否则它们会一层层堆积。
  RemoveOrphanAppContainerAces(path);

  PACL old_dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  DWORD rc = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &old_dacl, nullptr, &sd);
  if (rc != ERROR_SUCCESS) {
    warnings->push_back("cannot read DACL: " + win::Narrow(path) + ": " + win::ErrorMessage(rc));
    return false;
  }
  std::unique_ptr<void, decltype(&::LocalFree)> sd_guard(sd, &::LocalFree);

  EXPLICIT_ACCESS_W ea{};
  ea.grfAccessPermissions = access;
  ea.grfAccessMode = GRANT_ACCESS;
  // 目录上的 ACE 必须可继承，否则只有目录本身能访问，里面的文件一概拒绝。
  ea.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);

  PACL new_dacl = nullptr;
  rc = ::SetEntriesInAclW(1, &ea, old_dacl, &new_dacl);
  if (rc != ERROR_SUCCESS) {
    warnings->push_back("SetEntriesInAcl failed: " + win::Narrow(path) + ": " +
                        win::ErrorMessage(rc));
    return false;
  }
  std::unique_ptr<ACL, decltype(&::LocalFree)> acl_guard(new_dacl, &::LocalFree);

  rc = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
  if (rc != ERROR_SUCCESS) {
    warnings->push_back("cannot write DACL: " + win::Narrow(path) + ": " + win::ErrorMessage(rc));
    return false;
  }
  return true;
}

/** 撤销 GrantOnPath 加的那条 ACE。析构时尽力而为，失败不报错。 */
void RevokeOnPath(const std::wstring& path, PSID sid) {
  PACL old_dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &old_dacl, nullptr, &sd) != ERROR_SUCCESS) {
    return;
  }
  std::unique_ptr<void, decltype(&::LocalFree)> sd_guard(sd, &::LocalFree);

  EXPLICIT_ACCESS_W ea{};
  ea.grfAccessMode = REVOKE_ACCESS;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
  ea.Trustee.ptstrName = static_cast<LPWSTR>(sid);

  PACL new_dacl = nullptr;
  if (::SetEntriesInAclW(1, &ea, old_dacl, &new_dacl) != ERROR_SUCCESS) return;
  std::unique_ptr<ACL, decltype(&::LocalFree)> acl_guard(new_dacl, &::LocalFree);
  ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
}

/**
 * 清掉一个对象上**孤儿**的 AppContainer ACE。
 *
 * ★ 为什么需要：正常退出时 ~Confinement 会把自己打的 ACE 撤掉，
 *   但 hxd 被 kill -9 时析构函数根本不会跑，ACE 就留在用户的目录上了。
 *   开发/崩溃循环里这东西会一层层堆积 —— 实测两轮实验就在 C:\、
 *   C:\Users\<user>、AppData\Local、工作区上各留下一条。
 *
 * ★ 判据是 **SID 解析不出名字**：package profile 已经被删掉（SweepStaleProfiles
 *   会删，或者用户自己删了），SID 就成了无主的孤儿。还能解析出名字的
 *   属于真实安装的应用，一律不碰。
 *
 * ★ 只在**我们自己正要动的那几个对象**上做（roots / tmpdir / 卷根），
 *   不扫盘。范围越小越安全，也不会有性能问题。
 */
void RemoveOrphanAppContainerAces(const std::wstring& path) {
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
    return;
  }
  std::unique_ptr<void, decltype(&::LocalFree)> sd_guard(sd, &::LocalFree);
  if (dacl == nullptr) return;

  std::vector<EXPLICIT_ACCESS_W> revokes;
  std::vector<PSID> orphan_sids;
  for (WORD i = 0; i < dacl->AceCount; ++i) {
    LPVOID ace = nullptr;
    if (::GetAce(dacl, i, &ace) == 0) continue;
    auto* hdr = static_cast<ACE_HEADER*>(ace);
    if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
    PSID s = reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart);
    if (::IsValidSid(s) == 0) continue;

    // 只看 package SID（S-1-15-2-...）
    SID_IDENTIFIER_AUTHORITY* auth = ::GetSidIdentifierAuthority(s);
    if (auth == nullptr || auth->Value[5] != 15) continue;
    const UCHAR count = *::GetSidSubAuthorityCount(s);
    if (count < 1 || *::GetSidSubAuthority(s, 0) != 2) continue;

    // ★ 还要求「形状和我们自己打的一模一样」才动它。
    //
    //   光看"SID 解析不出名字"是不够的：用户卸载过的某个应用也可能在这里
    //   留下孤儿 ACE，那不是我们的东西，删了就是在动别人的数据。
    //   我们打的 ACE 掩码和继承标志都是定死的（见 GrantOnPath）——
    //   对不上就不是我们留下的，一律跳过。
    //   kOurTraverse 那一支留着，是为了清掉早先"每会话给卷根打 ACE"
    //   那个已经废弃的做法留下的残留。
    const ACCESS_MASK mask = static_cast<ACCESS_ALLOWED_ACE*>(ace)->Mask;
    const BYTE inherit = static_cast<BYTE>(hdr->AceFlags &
                                           (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE));
    constexpr ACCESS_MASK kOurRead = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    constexpr ACCESS_MASK kOurWrite = FILE_ALL_ACCESS;
    constexpr ACCESS_MASK kOurTraverse = FILE_READ_ATTRIBUTES | FILE_EXECUTE | SYNCHRONIZE;

    const bool ours =
        (inherit == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) &&
         (mask == kOurRead || mask == kOurWrite)) ||
        (inherit == 0 && mask == kOurTraverse);
    if (!ours) continue;

    // 解析得出名字 = package 还在 = 有主，别动
    wchar_t name[256]{}, domain[256]{};
    DWORD nlen = 256, dlen = 256;
    SID_NAME_USE use{};
    if (::LookupAccountSidW(nullptr, s, name, &nlen, domain, &dlen, &use) != 0) continue;

    orphan_sids.push_back(s);
  }
  if (orphan_sids.empty()) return;

  for (PSID s : orphan_sids) {
    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessMode = REVOKE_ACCESS;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(s);
    revokes.push_back(ea);
  }

  PACL new_dacl = nullptr;
  if (::SetEntriesInAclW(static_cast<ULONG>(revokes.size()), revokes.data(), dacl, &new_dacl) !=
      ERROR_SUCCESS) {
    return;
  }
  std::unique_ptr<ACL, decltype(&::LocalFree)> acl_guard(new_dacl, &::LocalFree);
  ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
}

/**
 * 取一个路径所在的卷根（"C:\" / "\\\\server\\share\\"）。取不到返回空。
 */
std::wstring VolumeRootOf(const std::wstring& path) {
  wchar_t buf[MAX_PATH]{};
  if (::GetVolumePathNameW(path.c_str(), buf, MAX_PATH) == 0) return std::wstring();
  return buf;
}

/**
 * 卷根**只检查、不修改**，缺了就如实告诉用户该跑哪条命令。
 *
 * ★ 为什么不像 roots 那样直接打 ACE —— 这是两次踩坑之后定下来的边界。
 *
 *   ① 逐级爬父链：灾难。SetNamedSecurityInfoW 作用在一个目录上会
 *      **向整个子树重算继承**，在 C:\Users\<user> 上调它等于遍历整个
 *      用户 profile，实测十分钟没回来，中途被杀还会在用户目录上留 ACE。
 *
 *   ② 退一步只写卷根：还是不行。实测 `icacls C:\ /grant ...` 单次就要
 *      **16 秒**（C:\ 的子项也要参与传播）。而会话开始要 grant、结束要
 *      revoke，于是**每个 session.open 平白多出 30 多秒**。
 *      为了一个静态的、全机器一次就够的授权，让每次会话都付这个代价，
 *      是明显划不来的。
 *
 *   所以改成：检查卷根允不允许 AppContainer 穿过去；不允许就发一条
 *   带**确切修复命令**的 warning，会话照常开。授权与否是用户的决定 ——
 *   那是改系统盘 ACL，不该由一个 agent harness 每次运行时偷偷做。
 *
 *   缺了它的后果只有一个：沙箱里 `dir` / `Get-ChildItem` 列不出目录
 *   （读文件、写文件、跑程序都不受影响）。warning 里写清楚了。
 */
void CheckVolumeRoots(const Policy& p, std::vector<std::string>* warnings) {
  std::vector<std::wstring> volumes;
  auto add_volume = [&](const std::string& path) {
    if (path.empty()) return;
    const std::wstring vol = VolumeRootOf(win::Widen(path));
    if (vol.empty()) return;
    for (const auto& v : volumes) {
      if (::CompareStringOrdinal(v.c_str(), -1, vol.c_str(), -1, TRUE) == CSTR_EQUAL) return;
    }
    volumes.push_back(vol);
  };
  for (const auto& root : p.roots) add_volume(root);
  for (const auto& extra : p.extra_read_paths) add_volume(extra);
  add_volume(p.tmpdir);

  PSID all_packages = MakeCapabilitySid(WinBuiltinAnyPackageSid);
  if (all_packages == nullptr) return;

  for (const auto& vol : volumes) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(vol.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
      continue;
    }
    std::unique_ptr<void, decltype(&::LocalFree)> sd_guard(sd, &::LocalFree);

    bool ok = false;
    // ★ SYNCHRONIZE 不能漏。只给 (X,RA) 的话 dir 照样 "Access is denied" ——
    //   同步打开句柄需要它。系统自带挂在 C:\ 上的那条能力 ACE 就是 (S,RD,X,RA)。
    constexpr ACCESS_MASK kNeeded = FILE_READ_ATTRIBUTES | FILE_EXECUTE | SYNCHRONIZE;
    if (dacl != nullptr) {
      for (WORD i = 0; i < dacl->AceCount; ++i) {
        LPVOID ace = nullptr;
        if (::GetAce(dacl, i, &ace) == 0) continue;
        if (static_cast<ACE_HEADER*>(ace)->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto* aa = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (::EqualSid(reinterpret_cast<PSID>(&aa->SidStart), all_packages) == 0) continue;
        if ((aa->Mask & kNeeded) == kNeeded) {
          ok = true;
          break;
        }
      }
    }
    if (!ok) {
      warnings->push_back(
          "volume root " + win::Narrow(vol) +
          " does not let AppContainers traverse it: directory listing (dir / Get-ChildItem) will "
          "fail inside the sandbox (reads, writes and exec are unaffected). One-time fix, "
          "elevated: icacls " + win::Narrow(vol) + " /grant \"*S-1-15-2-1:(S,X,RA)\"");
    }
  }
  ::LocalFree(all_packages);
}

/** 分配一个 well-known 能力 SID。调用方负责 FreeSid。 */
PSID MakeCapabilitySid(WELL_KNOWN_SID_TYPE type) {
  DWORD n = SECURITY_MAX_SID_SIZE;
  auto* sid = static_cast<PSID>(::LocalAlloc(LPTR, n));
  if (sid == nullptr) return nullptr;
  if (::CreateWellKnownSid(type, nullptr, sid, &n) == 0) {
    ::LocalFree(sid);
    return nullptr;
  }
  return sid;
}

/**
 * 系统只读路径在 Windows 上不需要我们授权 —— C:\Windows、Program Files 等
 * 出厂就给了 ALL APPLICATION PACKAGES 读+执行（Store 应用就靠这个跑）。
 * 这里只抽查一条，抽查不过就如实警告，而不是默默假设它成立。
 */
void CheckSystemReadable(std::vector<std::string>* warnings) {
  std::string sysdir;
  if (!platform::GetEnv("SystemRoot", &sysdir)) return;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (::GetNamedSecurityInfoW(win::Widen(sysdir).c_str(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                              &sd) != ERROR_SUCCESS) {
    return;
  }
  std::unique_ptr<void, decltype(&::LocalFree)> sd_guard(sd, &::LocalFree);
  PSID all_packages = MakeCapabilitySid(WinBuiltinAnyPackageSid);
  if (all_packages == nullptr || dacl == nullptr) return;
  bool found = false;
  for (WORD i = 0; i < dacl->AceCount; ++i) {
    LPVOID ace = nullptr;
    if (::GetAce(dacl, i, &ace) == 0) continue;
    auto* hdr = static_cast<ACE_HEADER*>(ace);
    if (hdr->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
    auto* aa = static_cast<ACCESS_ALLOWED_ACE*>(ace);
    if (::EqualSid(reinterpret_cast<PSID>(&aa->SidStart), all_packages) != 0) {
      found = true;
      break;
    }
  }
  ::LocalFree(all_packages);
  if (!found) {
    warnings->push_back(
        "%SystemRoot% does not grant ALL APPLICATION PACKAGES: sandboxed processes may be "
        "unable to run system executables");
  }
}

/**
 * 清掉上一次没来得及删的 AppContainer profile。
 *
 * ★ 为什么需要它：正常退出时 ~Confinement 会删掉自己那个 profile，
 *   但 hxd 被 kill -9 时析构函数根本不会跑，profile 就留在系统里。
 *   开发或崩溃循环里这东西会无限堆积 —— 实测跑了几十轮测试之后攒了二十多个。
 *
 *   rollout 那边的对策是 append-only + 单次写（被强杀也不留半行），
 *   这里没有等价物，所以退而求其次：**启动时扫一遍**。
 *
 *   判据是 profile 名字里的 pid 还在不在。pid 被复用的话我们只会
 *   "跳过删除"，不会误删别人的东西 —— 失败方向是安全的那一侧。
 *   另一个 hxd 正在用的 profile 同理会被跳过。
 *
 * ★ 枚举的是**注册表**，不是 %LOCALAPPDATA%\Packages。
 *   第一版扫目录，结果这个清扫器等于没写：CreateAppContainerProfile 只保证
 *   在 Mappings 下登记一条 moniker，那个目录是 profile 真被用到时才建的。
 *   实测连跑七个会话之后，Packages 下**一个目录都没有**，注册表里七条全在。
 *   DeleteAppContainerProfile 两边都清，所以按注册表枚举才是完整的那一侧。
 */
void SweepStaleProfiles() {
  static constexpr wchar_t kMappings[] =
      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\"
      L"CurrentVersion\\AppContainer\\Mappings";

  HKEY root = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kMappings, 0, KEY_READ, &root) != ERROR_SUCCESS) return;

  // 先收集再删：边枚举边删会让后面的索引整体前移，漏掉一半。
  std::vector<std::wstring> monikers;
  for (DWORD i = 0;; ++i) {
    wchar_t sub[256]{};
    DWORD sub_len = static_cast<DWORD>(std::size(sub));
    const LSTATUS st = ::RegEnumKeyExW(root, i, sub, &sub_len, nullptr, nullptr, nullptr, nullptr);
    if (st == ERROR_NO_MORE_ITEMS) break;
    if (st != ERROR_SUCCESS) continue;

    wchar_t moniker[256]{};
    DWORD bytes = sizeof(moniker);
    if (::RegGetValueW(root, sub, L"Moniker", RRF_RT_REG_SZ, nullptr, moniker, &bytes) !=
        ERROR_SUCCESS) {
      continue;
    }
    monikers.emplace_back(moniker);
  }
  ::RegCloseKey(root);

  for (const std::wstring& w : monikers) {
    const std::string name = win::Narrow(w);
    if (name.rfind("hx-", 0) != 0) continue;

    // hx-<pid>-<tick>（会话）或 hx-probe-<pid>（能力探测）
    unsigned long pid = 0;
    if (name.rfind("hx-probe-", 0) == 0) {
      pid = std::strtoul(name.c_str() + 9, nullptr, 10);
    } else {
      pid = std::strtoul(name.c_str() + 3, nullptr, 10);
    }
    if (pid == 0) continue;

    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (h != nullptr) {
      const bool running = ::WaitForSingleObject(h, 0) != WAIT_OBJECT_0;
      ::CloseHandle(h);
      if (running) continue;  // 可能是另一个 hxd 正在用，别动
    }
    ::DeleteAppContainerProfile(w.c_str());
  }
}

}  // namespace

Confinement::~Confinement() {
  // ★ 先撤 ACE 再删 profile。
  //   反过来的话 SID 已经没有 profile 背书，SetEntriesInAcl 仍然能写进去，
  //   但留下的是一条指向不存在 package 的孤儿 ACE，用户在资源管理器里
  //   会看到一个解析不出名字的乱码条目。
  for (const auto& p : granted_paths) {
    if (sid != nullptr) RevokeOnPath(p, sid);
  }
  for (PSID c : cap_sids) {
    if (c != nullptr) ::LocalFree(c);
  }
  if (!profile_name.empty()) ::DeleteAppContainerProfile(profile_name.c_str());
  if (sid != nullptr) ::FreeSid(sid);
}

bool FillSecurityCapabilities(const Confinement& c, SECURITY_CAPABILITIES* out) {
  if (c.sid == nullptr) return false;
  ::ZeroMemory(out, sizeof(*out));
  out->AppContainerSid = c.sid;
  out->CapabilityCount = static_cast<DWORD>(c.cap_sids.size());
  // Capabilities 指向调用方持有的数组；这里借用 Confinement 里的存储，
  // 生命周期由 Confinement 保证覆盖整个 CreateProcess 调用。
  static thread_local std::vector<SID_AND_ATTRIBUTES> attrs;
  attrs.clear();
  for (PSID s : c.cap_sids) {
    SID_AND_ATTRIBUTES sa{};
    sa.Sid = s;
    sa.Attributes = SE_GROUP_ENABLED;
    attrs.push_back(sa);
  }
  out->Capabilities = attrs.empty() ? nullptr : attrs.data();
  return true;
}

RulesetBuild BuildConfinement(const Policy& p, const Caps& caps) {
  RulesetBuild out;

  if (p.sandbox == SandboxMode::kDangerFullAccess) {
    out.warnings.push_back("sandbox disabled: danger-full-access requested");
    return out;
  }
  if (!caps.appcontainer) {
    out.warnings.push_back(
        "AppContainer unavailable on this system: NO filesystem isolation is in effect");
    return out;
  }

  // 先把上一次崩溃/被强杀留下的 profile 扫掉，再建自己的。
  // 只在第一次 BuildConfinement 时做一次，policy.set 反复收紧时不重复扫。
  static bool swept = [] {
    SweepStaleProfiles();
    return true;
  }();
  (void)swept;

  auto conf = std::make_shared<Confinement>();

  // 每会话一个唯一的 AppContainer。
  // ★ 不复用固定名字：两个并发会话共用一个 SID 的话，A 会话为自己的 root
  //   打的 ACE 会让 B 会话也能读 —— 两个沙箱互相开洞，而且谁都看不出来。
  wchar_t name[64];
  ::swprintf(name, 64, L"hx-%lu-%llx", static_cast<unsigned long>(::GetCurrentProcessId()),
             static_cast<unsigned long long>(::GetTickCount64()));
  conf->profile_name = name;

  PSID sid = nullptr;
  HRESULT hr = ::CreateAppContainerProfile(name, name, L"hx sandboxed cell", nullptr, 0, &sid);
  if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
    hr = ::DeriveAppContainerSidFromAppContainerName(name, &sid);
  }
  if (FAILED(hr) || sid == nullptr) {
    out.warnings.push_back("CreateAppContainerProfile failed: " +
                           win::ErrorMessage(static_cast<DWORD>(hr)) +
                           " - NO filesystem isolation is in effect");
    return out;
  }
  conf->sid = sid;

  // ---- 网络 ----
  //
  // ★ 这是整个移植里最干净的一处对位。
  //   Linux 需要 Landlock(TCP) + seccomp(AF_INET，为了覆盖 UDP/DNS) 两层；
  //   Windows 只需要「不授予 internetClient 能力」，WFP 在内核里
  //   把 TCP 和 UDP 一起挡掉。没有 UDP 盲区，所以也不需要第二层。
  if (p.net == NetMode::kAllow) {
    PSID net_cap = MakeCapabilitySid(WinCapabilityInternetClientSid);
    if (net_cap != nullptr) {
      conf->cap_sids.push_back(net_cap);
      conf->net_allowed = true;
    } else {
      out.warnings.push_back("cannot derive internetClient capability: network will be denied");
    }
  }
  // net:deny 时什么能力都不给 —— 默认就是断网，且是内核强制的。
  out.net_enforced = (p.net == NetMode::kDeny);

  CheckSystemReadable(&out.warnings);

  // ---- 文件系统 ----
  constexpr DWORD kRead = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
  constexpr DWORD kWrite = FILE_ALL_ACCESS;

  for (const auto& extra : p.extra_read_paths) {
    const std::wstring w = win::Widen(extra);
    if (GrantOnPath(w, sid, kRead, &out.warnings)) {
      conf->granted_paths.push_back(w);
    } else {
      out.warnings.push_back("extra read path not granted: " + extra);
    }
  }

  const DWORD root_access = (p.sandbox == SandboxMode::kWorkspaceWrite) ? kWrite : kRead;
  for (const auto& root : p.roots) {
    const std::wstring w = win::Widen(root);
    if (GrantOnPath(w, sid, root_access, &out.warnings)) {
      conf->granted_paths.push_back(w);
    } else {
      out.warnings.push_back("root not granted: " + root);
    }
  }

  // 会话私有 tmp 永远可写。少了它，凡是要落临时文件的工具（编译器、打包器）
  // 都会莫名其妙地失败 —— 与 Linux 侧给 TMPDIR 授权是同一个理由。
  if (p.sandbox == SandboxMode::kWorkspaceWrite && !p.tmpdir.empty()) {
    const std::wstring w = win::Widen(p.tmpdir);
    if (GrantOnPath(w, sid, kWrite, &out.warnings)) conf->granted_paths.push_back(w);
  }

  // ★ 卷根只检查不修改：缺授权时发一条带修复命令的 warning。
  //   详见 CheckVolumeRoots —— 每会话去写卷根 ACL 要 30+ 秒，划不来。
  CheckVolumeRoots(p, &out.warnings);

  out.conf = std::move(conf);
  out.enforced = true;
  return out;
}

}  // namespace hx
#endif  // _WIN32
