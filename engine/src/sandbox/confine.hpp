// The confinement carrier -- the platform seam of the sandbox layer.
//
// "Kernel enforcement" takes completely different shapes on the two
// platforms, but **the contract is the same**:
//
//   Linux    A Landlock ruleset fd. The parent builds it; after fork the child
//            calls landlock_restrict_self(), then execve.
//            An allow-list: not granted means denied.
//
//   Windows  An AppContainer. The parent derives a per-session unique
//            AppContainer SID, stamps ACEs on the roots, and drops the token
//            in at CreateProcess time via
//            PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES.
//            Also an allow-list: an AppContainer process can only reach
//            objects whose DACL explicitly grants its SID (or ALL APPLICATION
//            PACKAGES).
//
// ★ The invariant shared by both sides must be preserved: **if it cannot be
//   applied, do not execute**. When enforced == false and the policy is not
//   danger-full-access, the caller must refuse to start the process and never
//   degrade to running unprotected.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sandbox/caps.hpp"
#include "sandbox/policy.hpp"

namespace hx {

/**
 * The platform-private confinement object.
 *
 * Deliberately only forward-declared here: engine.cpp has no business knowing
 * whether it is an fd or a SID. It is defined in confine_posix.hpp /
 * confine_win.hpp, and only the matching platform's spawn includes it.
 */
struct Confinement;

struct RulesetBuild {
  /** Null = not applied (danger-full-access, or the kernel does not support it
   *  and that is already explained in warnings). */
  std::shared_ptr<Confinement> conf;
  bool enforced = false;              // whether there will really be kernel enforcement
  bool net_enforced = false;          // whether the network restriction really takes effect
  std::vector<std::string> warnings;  // degradation notes; must be reported honestly to the host
};

/** Built in the parent. The returned object can be reused across spawns. */
RulesetBuild BuildConfinement(const Policy& p, const Caps& caps);

}  // namespace hx
