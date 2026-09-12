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

// Both are defined below; forward-declared here so definitions do not have to
// be shuffled around to satisfy call order.
void RemoveOrphanAppContainerAces(const std::wstring& path);
PSID MakeCapabilitySid(WELL_KNOWN_SID_TYPE type);

/**
 * Add one inheritable ACE for a SID on a path.
 *
 * ★ Why GRANT_ACCESS + SetEntriesInAcl rather than assembling the ACL by
 *   hand: it places the new ACE in the correct position -- **after existing
 *   deny ACEs and before allow ACEs**. Get the order wrong by hand and a deny
 *   that should have taken effect is pre-empted by our allow, which punches a
 *   hole in the user's directory rather than granting the sandbox access.
 */
bool GrantOnPath(const std::wstring& path, PSID sid, DWORD access,
                 std::vector<std::string>* warnings) {
  // Clear orphan ACEs left by a previous crash before adding our own --
  // otherwise they pile up layer by layer.
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
  // An ACE on a directory must be inheritable, or only the directory itself is
  // accessible and every file inside is denied.
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

/** Revoke the ACE GrantOnPath added. Best-effort during destruction; failures
 *  are not reported. */
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
 * Clear **orphan** AppContainer ACEs from one object.
 *
 * ★ Why this is needed: on a clean exit ~Confinement revokes the ACEs it
 *   added, but when hxd is killed with -9 the destructor never runs at all and
 *   the ACEs stay behind in the user's directories. Across a development or
 *   crash loop they pile up layer by layer -- measured, two rounds of
 *   experiments left one each on C:\, C:\Users\<user>, AppData\Local and the
 *   workspace.
 *
 * ★ The test is that the **SID does not resolve to a name**: once the package
 *   profile has been deleted (by SweepStaleProfiles, or by the user), the SID
 *   is an ownerless orphan. Anything that still resolves to a name belongs to
 *   a genuinely installed application and is never touched.
 *
 * ★ Done only on **the handful of objects we are about to touch ourselves**
 *   (roots / tmpdir / volume roots), never by scanning the disk. The narrower
 *   the scope the safer, and there is no performance problem either.
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

    // Only consider package SIDs (S-1-15-2-...)
    SID_IDENTIFIER_AUTHORITY* auth = ::GetSidIdentifierAuthority(s);
    if (auth == nullptr || auth->Value[5] != 15) continue;
    const UCHAR count = *::GetSidSubAuthorityCount(s);
    if (count < 1 || *::GetSidSubAuthority(s, 0) != 2) continue;

    // ★ Also require that **the shape matches exactly what we stamp** before
    //   touching it.
    //
    //   "The SID does not resolve to a name" is not enough on its own: an
    //   application the user once uninstalled can leave an orphan ACE here
    //   too, and that is not ours -- deleting it would be touching someone
    //   else's data. The mask and inheritance flags of the ACEs we stamp are
    //   fixed (see GrantOnPath), so anything that does not match was not left
    //   by us and is skipped.
    //   The kOurTraverse branch is kept in order to clean up residue from the
    //   now-abandoned approach of stamping an ACE on the volume root per
    //   session.
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

    // Resolves to a name = the package still exists = it has an owner; leave it
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
 * The volume root a path lives on ("C:\" / "\\\\server\\share\\"). Empty when
 * it cannot be determined.
 */
std::wstring VolumeRootOf(const std::wstring& path) {
  wchar_t buf[MAX_PATH]{};
  if (::GetVolumePathNameW(path.c_str(), buf, MAX_PATH) == 0) return std::wstring();
  return buf;
}

/**
 * Volume roots are **checked, never modified**; if the grant is missing, tell
 * the user honestly which command to run.
 *
 * ★ Why not just stamp an ACE the way roots do -- a boundary settled after
 *   being burned twice.
 *
 *   1. Walking up the ancestor chain: a disaster. SetNamedSecurityInfoW
 *      applied to a directory **recomputes inheritance across the entire
 *      subtree**, so calling it on C:\Users\<user> means traversing the whole
 *      user profile. Measured: ten minutes without returning, and killing it
 *      partway leaves ACEs behind in the user's directory.
 *
 *   2. Backing off to writing only the volume root: still no good. Measured,
 *      a single `icacls C:\ /grant ...` takes **16 seconds** (the children of
 *      C:\ take part in propagation too). A session would grant at the start
 *      and revoke at the end, so **every session.open would cost 30-odd extra
 *      seconds** for nothing. Paying that on every session, for a grant that
 *      is static and only needed once per machine, is plainly a bad trade.
 *
 *   So instead: check whether the volume root lets an AppContainer traverse
 *   it, and if not emit a warning containing the **exact fix command** while
 *   opening the session as usual. Whether to grant it is the user's decision
 *   -- it modifies the system drive's ACL, and an agent harness has no
 *   business doing that quietly on every run.
 *
 *   The consequence of its absence is exactly one thing: `dir` /
 *   `Get-ChildItem` cannot list directories inside the sandbox (reading files,
 *   writing files and running programs are unaffected). The warning says so.
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
    // ★ SYNCHRONIZE must not be omitted. With only (X,RA), dir still says
    //   "Access is denied" -- opening a handle synchronously requires it. The
    //   capability ACE Windows itself ships on C:\ is (S,RD,X,RA).
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

/** Allocate a well-known capability SID. The caller is responsible for FreeSid. */
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
 * System read-only paths need no grant from us on Windows -- C:\Windows,
 * Program Files and the like ship granting ALL APPLICATION PACKAGES read +
 * execute (which is how Store apps run at all).
 * This spot-checks one of them, and warns honestly when the check fails rather
 * than quietly assuming it holds.
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
 * Clear AppContainer profiles a previous run never got to delete.
 *
 * ★ Why this is needed: on a clean exit ~Confinement deletes its own profile,
 *   but when hxd is killed with -9 the destructor never runs and the profile
 *   stays on the system. Across a development or crash loop they accumulate
 *   without bound -- measured, a few dozen test runs had built up more than
 *   twenty.
 *
 *   The rollout's answer to this is append-only plus one write per record (a
 *   hard kill leaves no half line). There is no equivalent here, so the
 *   fallback is the next best thing: **sweep once at startup**.
 *
 *   The test is whether the pid embedded in the profile name is still alive.
 *   If a pid has been reused we merely "skip deleting" and never delete
 *   someone else's -- the failure direction is the safe one. A profile another
 *   hxd is currently using is skipped for the same reason.
 *
 * ★ What gets enumerated is the **registry**, not %LOCALAPPDATA%\Packages.
 *   The first version scanned the directory, which made this sweeper a no-op:
 *   CreateAppContainerProfile only guarantees registering a moniker under
 *   Mappings, and that directory is created only when the profile is actually
 *   used. Measured, after seven consecutive sessions there was **not one
 *   directory** under Packages while all seven entries sat in the registry.
 *   DeleteAppContainerProfile cleans both, so enumerating the registry is the
 *   complete side.
 */
void SweepStaleProfiles() {
  static constexpr wchar_t kMappings[] =
      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\"
      L"CurrentVersion\\AppContainer\\Mappings";

  HKEY root = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kMappings, 0, KEY_READ, &root) != ERROR_SUCCESS) return;

  // Collect first, delete after: deleting while enumerating shifts every later
  // index down and misses half of them.
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

    // hx-<pid>-<tick> (a session) or hx-probe-<pid> (capability probing)
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
      if (running) continue;  // another hxd may be using it; leave it alone
    }
    ::DeleteAppContainerProfile(w.c_str());
  }
}

}  // namespace

