// The Windows definition of Confinement: a per-session unique AppContainer.
//
// ★ Why AppContainer is Landlock's counterpart rather than "something roughly
//   similar":
//
//   Landlock is an allow-list -- any path not granted in the ruleset is
//   denied, no blacklist is needed, and symlinks are judged on the resolved
//   path.
//
//   AppContainer is also an allow-list -- the process token carries a
//   **low-privilege package SID**, and access to any kernel object is allowed
//   only when the DACL explicitly grants that SID (or ALL APPLICATION
//   PACKAGES). The user profile directory grants neither by default, so
//   `type %USERPROFILE%\.ssh\id_rsa` is a flat ACCESS_DENIED -- the same
//   mechanism as `cat ~/.ssh/id_rsa` returning EACCES in the README, and
//   equally without needing a blacklist.
//
//   The network half is cleaner still: Landlock ABI 4 only covers TCP, with
//   UDP/DNS a blind spot, so the Linux side has to add a seccomp layer to
//   block AF_INET. AppContainer needs no such thing -- without the
//   internetClient capability, WFP blocks **all** outbound traffic in the
//   kernel, TCP and UDP alike. Here net:deny is one layer, not two stitched
//   together.
//
// ★ The cost has to be stated plainly: granting roots is implemented by
//   adding ACEs to real directories, which is a **persistent modification** of
//   the user's filesystem. So the ACEs grant only this session's unique
//   AppContainer SID, and are revoked when the Confinement is destroyed. If
//   the process is killed with -9 the ACEs are left behind -- see the notes on
//   kAceTag in the .cpp.
#pragma once
#ifdef _WIN32

#include <windows.h>

#include <string>
#include <vector>

#include "sandbox/confine.hpp"

namespace hx {

struct Confinement {
  PSID sid = nullptr;             // the AppContainer's package SID
  std::wstring profile_name;      // unique to this session, shaped like hx-<pid>-<tick>
  std::vector<PSID> cap_sids;     // granted capabilities (internetClient only under net:allow)
  std::vector<std::wstring> granted_paths;  // paths that got an ACE; revoked on destruction
  bool net_allowed = false;

  ~Confinement();
  Confinement() = default;
  Confinement(const Confinement&) = delete;
  Confinement& operator=(const Confinement&) = delete;
};

/**
 * Fill in the SECURITY_CAPABILITIES that CreateProcess uses.
 *
 * spawn_win.cpp / pty_win.cpp pass it to CreateProcess via
 * PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES -- this is the Windows form of
 * "drop your own privileges before execve".
 * The only difference from Landlock is timing: on Linux the child calls
 * restrict_self on itself, while on Windows the parent fixes the token at
 * process-creation time, leaving the child no window at all.
 */
bool FillSecurityCapabilities(const Confinement& c, SECURITY_CAPABILITIES* out);

}  // namespace hx

#endif  // _WIN32