Confinement::~Confinement() {
  // ★ Revoke the ACEs before deleting the profile.
  //   The other way round, the SID no longer has a profile backing it;
  //   SetEntriesInAcl still writes happily, but what it leaves is an orphan ACE
  //   pointing at a package that does not exist, which the user sees in
  //   Explorer as a garbled entry that resolves to no name.
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
  // Capabilities points at an array the caller owns; this borrows storage from
  // the Confinement, whose lifetime is guaranteed to cover the whole
  // CreateProcess call.
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

  // Sweep away profiles left by a previous crash or hard kill before creating
  // our own. Done once, at the first BuildConfinement, so repeatedly
  // tightening via policy.set does not re-sweep.
  static bool swept = [] {
    SweepStaleProfiles();
    return true;
  }();
  (void)swept;

  auto conf = std::make_shared<Confinement>();

  // One unique AppContainer per session.
  // ★ Never reuse a fixed name: if two concurrent sessions shared a SID, the
  //   ACE session A stamps for its own root would let session B read it too --
  //   two sandboxes punching holes in each other, and invisibly at that.
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

  // ---- network ----
  //
  // ★ The cleanest correspondence in the entire port.
  //   Linux needs two layers, Landlock(TCP) + seccomp(AF_INET, to cover
  //   UDP/DNS); Windows needs only "do not grant the internetClient
  //   capability", and WFP blocks TCP and UDP alike in the kernel. There is no
  //   UDP blind spot, so there is no second layer either.
  if (p.net == NetMode::kAllow) {
    PSID net_cap = MakeCapabilitySid(WinCapabilityInternetClientSid);
    if (net_cap != nullptr) {
      conf->cap_sids.push_back(net_cap);
      conf->net_allowed = true;
    } else {
      out.warnings.push_back("cannot derive internetClient capability: network will be denied");
    }
  }
  // Under net:deny no capability is granted at all -- offline is the default,
  // and it is kernel-enforced.
  out.net_enforced = (p.net == NetMode::kDeny);

  CheckSystemReadable(&out.warnings);

  // ---- filesystem ----
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

  // The session-private tmp is always writable. Without it every tool that
  // needs a temp file (compilers, bundlers) fails for no visible reason -- the
  // same reason the Linux side grants TMPDIR.
  if (p.sandbox == SandboxMode::kWorkspaceWrite && !p.tmpdir.empty()) {
    const std::wstring w = win::Widen(p.tmpdir);
    if (GrantOnPath(w, sid, kWrite, &out.warnings)) conf->granted_paths.push_back(w);
  }

  // ★ Volume roots are checked, never modified: when the grant is missing,
  //   emit a warning carrying the fix command. See CheckVolumeRoots -- writing
  //   the volume root ACL per session costs 30+ seconds, which is a bad trade.
  CheckVolumeRoots(p, &out.warnings);

  out.conf = std::move(conf);
  out.enforced = true;
  return out;
}

}  // namespace hx
#endif  // _WIN32
